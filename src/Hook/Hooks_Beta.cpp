#include "Hooks_Beta.h"
#include "dllmain.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/SteamMetadata/PatternLoader.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace {

    using SetActiveBeta_t = bool(__fastcall*)(void*, AppId_t, const char*);
    using Clock = std::chrono::steady_clock;

    // CUserAppManager::SetActiveBeta(CUserAppManager*, AppId_t, const char*)
    // is resolved through PatternLoader ("SetActiveBeta"); the RVA/signature
    // live in the steamclient pattern spec, not in this file. Atomic because
    // Uninstall() runs on the diversion watchdog thread while OnUIThread()
    // runs on the UI thread.
    std::atomic<SetActiveBeta_t> oSetActiveBeta{ nullptr };

    std::atomic<void*> g_userAppMgr{ nullptr };
    std::atomic<bool> g_finished{ false };

    // 0 = 尚未就绪。用 steady_clock ticks 存，UI/卸载线程都能安全读写。
    std::atomic<int64_t> g_uiReadyAt{ 0 };
    // RunFrame 首次执行时间；FillInAppOverview 信号缺席时用来兜底。
    std::atomic<int64_t> g_firstFrameAt{ 0 };

    constexpr auto kStableDelay = std::chrono::seconds(4);
    constexpr auto kFrameFallbackDelay = std::chrono::seconds(8);
    constexpr auto kRetryInterval = std::chrono::seconds(2);
    constexpr int kMaxAttempts = 12;

    struct PendingBeta {
        AppId_t appId = 0;
        std::string betaKey;
    };

    std::mutex g_mutex;
    std::vector<PendingBeta> g_pending;
    bool g_prepared = false;
    int  g_attempts = 0;
    int64_t g_lastAttempt = 0;

    int64_t NowTicks() {
        return Clock::now().time_since_epoch().count();
    }

    bool Elapsed(int64_t startTicks, int64_t nowTicks, std::chrono::seconds delay) {
        return startTicks != 0 &&
            nowTicks - startTicks >=
                std::chrono::duration_cast<Clock::duration>(delay).count();
    }

    // 只认第一次就绪信号；重复调用无副作用。
    void MarkUIReady(const char* reason) {
        int64_t expected = 0;
        const int64_t now = NowTicks();
        if (g_uiReadyAt.compare_exchange_strong(expected, now,
                                                std::memory_order_relaxed)) {
            LOG_BETA_DEBUG("UI ready ({}), beta restore armed", reason);
        }
    }

    // ── small VDF / file helpers ────────────────────────────────────

    std::string ReadTextFile(const std::filesystem::path& path) {
        constexpr uintmax_t kMaxSize = 1u << 20;
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        // appmanifest 正常只有几 KB；异常大文件先看大小，别先读进内存。
        if (ec || size == 0 || size > kMaxSize) return {};

        std::ifstream in(path, std::ios::binary);
        if (!in) return {};
        std::string text(static_cast<size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(size));
        // 读不满说明文件在 file_size 和 read 之间被 Steam 改过；宁可跳过
        // 这一次，也不要基于半截 manifest 做恢复决定。
        if (static_cast<uintmax_t>(in.gcount()) != size) return {};
        return text;
    }

    // 在 "Section" { ... } 块里取 key 的第一个字符串值。
    // section 传入带引号的形式（"UserConfig"），避免子串误命中。
    std::optional<std::string> GetSectionValue(const std::string& text,
                                               const char* section,
                                               const char* key) {
        size_t pos = 0;
        while ((pos = text.find(section, pos)) != std::string::npos) {
            size_t open = text.find('{', pos);
            if (open == std::string::npos) return std::nullopt;

            int depth = 0;
            size_t end = std::string::npos;
            for (size_t i = open; i < text.size(); ++i) {
                if (text[i] == '{') ++depth;
                else if (text[i] == '}') {
                    if (--depth == 0) { end = i; break; }
                }
            }
            if (end == std::string::npos) return std::nullopt;

            const std::string block = text.substr(open, end - open + 1);
            const std::string needle = std::string("\"") + key + "\"";
            size_t kpos = block.find(needle);
            if (kpos == std::string::npos) return std::nullopt;

            size_t q1 = block.find('"', kpos + needle.size());
            if (q1 == std::string::npos) return std::nullopt;
            size_t q2 = block.find('"', q1 + 1);
            if (q2 == std::string::npos) return std::nullopt;
            return block.substr(q1 + 1, q2 - q1 - 1);
        }
        return std::nullopt;
    }

    // libraryfolders.vdf 里的路径是双反斜杠转义的 VDF 字符串。
    std::string UnescapeVdfPath(std::string value) {
        std::string out;
        out.reserve(value.size());
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '\\' && i + 1 < value.size() && value[i + 1] == '\\') {
                out.push_back('\\');
                ++i;
            } else {
                out.push_back(value[i]);
            }
        }
        return out;
    }

    std::vector<std::string> GetSteamLibraryPaths() {
        std::vector<std::string> paths;
        if (!SteamInstallPath[0]) {
            LOG_BETA_WARN("GetSteamLibraryPaths: Steam install path is empty");
            return paths;
        }
        paths.emplace_back(SteamInstallPath);

        const std::filesystem::path vdf =
            std::filesystem::path(SteamInstallPath) / "steamapps" / "libraryfolders.vdf";
        const std::string text = ReadTextFile(vdf);
        if (text.empty()) return paths;

        size_t pos = 0;
        const std::string needle = "\"path\"";
        while ((pos = text.find(needle, pos)) != std::string::npos) {
            size_t q1 = text.find('"', pos + needle.size());
            if (q1 == std::string::npos) break;
            size_t q2 = text.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string path = UnescapeVdfPath(text.substr(q1 + 1, q2 - q1 - 1));
            if (!path.empty() &&
                std::find(paths.begin(), paths.end(), path) == paths.end())
                paths.push_back(std::move(path));
            pos = q2 + 1;
        }
        return paths;
    }

    std::optional<AppId_t> ParseManifestAppId(std::string_view name) {
        constexpr std::string_view kPrefix = "appmanifest_";
        constexpr std::string_view kSuffix = ".acf";
        if (name.size() <= kPrefix.size() + kSuffix.size()) return std::nullopt;
        if (name.substr(0, kPrefix.size()) != kPrefix) return std::nullopt;
        if (name.substr(name.size() - kSuffix.size()) != kSuffix) return std::nullopt;

        const std::string_view digits = name.substr(
            kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
        uint32_t id = 0;
        const auto [ptr, ec] =
            std::from_chars(digits.data(), digits.data() + digits.size(), id);
        if (ec != std::errc{} || ptr != digits.data() + digits.size())
            return std::nullopt;
        return static_cast<AppId_t>(id);
    }

    // 每个库只扫一次 steamapps，建 appId -> appmanifest 路径的索引。
    // 避免对每个 lua appId 都在 N 个库目录上各做一次 exists。
    std::unordered_map<AppId_t, std::filesystem::path> IndexAppManifests(
        const std::vector<std::string>& libraries) {
        std::unordered_map<AppId_t, std::filesystem::path> index;
        for (const auto& lib : libraries) {
            const std::filesystem::path dir =
                std::filesystem::path(lib) / "steamapps";
            std::error_code ec;
            std::filesystem::directory_iterator it(dir, ec);
            const std::filesystem::directory_iterator end;
            if (ec) continue;
            for (; it != end; it.increment(ec)) {
                if (ec) break;
                const auto appId =
                    ParseManifestAppId(it->path().filename().string());
                if (appId) index.try_emplace(*appId, it->path());
            }
        }
        return index;
    }

    // 找出需要恢复的 app：UserConfig 已丢失分支、但 MountedConfig 仍记录
    // 着非 public 分支（即磁盘上当前挂载的确实是测试版）。
    std::vector<PendingBeta> BuildPendingList() {
        std::vector<PendingBeta> result;
        const auto libraries = GetSteamLibraryPaths();
        if (libraries.empty()) return result;

        const auto manifests = IndexAppManifests(libraries);
        if (manifests.empty()) {
            LOG_BETA_DEBUG("BuildPendingList: no appmanifest in {} library path(s)",
                           libraries.size());
            return result;
        }

        // LuaConfig 的现有接口没有锁；本模块按项目既有线程模型，只在 UI
        // 线程上调用一次（Hooks_Package / CloudRedirectHost 同样如此）。
        auto appIds = LuaConfig::GetAllDepotIds();
        std::sort(appIds.begin(), appIds.end());
        appIds.erase(std::unique(appIds.begin(), appIds.end()), appIds.end());

        for (AppId_t appId : appIds) {
            const auto it = manifests.find(appId);
            if (it == manifests.end()) continue;
            const std::string text = ReadTextFile(it->second);
            if (text.empty()) continue;

            const auto userBeta =
                GetSectionValue(text, "\"UserConfig\"", "BetaKey");
            const auto mountedBeta =
                GetSectionValue(text, "\"MountedConfig\"", "BetaKey");

            // Steam 已经知道当前分支，不需要干预。
            if (userBeta && !userBeta->empty() && *userBeta != "public")
                continue;

            // 注意：UserConfig 里显式写着 "public" 时这里也会恢复。Steam
            // 丢弃该键时是直接删除还是写成 public，需要在目标 build 上
            // 用实际 manifest 确认；这一条已列为 PR 的待确认项。
            if (mountedBeta && !mountedBeta->empty() && *mountedBeta != "public")
                result.push_back({ appId, *mountedBeta });
        }
        return result;
    }

    // SEH 外壳：只含 POD 局部量，避免 /EHsc 下的 C++ 对象展开问题。
    bool CallSetActiveBetaRaw(SetActiveBeta_t fn, void* userAppMgr,
                              AppId_t appId, const char* betaKey) {
        bool ok = false;
        __try {
            ok = fn(userAppMgr, appId, betaKey);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        return ok;
    }

} // namespace

namespace Hooks_Beta {

    void Install() {
        if (!client_hModule) {
            LOG_BETA_WARN("Install: steamclient module not loaded, beta restore disabled");
            return;
        }
        if (oSetActiveBeta.load(std::memory_order_relaxed)) return;

        void* addr = PatternLoader::FindPattern(client_hModule, "SetActiveBeta");
        if (!addr) {
            LOG_BETA_WARN("Install: SetActiveBeta not resolved by PatternLoader "
                          "(missing pattern entry), beta restore disabled");
            return;
        }

        g_finished.store(false, std::memory_order_relaxed);
        oSetActiveBeta.store(reinterpret_cast<SetActiveBeta_t>(addr),
                             std::memory_order_release);
        const auto rva = reinterpret_cast<uintptr_t>(addr) -
                         reinterpret_cast<uintptr_t>(client_hModule);
        LOG_BETA_INFO("Install: SetActiveBeta resolved at {} (RVA 0x{:X})",
                      addr, rva);
    }

    void Uninstall() {
        // 先置 finished 再清函数指针：UI 线程看到 finished 就不再往下走。
        // 必须把全部运行时状态一起重置，fallback 后重新 Install 才能干净
        // 地重新武装（看门狗会走 CoreUnhook -> CoreHook）。
        g_finished.store(true, std::memory_order_relaxed);
        oSetActiveBeta.store(nullptr, std::memory_order_release);
        g_userAppMgr.store(nullptr, std::memory_order_relaxed);
        g_uiReadyAt.store(0, std::memory_order_relaxed);
        g_firstFrameAt.store(0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending.clear();
            g_prepared = false;
            g_attempts = 0;
            g_lastAttempt = 0;
        }
    }

    void NotifyUserAppMgr(void* userAppMgr) {
        if (!userAppMgr) return;
        void* expected = nullptr;
        g_userAppMgr.compare_exchange_strong(expected, userAppMgr,
                                             std::memory_order_relaxed);
    }

    void NotifyOverview() {
        MarkUIReady("app overview");
    }

    void OnUIThread() {
        if (g_finished.load(std::memory_order_relaxed)) return;

        const SetActiveBeta_t fn = oSetActiveBeta.load(std::memory_order_acquire);
        if (!fn) return;

        const int64_t now = NowTicks();

        // FillInAppOverview 是首选信号；如果库里没有任何 lua app 的
        // overview 被填充，就一直不会来，用帧回调兜底避免静默失效。
        if (g_uiReadyAt.load(std::memory_order_relaxed) == 0) {
            int64_t first = g_firstFrameAt.load(std::memory_order_relaxed);
            if (first == 0) {
                int64_t expected = 0;
                g_firstFrameAt.compare_exchange_strong(
                    expected, now, std::memory_order_relaxed);
                return;
            }
            if (!Elapsed(first, now, kFrameFallbackDelay)) return;
            MarkUIReady("frame fallback");
        }

        const int64_t readyAt = g_uiReadyAt.load(std::memory_order_relaxed);
        // 等 UI 稳定几秒再动手：app info / 分支数据需要先加载完。
        if (!Elapsed(readyAt, now, kStableDelay)) return;

        void* userAppMgr = g_userAppMgr.load(std::memory_order_relaxed);
        if (!userAppMgr) return;

        std::vector<PendingBeta> work;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_prepared) {
                g_pending = BuildPendingList();
                g_prepared = true;
                if (g_pending.empty()) {
                    g_finished.store(true, std::memory_order_relaxed);
                    LOG_BETA_DEBUG("OnUIThread: no app needs beta restore");
                    return;
                }
                LOG_BETA_INFO("OnUIThread: restoring branches for {} app(s)",
                              g_pending.size());
            }
            if (g_attempts > 0 && !Elapsed(g_lastAttempt, now, kRetryInterval))
                return;
            if (g_attempts >= kMaxAttempts) {
                g_finished.store(true, std::memory_order_relaxed);
                LOG_BETA_WARN("OnUIThread: giving up after {} attempts, "
                              "{} app(s) still pending", g_attempts, g_pending.size());
                return;
            }
            ++g_attempts;
            g_lastAttempt = now;
            work = g_pending;
        }

        std::vector<PendingBeta> remaining;
        for (const auto& item : work) {
            // 恢复途中如果发生了 fallback 卸载，别再碰已经不成立的实例。
            if (g_finished.load(std::memory_order_relaxed)) return;
            const bool ok = CallSetActiveBetaRaw(
                fn, userAppMgr, item.appId, item.betaKey.c_str());
            LOG_BETA_INFO("SetActiveBeta app {} -> '{}': {}",
                          item.appId, item.betaKey, ok ? "ok" : "failed");
            if (!ok) remaining.push_back(item);
        }

        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_pending = std::move(remaining);
            if (g_pending.empty()) {
                g_finished.store(true, std::memory_order_relaxed);
                LOG_BETA_INFO("OnUIThread: beta restore completed");
            }
        }
    }

} // namespace Hooks_Beta
