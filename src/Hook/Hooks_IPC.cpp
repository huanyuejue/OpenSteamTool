#include "dllmain.h"
#include "Hooks_IPC.h"
#include "Hooks_IPC_ISteamUser.h"
#include "Hooks_IPC_ISteamUtils.h"
#include "HookMacros.h"
#include "Hooks_Misc.h"
#include "Pipe/PipeManager.h"
#include "Utils/Support/FnvHash.h"
#include "Utils/SteamMetadata/IPCLoader.h"

namespace {

    RESOLVE_FUNC(GetPipeClient, CPipeClient*, void* pEngine, HSteamPipe hSteamPipe);

    static CPipeClient* GetPipe(void* pServer, HSteamPipe hSteamPipe) {
        return oGetPipeClient ? oGetPipeClient(pServer, hSteamPipe) : nullptr;
    }

    //  Handler dispatch table
    struct ResolvedHandler {
        EIPCInterface         interfaceID;
        uint32                funcHash;
        std::string           name;       // "IClientUser::GetSteamID" — for logs
        uint32                fencepost;
        uint32                argc;
        IPCHandlerFn          pre;
        IPCHandlerFn          post;

        ResolvedHandler(const IPCHandlerEntry& entry, const IPCLoader::Method& method)
            : interfaceID(method.interfaceID),
              funcHash(method.funcHash),
              name(std::string(entry.interfaceName) + "::" + entry.methodName),
              fencepost(method.fencepost),
              argc(method.argc),
              pre(entry.pre),
              post(entry.post) {}

        std::string DebugString() const {
            return std::format("{} -> hash=0x{:08X} fencepost=0x{:08X} argc={}",
                name, funcHash, fencepost, argc);
        }
    };
    std::vector<ResolvedHandler> g_Handlers;

    // IClientUtils::GetAppID 的 funcHash，注册时填。onlinefix 的 480 翻转要靠
    // 这个接口在 appid 不在 Lua 名单的情况下也照常分发，其余 handler 不放行。
    uint32 g_onlineFixAppIdHash = 0;

    static ResolvedHandler* FindHandler(EIPCInterface iface, uint32 funcHash) {
        for (auto& e : g_Handlers) {
            if (e.interfaceID == iface && e.funcHash == funcHash) return &e;
        }
        return nullptr;
    }

    struct IPCDispatch {
        CPipeClient*     pipe = nullptr;
        ResolvedHandler* handler = nullptr;

        bool enabled() const {
            return pipe && handler;
        }

        std::string DebugString() const {
            return std::format("{} {}",pipe ? pipe->DebugString() : "null",
                                handler ? handler->DebugString() : "null");
        }
    };

    static IPCDispatch ResolveDispatch(void* pServer,HSteamPipe hSteamPipe,CUtlBuffer* pRead)
    {
        IPCDispatch dispatch{};
        dispatch.pipe = GetPipe(pServer, hSteamPipe);
        if (!dispatch.pipe) return dispatch;

        // We only care about InterfaceCall messages
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok()) return dispatch;
        if (request.command() != EIPCCommand::InterfaceCall) return dispatch;

        // Parse out the interface call header before the appid gate: the
        // onlinefix exemption below needs to know which method this is.
        IPCMessages::IPCInterfaceCall call{request.body()};
        if (!call.ok()) return dispatch;

        // Ignore calls when appId is not resolved or not in Lua config.
        // -onlinefix games are exempt, but only for IClientUtils::GetAppID:
        // a genuine copy launched with -onlinefix is not in the Lua list, and
        // gating everything would stop the 480 flip from ever running.
        // Every other handler keeps the gate — letting them through would have
        // the ownership-ticket path forge a pooled ticket over the real one.
        const bool onlineFixAppIdQuery =
            Hooks_Misc::IsOnlineFixActive() &&
            g_onlineFixAppIdHash != 0 &&
            call.interfaceID() == EIPCInterface::IClientUtils &&
            call.funcHash() == g_onlineFixAppIdHash;
        if (!onlineFixAppIdQuery &&
            !LuaConfig::HasDepot(Hooks_Misc::ResolveAppId())) {
            return dispatch;
        }

        // Lookup handler by interface ID + method hash
        dispatch.handler = FindHandler(call.interfaceID(), call.funcHash());
        if (!dispatch.handler) return dispatch;

        LOG_IPC_TRACE("Resolved IPC handler: {}", dispatch.DebugString());
        return dispatch;
    }

    static void HandleHandshake(void* pServer, HSteamPipe hSteamPipe,CUtlBuffer* pRead)
    {
        IPCMessages::IPCRequest request{pRead};
        if (!request.ok()|| request.command() != EIPCCommand::Handshake) return;
        IPCMessages::IPCHandshakeReq handshake{request.body()};
        if(!handshake.ok()) return;

        CPipeClient* pipe = GetPipe(pServer, hSteamPipe);
        if (!pipe) return;
        // set client PID 
        pipe->m_clientPID = handshake.pid();

        LOG_IPC_DEBUG("Received handshake from {},{}", pipe->DebugString(), handshake.DebugString());
        PipeManager::OnHandshake(pipe);
    }

    // 抓游戏首次使用 SteamNetworkingSockets 的时机，据此决定 GetAppID 是否切到 480。
    // 联机会话证书按 480 签发，早于 P2P 切换会让游戏拿不到自己的真实配置；
    // 配了 -realappid 时这里只做记录，不改变上报值。
    static void DetectNetworkingSockets(CUtlBuffer* pRead)
    {
        if (!Hooks_Misc::IsOnlineFixActive() || Hooks_Misc::ShouldReportOnlineFixAppId()) return;

        IPCMessages::IPCRequest request{pRead};
        if (!request.ok() || request.command() != EIPCCommand::InterfaceCall) return;

        IPCMessages::IPCInterfaceCall call{request.body()};
        if (!call.ok()) return;

        if (call.interfaceID() == EIPCInterface::IClientNetworkingSocketsSerialized)
            Hooks_Misc::NotifyNetworkingSocketsUsed();
    }

    HOOK_FUNC(IPCProcessMessage, bool,void* pServer, HSteamPipe hSteamPipe,
              CUtlBuffer* pRead, CUtlBuffer* pWrite)
    {
        // handle handshake messages
        HandleHandshake(pServer, hSteamPipe, pRead);
        DetectNetworkingSockets(pRead);

        IPCDispatch dispatch = ResolveDispatch(pServer, hSteamPipe, pRead);
        // If we didn't find a handler for this message, just pass through to the original function.
        if(!dispatch.enabled()) return oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

        // If we did find a handler, run the pre-handler
        if (dispatch.handler->pre)
            dispatch.handler->pre(dispatch.pipe, pRead, pWrite);

        // Then call the original function to let steamclient process the message as normal.
        bool result = oIPCProcessMessage(pServer, hSteamPipe, pRead, pWrite);

        // Ultimately the post-handler can choose to modify the response.
        if (result && dispatch.handler->post)
            dispatch.handler->post(dispatch.pipe, pRead, pWrite);

        return result;
    }

} // namespace


namespace Hooks_IPC {

    void Install() {
        RESOLVE_C(GetPipeClient);

        // Each module registers a static array. Hash lookup against the
        // IPCLoader metadata happens inside RegisterHandlers.
        Hooks_IPC_ISteamUser::Register();
        Hooks_IPC_ISteamUtils::Register();

        LOG_IPC_INFO("Hooks_IPC: {} handlers registered", g_Handlers.size());

        HOOK_BEGIN();
        INSTALL_HOOK_C(IPCProcessMessage);
        HOOK_END();
    }

    void Uninstall() {
        // 回退场景会重装 hook：不清掉 handler 会越积越多（已实测 6→12），
        // 虽然 dispatch 只取首个匹配无功能影响，但属于泄漏，必须清。
        // hash 跟着一起清，否则重装前遗留的值会指向不存在的表项。
        g_Handlers.clear();
        g_onlineFixAppIdHash = 0;
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(IPCProcessMessage);
        UNHOOK_END();
    }

    void RegisterHandlers(std::span<const IPCHandlerEntry> entries) {
        for (const auto& e : entries) {
            const auto* m = IPCLoader::Find(e.interfaceName, e.methodName);
            if (!m) {
                LOG_IPC_WARN("[Handler Disabled] no IPC spec for {}",e.DebugString());
                continue;
            }
            auto& handler = g_Handlers.emplace_back(e,*m);
            // 记下 GetAppID 的 hash，供 ResolveDispatch 给 onlinefix 精确开口子。
            if (handler.interfaceID == EIPCInterface::IClientUtils &&
                strcmp(e.methodName, "GetAppID") == 0) {
                g_onlineFixAppIdHash = handler.funcHash;
            }
            LOG_IPC_DEBUG("Hooks_IPC: resolved {}", handler.DebugString());
        }
    }

}
