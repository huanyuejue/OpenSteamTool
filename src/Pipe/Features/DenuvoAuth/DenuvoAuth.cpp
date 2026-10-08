#include "Pipe/Features/DenuvoAuth/DenuvoAuth.h"

#include "Hook/Hooks_Package.h"
#include "Pipe/Features/DenuvoAuth/ProtectionScan.h"
#include "Utils/Logging/Log.h"
#include "Utils/Tickets/AppTicket.h"
#include "Utils/Config/LuaConfig.h"
#include "OSTPlatform/include/SteamCredentialStore.h"

#include <chrono>
#include <cwctype>
#include <optional>
#include <string_view>
#include <unordered_map>

namespace PipeManager::DenuvoAuth {
namespace {

    constexpr uint32 kEndDenuvoVerificationHandshake = 2;

    // Scheme 1（默认）：启动微脉冲 300ms。盖住 Denuvo 离线 token 验证，
    // 同时在游戏引擎初始化存档目录前过期，存档进原生 userdata/<真SteamID>/。
    constexpr std::chrono::milliseconds kScheme1StartupPulseDuration{300};
    // Scheme 1（默认）：票据微脉冲 300ms。Denuvo 取票后立即做
    // memcmp(Ticket->SteamID, GetSteamID()) 交叉检查（约 3ms），300ms 高容错覆盖多 pipe 验证。
    constexpr std::chrono::milliseconds kScheme1TicketPulseDuration{300};
    // Scheme 2（dauth2 兼容）：启动宽限 2500ms。盖住 Denuvo VM 加密延迟（约 1.4s）。
    constexpr std::chrono::milliseconds kDAuth2StartupGraceDuration{2500};
    // Scheme 2（dauth2 兼容）：票据租约 3000ms。盖住二次验证 burst（约 1.8s）。
    constexpr std::chrono::milliseconds kDAuth2TicketLeaseDuration{3000};

    enum class Stage {
        None,
        Authorizing,
        EndAuthorization,
    };

    const char* ToString(Stage stage) {
        switch (stage) {
        case Stage::None:             return "None";
        case Stage::Authorizing:      return "Authorizing";
        case Stage::EndAuthorization: return "EndAuthorization";
        }
        return "?";
    }

    bool EqualsUniverseName(std::wstring_view lhs, std::wstring_view rhs) {
        if (lhs.size() != rhs.size()) return false;

        for (size_t i = 0; i < lhs.size(); ++i) {
            if (std::towlower(lhs[i]) != std::towlower(rhs[i])) return false;
        }

        return true;
    }

    EUniverse ParseUniverse(std::wstring_view universe) {
        if (EqualsUniverseName(universe, L"Public")) return k_EUniversePublic;
        if (EqualsUniverseName(universe, L"Beta")) return k_EUniverseBeta;
        if (EqualsUniverseName(universe, L"Internal")) return k_EUniverseInternal;
        if (EqualsUniverseName(universe, L"Dev")) return k_EUniverseDev;
        return k_EUniverseInvalid;
    }

    std::optional<uint64> GetCurrentSteamIdForDenuvoAuth() {
        uint32 accountId = 0;
        std::wstring universeName;
        const auto status = OSTPlatform::SteamCredentialStore::GetActiveUser(accountId, universeName);
        if (status != OSTPlatform::SteamCredentialStore::Status::Ok) {
            LOG_PIPE_WARN("DenuvoAuth: active Steam user unavailable ({})",
                           OSTPlatform::SteamCredentialStore::ToString(status));
            return std::nullopt;
        }

        const EUniverse universe = ParseUniverse(universeName);
        if (universe == k_EUniverseInvalid) {
            LOG_PIPE_WARN("DenuvoAuth: active Steam user has unsupported universe");
            return std::nullopt;
        }

        CSteamID steamId;
        steamId.Set(accountId, universe, k_EAccountTypeIndividual);
        return steamId.ConvertToUint64();
    }

    struct ProcessAuth {
        bool scanned = false;
        bool denuvo = false;
        Stage stage = Stage::None;
        uint32 pid = 0;
        uint32 handshakeCount = 0;
        // Scheme 2 开关：dauth2(appid) 配过即 true，单向升级不降级。
        bool isDAuth2 = false;
        // 启动脉冲只 armed 一次；deadline 只延长不缩短（monotonic）。
        bool startupArmed = false;
        std::chrono::steady_clock::time_point scheme1Deadline{};
        std::chrono::steady_clock::time_point authDeadline{};

        std::optional<PipeKey> authorizationPipe;
        AppId_t authorizedAppId = k_uAppIdInvalid;

        std::string DebugString() const {
            const auto now = std::chrono::steady_clock::now();
            const auto& deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            long long remainingMs = 0;
            if (deadline != std::chrono::steady_clock::time_point{}) {
                remainingMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - now).count();
                if (remainingMs < 0) remainingMs = 0;
            }
            return std::format("mode={} denuvo={} stage={} remaining_ms={} handshakeCount={} auth_appid={} pid={}",
                               isDAuth2 ? "dauth2" : "default", denuvo, ToString(stage),
                               remainingMs, handshakeCount, authorizedAppId, pid);
        }

        void OnHandshake(const PipeContext& ctx, const PipeKey& pipeKey) {
            ++handshakeCount;

            if (!denuvo) {
                stage = Stage::None;
                return;
            }

            if (!authorizationPipe.has_value()) {
                authorizationPipe = pipeKey;
                authorizedAppId = ctx.appId;
                pid = ctx.process.pid;
                stage = Stage::Authorizing;
                LOG_PIPE_INFO("DenuvoAuth: authorization pipe selected {}", this->DebugString());
                // 首个授权 pipe 上 armed 启动脉冲：多线程引擎的多 pipe 握手都盖在租约里，
                // 不再单纯依赖"2 次握手就关"的计数（多线程下计数不可靠）。deadline 只延长不缩短。
                if (!startupArmed) {
                    startupArmed = true;
                    const auto now = std::chrono::steady_clock::now();
                    const auto startupDeadline = now + (isDAuth2 ? kDAuth2StartupGraceDuration
                                                                : kScheme1StartupPulseDuration);
                    auto& deadline = isDAuth2 ? authDeadline : scheme1Deadline;
                    if (startupDeadline > deadline) {
                        deadline = startupDeadline;
                    }
                    LOG_PIPE_INFO("DenuvoAuth: startup pulse armed for pid={} mode={} {}", pid,
                                  isDAuth2 ? "dauth2" : "default", this->DebugString());
                }
            }

            if (stage == Stage::Authorizing &&
                handshakeCount >= kEndDenuvoVerificationHandshake) {
                stage = Stage::EndAuthorization;
                LOG_PIPE_INFO("DenuvoAuth: authorization window ended {}", this->DebugString());
                // 有效许可（真拥有或家庭共享）才持久化身份：共享游戏排除在外会导致其 54。
                if(Hooks_Package::HasValidLicense(authorizedAppId)){
                    WriteSteamIdOnEndAuthorization();
                }
            }
        }

        // 票据请求延长租约：Denuvo 取票后立即做 memcmp 交叉验证，
        // 租约盖住验证 burst，过期自动恢复真实身份。
        void OnOwnershipTicketRequested() {
            if (!denuvo || !authorizationPipe.has_value()) return;
            const auto now = std::chrono::steady_clock::now();
            const auto extension = now + (isDAuth2 ? kDAuth2TicketLeaseDuration
                                                  : kScheme1TicketPulseDuration);
            auto& deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            if (extension > deadline) {
                deadline = extension;
                LOG_PIPE_INFO("DenuvoAuth: ticket pulse extended for pid={} mode={} {}", pid,
                              isDAuth2 ? "dauth2" : "default", this->DebugString());
            }
        }

        bool LeaseActive() const {
            const auto& deadline = isDAuth2 ? authDeadline : scheme1Deadline;
            if (deadline == std::chrono::steady_clock::time_point{}) return false;
            return std::chrono::steady_clock::now() <= deadline;
        }

        bool CanUseAuthorizedIdentity(const PipeKey& key) const {
            if (!denuvo || authorizationPipe != key) return false;
            // 纯租约制：计数只决定存档身份持久化时机，不决定授权开关。
            // 多线程验证 burst 在租约内看到的身份一致，不 54；过期自动恢复真实身份。
            return LeaseActive();
        }

        void WriteSteamIdOnEndAuthorization() const {
            if (authorizedAppId == k_uAppIdInvalid) {
                LOG_PIPE_WARN("DenuvoAuth: end authorization skipped SteamID persist without auth app");
                return;
            }

            const std::optional<uint64> steamId = GetCurrentSteamIdForDenuvoAuth();
            if (!steamId || *steamId == 0) {
                LOG_PIPE_WARN("DenuvoAuth: end authorization no current SteamID source auth_appid={}",
                               authorizedAppId);
                return;
            }

            if (AppTicket::WriteSteamID(authorizedAppId, *steamId)) {
                LOG_PIPE_INFO("DenuvoAuth: persisted end-authorization SteamID auth_appid={} steamid={}",
                              authorizedAppId, *steamId);
                return;
            }

            LOG_PIPE_WARN("DenuvoAuth: failed to persist end-authorization SteamID auth_appid={} steamid={}",
                          authorizedAppId, *steamId);
        }
    };

    // All access runs on the single Steam IPC thread, so these need no lock.
    std::unordered_map<ProcessKey, ProcessAuth, ProcessKeyHash> g_processAuth;
    std::unordered_map<PipeKey, ProcessKey, PipeKeyHash> g_pipeProcess;

    ProcessAuth* FindAuthForPipe(const PipeKey& pipeKey) {
        const auto pipeIt = g_pipeProcess.find(pipeKey);
        if (pipeIt == g_pipeProcess.end()) return nullptr;

        const auto authIt = g_processAuth.find(pipeIt->second);
        return authIt == g_processAuth.end() ? nullptr : &authIt->second;
    }

    void EnsureScanned(ProcessAuth& auth, const ProcessKey& process) {
        if (auth.scanned) {
            LOG_PIPE_TRACE("DenuvoAuth: reusing cached protection result {} denuvo={}",
                           process.DebugString(), auth.denuvo);
            return;
        }

        auth.scanned = true;
        auth.denuvo = ScanProtection(process.pid).denuvoDetected;
        if (!auth.denuvo) auth.stage = Stage::None;
    }

} // namespace

void Apply(const PipeContext& ctx) {
    if (!ctx.gameProcess || !ctx.trackedApp) return;

    const PipeKey pipeKey = MakePipeKey(ctx.pipe);
    if (!pipeKey.IsValid()) return;

    ProcessAuth& auth = g_processAuth[ctx.process];
    g_pipeProcess[pipeKey] = ctx.process;

    EnsureScanned(auth, ctx.process);
    // Scheme 2 开关单向升级：一旦配过 dauth2 就保持，不降级。
    // 升级时若启动脉冲已按 default armed 过，authDeadline 还是 zero，
    // 必须补一次 dauth2 的 armed，否则授权会立刻断（monotonic，只延长）。
    if (LuaConfig::IsDAuth2(ctx.appId) && !auth.isDAuth2) {
        auth.isDAuth2 = true;
        if (auth.startupArmed) {
            const auto now = std::chrono::steady_clock::now();
            const auto startupDeadline = now + kDAuth2StartupGraceDuration;
            if (startupDeadline > auth.authDeadline) {
                auth.authDeadline = startupDeadline;
            }
        }
    }
    auth.OnHandshake(ctx, pipeKey);
}

void OnOwnershipTicketRequested(const CPipeClient* pipe) {
    if (!pipe) return;
    const PipeKey pipeKey = MakePipeKey(pipe);
    ProcessAuth* auth = FindAuthForPipe(pipeKey);
    if (!auth) return;
    auth->OnOwnershipTicketRequested();
}

bool IsAuthorizedPipe(const CPipeClient* pipe) {
    if (!pipe) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not in authorization window: null pipe");
        return false;
    }

    const PipeKey pipeKey = MakePipeKey(pipe);
    ProcessAuth* auth = FindAuthForPipe(pipeKey);
    if (!auth) {
        LOG_PIPE_TRACE("DenuvoAuth: pipe not tracked by DenuvoAuth {}", pipeKey.DebugString());
        return false;
    }

    if (!auth->CanUseAuthorizedIdentity(pipeKey)) {
        // 租约过期但计数没走完：计数触发不了收尾，这里补推进，
        // 否则存档身份永远落不了盘（WriteSteamIdOnEndAuthorization 调不到）。
        if (auth->denuvo && auth->stage == Stage::Authorizing &&
            auth->startupArmed && !auth->LeaseActive()) {
            auth->stage = Stage::EndAuthorization;
            LOG_PIPE_INFO("DenuvoAuth: lease expired, authorization window ended {}",
                          auth->DebugString());
            if (Hooks_Package::HasValidLicense(auth->authorizedAppId)) {
                auth->WriteSteamIdOnEndAuthorization();
            }
        }
        LOG_PIPE_TRACE("DenuvoAuth: pipe not in authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
        return false;
    }
    LOG_PIPE_INFO("DenuvoAuth: pipe in authorization window {} {}", pipeKey.DebugString(), auth->DebugString());
    return true;
}

} // namespace PipeManager::DenuvoAuth
