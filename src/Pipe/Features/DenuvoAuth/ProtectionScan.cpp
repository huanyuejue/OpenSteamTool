#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"

#include "OSTPlatform/include/ByteSearch.h"
#include "OSTPlatform/include/PE.h"
#include "OSTPlatform/include/Process.h"
#include "Utils/Logging/Log.h"
#include "Utils/Support/Stopwatch.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace PipeManager::DenuvoAuth {
namespace {

    struct ModuleCandidate {
        std::string path;
        std::filesystem::path nativePath;
        uint32 size = 0;
        bool executable = false;
        bool inGameTree = false;
        size_t order = 0;

        std::string DebugString() const {
            return std::format("path={} size={} executable={} inGameTree={} order={}",
                path, size, executable, inGameTree, order);
        }
    };

    struct DetectionMatch {
        DetectionMethod method = DetectionMethod::None;
        std::string sectionName;
        uint32 entryPointRva = 0;
        uint32 matchRva = 0;
        size_t matchRawOffset = 0;
    };

    constexpr std::array<std::string_view, 5> kLegacyDenuvoSections = {
        ".arch",
        ".srdata",
        ".xpdata",
        ".xdata",
        ".xtls",
    };

    constexpr std::array<uint8_t, 6> kLegacyDenuvoString = {
        'D', 'E', 'N', 'U', 'V', 'O',
    };

    // High-version quick path: only scan the section containing the entry point.
    constexpr std::array<uint8_t, 10> kDenuvoOepPattern = {
        0x48, 0xB9, 0x44, 0x4F, 0x44, 0x45, 0x4E, 0x55, 0x56, 0x4F,
    };

    // Packed Denuvo game modules are expected to be large; this avoids most
    // small runtime DLLs before any PE parsing or disk reads.
    constexpr uint32 kMinPackedModuleBytes = 80u * 1024u * 1024u;
    constexpr size_t kLegacyScanChunkBytes = 8ull * 1024ull * 1024ull;
    constexpr size_t kOepScanChunkBytes = 8ull * 1024ull * 1024ull;

    // 结构兜底的尺寸门槛：太小的 RWX 段是正常的 JIT / 跳板代码，不参与判定。
    constexpr uint32 kProtectorBlobMinBytes = 4u * 1024u * 1024u;
    // 熵达到这个值本身就足以判定，不需要段名佐证：保护壳在盘上是加密态，
    // 实测 .text 6.64、.ecode 6.63、.didata 6.72 都远高于普通代码段。
    constexpr double kProtectorBlobEncryptedEntropy = 6.0;
    constexpr size_t kProtectorBlobEntropySampleBytes = 8ull * 1024ull * 1024ull;

    double BytesToMiB(uint64 bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

    double SectionEntropy(std::span<const uint8_t> bytes) {
        if (bytes.empty()) return 0.0;
        std::array<uint64, 256> counts{};
        for (uint8_t value : bytes) ++counts[value];

        const double inv = 1.0 / static_cast<double>(bytes.size());
        double entropy = 0.0;
        for (uint64 count : counts) {
            if (count == 0) continue;
            const double p = static_cast<double>(count) * inv;
            entropy -= p * std::log2(p);
        }
        return entropy;
    }

    constexpr std::array<std::string_view, 10> kSteamRuntimeModuleNames = {
        "steamclient.dll",
        "steamclient64.dll",
        "steam_api.dll",
        "steam_api64.dll",
        "tier0_s.dll",
        "tier0_s64.dll",
        "vstdlib_s.dll",
        "vstdlib_s64.dll",
        "gameoverlayrenderer.dll",
        "gameoverlayrenderer64.dll",
    };

    std::string Lower(std::string value) {
        for (char& ch : value) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return value;
    }

    bool EndsWithInsensitive(std::string_view value, std::string_view suffix) {
        if (value.size() < suffix.size()) return false;

        const size_t offset = value.size() - suffix.size();
        for (size_t i = 0; i < suffix.size(); ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(value[offset + i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(suffix[i])));
            if (a != b) return false;
        }
        return true;
    }

    std::string BaseNameFromPath(const std::string& path) {
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos) return path;
        return path.substr(slash + 1);
    }

    std::string DirectoryFromPath(const std::string& path) {
        const size_t slash = path.find_last_of("\\/");
        if (slash == std::string::npos) return {};
        return path.substr(0, slash + 1);
    }

    std::string NormalizeDirectoryPrefix(std::string path) {
        path = Lower(std::move(path));
        std::replace(path.begin(), path.end(), '/', '\\');
        if (!path.empty() && path.back() != '\\') path += '\\';
        return path;
    }

    bool PathStartsWithInsensitive(const std::string& path, const std::string& prefix) {
        if (prefix.empty()) return false;
        std::string normalizedPath = Lower(path);
        std::replace(normalizedPath.begin(), normalizedPath.end(), '/', '\\');
        return normalizedPath.starts_with(prefix);
    }

    bool IsSteamRuntimeModuleName(std::string name) {
        name = Lower(std::move(name));
        return std::ranges::find(kSteamRuntimeModuleNames, name) != kSteamRuntimeModuleNames.end();
    }

    const OSTPlatform::PE::Section* FindLegacyDenuvoSection(const OSTPlatform::PE::Image& image) {
        for (std::string_view name : kLegacyDenuvoSections) {
            for (const auto& section : image.Sections()) {
                if (section.name == name) return &section;
            }
        }
        return nullptr;
    }

    std::optional<DetectionMatch> TryLegacySectionString(
        const ModuleCandidate& module,
        const OSTPlatform::PE::Image& image,
        const OSTPlatform::PE::Section& section) {
        const auto matchRaw = OSTPlatform::ByteSearch::FindInFile(
            module.nativePath,
            kLegacyDenuvoString,
            kLegacyScanChunkBytes);
        if (!matchRaw) {
            LOG_PIPE_DEBUG("DenuvoAuth: legacy section present but DENUVO string not found path={} section={}",
                           module.path, section.name);
            return std::nullopt;
        }

        DetectionMatch match{};
        match.method = DetectionMethod::LegacySectionString;
        match.sectionName = section.name;
        match.entryPointRva = image.EntryPointRva();
        match.matchRawOffset = static_cast<size_t>(*matchRaw);
        match.matchRva = image.RawOffsetToRva(match.matchRawOffset).value_or(0);
        return match;
    }

    std::optional<DetectionMatch> TryOepPattern(
        const ModuleCandidate& module,
        const OSTPlatform::PE::Image& image) {
        const uint32 oep = image.EntryPointRva();
        if (oep == 0) {
            LOG_PIPE_TRACE("DenuvoAuth: module skipped zero OEP path={}", module.path);
            return std::nullopt;
        }

        const OSTPlatform::PE::Section* section = image.SectionContainingRva(oep);
        if (!section) {
            LOG_PIPE_TRACE("DenuvoAuth: module skipped OEP not in section path={} oep=0x{:X}",
                           module.path, oep);
            return std::nullopt;
        }

        if (section->rawSize == 0) {
            LOG_PIPE_TRACE("DenuvoAuth: module skipped empty OEP section path={} section={} oep=0x{:X}",
                           module.path, section->name, oep);
            return std::nullopt;
        }

        // Streaming scan: read the section straight from disk in overlapped chunks
        // so the BMH pass runs concurrently with the next chunk's read, instead of
        // materializing the whole (~100s of MB) section in memory first. Returns the
        // absolute file offset of the match.
        const Utils::Stopwatch scanTimer;
        const auto matchRaw = OSTPlatform::ByteSearch::FindInFileRange(
            module.nativePath,
            section->rawOffset,
            section->rawSize,
            kDenuvoOepPattern,
            kOepScanChunkBytes);
        const double scanMs = scanTimer.ElapsedMs();
        LOG_PIPE_DEBUG("DenuvoAuth: OEP section timing path={} section={} raw_start=0x{:X} scan_size={} ({:.2f} MB) scan_ms={:.3f} matched={}",
                       module.path,
                       section->name,
                       section->rawOffset,
                       section->rawSize,
                       BytesToMiB(static_cast<uint64>(section->rawSize)),
                       scanMs,
                       matchRaw.has_value());
        if (!matchRaw) {
            LOG_PIPE_DEBUG("DenuvoAuth: OEP section scanned path={} section={} oep=0x{:X} raw_start=0x{:X} scan_size={} ({:.2f} MB) matched=false",
                           module.path,
                           section->name,
                           oep,
                           section->rawOffset,
                           section->rawSize,
                           BytesToMiB(static_cast<uint64>(section->rawSize)));
            return std::nullopt;
        }

        const size_t localMatch = static_cast<size_t>(*matchRaw) - section->rawOffset;
        DetectionMatch match{};
        match.method = DetectionMethod::OepPattern;
        match.sectionName = section->name;
        match.entryPointRva = oep;
        match.matchRawOffset = static_cast<size_t>(*matchRaw);
        match.matchRva = section->virtualAddress + static_cast<uint32>(localMatch);
        return match;
    }

    // 结构兜底：抓既没有 OEP 特征、段内也没有 DENUVO 字样的构建。
    //
    // 前提是存在足够大的可写且可执行段——运行时就地解密的必然产物，段头标志位
    // 在盘上和解密前就能读到。段名不能当必要条件：保护壳每次改版都会换名字，
    // 同一个游戏不同构建可能从保护区段名换成常规代码段名，只认段名就会漏判。
    //
    // 因此段名降级为补充证据，两条任一成立即判定：
    //   熵   盘上是加密态，最直接，但受样本分布影响，稀疏的构建会偏低；
    //   段名 保护壳改名留下的痕迹，随版本变化，专给熵偏低时兜底。
    // 两条都不成立才排除。判定偏向多认：漏判会让整条授权链路失效，而误判还会
    // 被 GetSpoofSteamID 的名单门控和取票失败的返回值挡住，代价小得多。
    std::optional<DetectionMatch> TryProtectedBlobSection(
        const ModuleCandidate& module,
        const OSTPlatform::PE::Image& image) {
        const bool hasLegacySection = FindLegacyDenuvoSection(image) != nullptr;

        for (const auto& section : image.Sections()) {
            if (!(section.IsExecutable() && section.IsWritable())) continue;
            if (section.rawSize < kProtectorBlobMinBytes) continue;

            const size_t sampleSize =
                (std::min)(static_cast<size_t>(section.rawSize), kProtectorBlobEntropySampleBytes);
            const OSTPlatform::PE::ByteBuffer sample = image.ReadRawBytes(section.rawOffset, sampleSize);
            const double entropy = sample.empty() ? 0.0 : SectionEntropy(sample);

            const bool highEntropy = entropy >= kProtectorBlobEncryptedEntropy;
            if (!highEntropy && !hasLegacySection) {
                LOG_PIPE_DEBUG("DenuvoAuth: RWX blob skipped, low entropy without legacy section path={} section={} entropy={:.3f}",
                               module.path, section.name, entropy);
                continue;
            }

            DetectionMatch match{};
            match.method = DetectionMethod::ProtectedBlobSection;
            match.sectionName = section.name;
            match.entryPointRva = image.EntryPointRva();
            match.matchRawOffset = section.rawOffset;
            match.matchRva = section.virtualAddress;
            LOG_PIPE_INFO("DenuvoAuth: protector blob section path={} section={} raw_size={} ({:.2f} MB) entropy={:.3f} matched_by={}",
                          module.path, section.name, section.rawSize,
                          BytesToMiB(static_cast<uint64>(section.rawSize)), entropy,
                          highEntropy ? "entropy" : "legacy-section-name");
            return match;
        }

        LOG_PIPE_DEBUG("DenuvoAuth: no qualifying RWX blob path={} legacy_section={}", module.path, hasLegacySection);
        return std::nullopt;
    }

    std::optional<DetectionMatch> DetectModule(
        const ModuleCandidate& module,
        const OSTPlatform::PE::Image& image) {
        // Prefer the OEP signature path; legacy section/string scan is broader.
        if (const auto match = TryOepPattern(module, image)) {
            return match;
        }

        if (const auto* legacySection = FindLegacyDenuvoSection(image)) {
            if (const auto match = TryLegacySectionString(module, image, *legacySection)) {
                return match;
            }
        }

        // 前两条都没命中时的最后一道：段结构特征。
        if (const auto match = TryProtectedBlobSection(module, image)) {
            return match;
        }
        return std::nullopt;
    }

    std::vector<ModuleCandidate> EnumerateModules(PID_t pid) {
        const Utils::Stopwatch timer;
        std::vector<ModuleCandidate> modules;

        size_t order = 0;
        for (auto module : OSTPlatform::Process::EnumerateModules(pid)) {
            if (module.path.empty()) continue;

            const bool executable = EndsWithInsensitive(module.path, ".exe");
            const bool dll = EndsWithInsensitive(module.path, ".dll");
            if (!executable && !dll) continue;
            if (module.size < kMinPackedModuleBytes) {
                LOG_PIPE_TRACE("DenuvoAuth: module skipped below packed size floor path={} size={} ({:.2f} MB) min={} ({:.2f} MB)",
                               module.path,
                               module.size,
                               BytesToMiB(module.size),
                               kMinPackedModuleBytes,
                               BytesToMiB(kMinPackedModuleBytes));
                continue;
            }
            if (!executable && OSTPlatform::Process::IsSystemModulePath(module.path)) continue;
            if (!executable && IsSteamRuntimeModuleName(BaseNameFromPath(module.path))) {
                LOG_PIPE_TRACE("DenuvoAuth: module skipped Steam runtime path={}", module.path);
                continue;
            }

            modules.push_back(ModuleCandidate{
                std::move(module.path),
                std::move(module.nativePath),
                module.size,
                executable,
                false,
                order++,
            });
        }

        std::string gameDirectory;
        for (const auto& module : modules) {
            if (!module.executable) continue;
            gameDirectory = NormalizeDirectoryPrefix(DirectoryFromPath(module.path));
            break;
        }

        for (auto& module : modules) {
            module.inGameTree = !module.executable &&
                PathStartsWithInsensitive(module.path, gameDirectory);
        }

        const size_t unsortedCount = modules.size();
        std::stable_sort(modules.begin(), modules.end(),
            [](const ModuleCandidate& lhs, const ModuleCandidate& rhs) {
                if (lhs.executable != rhs.executable) return lhs.executable;
                if (!lhs.executable && lhs.inGameTree != rhs.inGameTree) return lhs.inGameTree;
                if (!lhs.executable && lhs.size != rhs.size) return lhs.size > rhs.size;
                return lhs.order < rhs.order;
            });

        LOG_PIPE_DEBUG("DenuvoAuth: pid={} enumerated {} candidate module(s) elapsed_ms={:.3f}",
                       pid, unsortedCount, timer.ElapsedMs());
        return modules;
    }

    ProtectionScanReport RunProtectionScan(PID_t pid) {
        ProtectionScanReport report{};
        if (pid == 0) return report;

        const Utils::Stopwatch totalTimer;
        std::vector<ModuleCandidate> modules = EnumerateModules(pid);
        LOG_PIPE_DEBUG("DenuvoAuth: pid={} scanning {} candidate module(s)", pid, modules.size());

        for (const auto& module : modules) {
            ++report.scannedModules;
            LOG_PIPE_DEBUG("DenuvoAuth: module candidate {}: {}", report.scannedModules, module.DebugString());

            const OSTPlatform::PE::Image image(module.nativePath);
            if (!image) {
                LOG_PIPE_DEBUG("DenuvoAuth: module skipped invalid or unreadable PE path={} size={}",
                               module.path, module.size);
                continue;
            }

            const auto match = DetectModule(module, image);
            if (!match) continue;

            report.pid = pid;
            report.denuvoDetected = true;
            report.method = match->method;
            report.modulePath = module.path;
            report.sectionName = match->sectionName;
            report.moduleSize = module.size;
            report.entryPointRva = match->entryPointRva;
            report.matchRva = match->matchRva;
            report.matchRawOffset = match->matchRawOffset;
            report.elapsedMs = totalTimer.ElapsedMs();

            LOG_PIPE_INFO("DenuvoAuth: success to detect Denuvo in module {}", report.DebugString());
            return report;
        }

        report.elapsedMs = totalTimer.ElapsedMs();
        LOG_PIPE_DEBUG("DenuvoAuth: pid={} no Denuvo match scanned={} elapsed_ms={:.3f}",
                       pid, report.scannedModules, report.elapsedMs);
        return report;
    }

} // namespace

const char* ToString(DetectionMethod method) {
    switch (method) {
    case DetectionMethod::None: return "None";
    case DetectionMethod::LegacySectionString: return "LegacySectionString";
    case DetectionMethod::OepPattern: return "OepPattern";
    case DetectionMethod::ProtectedBlobSection: return "ProtectedBlobSection";
    }
    return "Unknown";
}

ProtectionScanReport ScanProtection(PID_t pid) {
    return RunProtectionScan(pid);
}

} // namespace PipeManager::DenuvoAuth
