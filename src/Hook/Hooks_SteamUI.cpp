#include "Hooks_SteamUI.h"
#include "Hooks_Package.h"
#include "OSTPlatform/include/SteamCredentialStore.h"
#include "HookManager.h"
#include "HookMacros.h"
#include "dllmain.h"
#include "steam_messages.pb.h"
#include "Utils/HookSupport/VehCommon.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>
#include <windows.h>

namespace
{
    using namespace std::chrono_literals;
    // 屏障轮询：10ms × 500 = 最多等 5s，超时直接放弃劫持返回原句柄，
    // 绝不能卡死 Steam 自己的模块加载。
    constexpr int kMaxRetry = 500;
    constexpr auto kRetryInterval = 10ms;

    static std::string_view ExtractFileName(std::string_view path) {
        const size_t pos = path.find_last_of("\\/");
        return (pos == std::string_view::npos) ? path : path.substr(pos + 1);
    }

    static std::wstring_view ExtractFileNameW(std::wstring_view path) {
        const size_t pos = path.find_last_of(L"\\/");
        return (pos == std::wstring_view::npos) ? path : path.substr(pos + 1);
    }

    // 文件名长度快判，比后缀扫描便宜；
    // 注意 "steamclient64.dll"=17、"steamclient.dll"=15，数错会直接导致重定向全灭。
    static bool IsSteamClientPath(const char* path) {
        if (!path) return false;
        std::string_view fn = ExtractFileName(path);
        switch (fn.size()) {
        case 17: return _strnicmp(fn.data(), "steamclient64.dll", 17) == 0;
        case 15: return _strnicmp(fn.data(), "steamclient.dll", 15) == 0;
        case 13: return _strnicmp(fn.data(), "steamclient64", 13) == 0;
        case 11: return _strnicmp(fn.data(), "steamclient", 11) == 0;
        default: return false;
        }
    }

    static bool IsSteamClientPathW(const wchar_t* path) {
        if (!path) return false;
        std::wstring_view fn = ExtractFileNameW(path);
        switch (fn.size()) {
        case 17: return _wcsnicmp(fn.data(), L"steamclient64.dll", 17) == 0;
        case 15: return _wcsnicmp(fn.data(), L"steamclient.dll", 15) == 0;
        case 13: return _wcsnicmp(fn.data(), L"steamclient64", 13) == 0;
        case 11: return _wcsnicmp(fn.data(), L"steamclient", 11) == 0;
        default: return false;
        }
    }

    // 系统模块查询重定向：凡是按名字查 steamclient 的，一律给 diversion 副本句柄，
    // 否则 Steam 内部拿到原模块句柄，hook 全部打空。FROM_ADDRESS 按地址查询不受影响。
    // 注意 cloud_redirect 也必须拿副本：它的 vtable hook 只有打在 Steam 实际调用的
    // 模块上才拦得到 Cloud RPC；给它原模块会导致其安装失败（Steam 不用原模块了），
    // 云同步退化到 KV 未就绪的 packet 层，游戏启动报云同步失败。
    static decltype(&GetModuleHandleA) oGetModuleHandleA = &GetModuleHandleA;
    static decltype(&GetModuleHandleW) oGetModuleHandleW = &GetModuleHandleW;
    static decltype(&GetModuleHandleExA) oGetModuleHandleExA = &GetModuleHandleExA;
    static decltype(&GetModuleHandleExW) oGetModuleHandleExW = &GetModuleHandleExW;

    HMODULE WINAPI hkGetModuleHandleA(LPCSTR lpModuleName)
    {
        if (client_hModule && IsSteamClientPath(lpModuleName)) {
            return static_cast<HMODULE>(client_hModule);
        }
        return oGetModuleHandleA(lpModuleName);
    }

    HMODULE WINAPI hkGetModuleHandleW(LPCWSTR lpModuleName)
    {
        if (client_hModule && IsSteamClientPathW(lpModuleName)) {
            return static_cast<HMODULE>(client_hModule);
        }
        return oGetModuleHandleW(lpModuleName);
    }

    BOOL WINAPI hkGetModuleHandleExA(DWORD dwFlags, LPCSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPath(lpModuleName))
        {
            if (!phModule) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
                (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                HMODULE dummy = nullptr;
                if (!oGetModuleHandleExA((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                         reinterpret_cast<LPCSTR>(client_hModule), &dummy))
                {
                    *phModule = nullptr;
                    return FALSE;
                }
            }
            *phModule = static_cast<HMODULE>(client_hModule);
            return TRUE;
        }
        return oGetModuleHandleExA(dwFlags, lpModuleName, phModule);
    }

    BOOL WINAPI hkGetModuleHandleExW(DWORD dwFlags, LPCWSTR lpModuleName, HMODULE* phModule)
    {
        if (client_hModule && !(dwFlags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) &&
            IsSteamClientPathW(lpModuleName))
        {
            if (!phModule) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if ((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) &&
                (dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT))
            {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            if (!(dwFlags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT)) {
                HMODULE dummy = nullptr;
                if (!oGetModuleHandleExW((dwFlags & GET_MODULE_HANDLE_EX_FLAG_PIN) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                         reinterpret_cast<LPCWSTR>(client_hModule), &dummy))
                {
                    *phModule = nullptr;
                    return FALSE;
                }
            }
            *phModule = static_cast<HMODULE>(client_hModule);
            return TRUE;
        }
        return oGetModuleHandleExW(dwFlags, lpModuleName, phModule);
    }

    // ── 首载抢劫（1791 隔离修复）─────────────────────────
    // 1791 的 steam.exe 不再走 LoadModuleWithPath 拿 steamclient，而是启动期
    // 直接 LoadLibraryExW，发生在劫持视野之外，adoption 永远等不来。
    // 把 kernel32 的首载也挂上：凡是要 steamclient 的首载，直接给副本句柄。
    // GetProcAddress 定位 kernel32 函数，不走 pattern（系统库地址精确、无需 toml）。
    static decltype(&LoadLibraryExW) oLoadLibraryExW = nullptr;
    static decltype(&LoadLibraryW) oLoadLibraryW = nullptr;
    static decltype(&LoadLibraryExA) oLoadLibraryExA = nullptr;
    static decltype(&LoadLibraryA) oLoadLibraryA = nullptr;
    static std::atomic<bool> g_bypassFirstLoadRedirect{false};

    static HMODULE TryDivertFirstLoad(LPCWSTR lpLibFileName) {
        if (g_bypassFirstLoadRedirect.load(std::memory_order_acquire)) return nullptr;
        if (!g_IsDiversionActive.load() || !client_hModule) return nullptr;
        if (!IsSteamClientPathW(lpLibFileName)) return nullptr;
        // 首载语义：原版已经在内存里，这次就是重复请求，直接放行拿原版。
        // 不然（比如我们来晚了、启动期那次没抢到）就会造出“主 client 跑原版、
        // 某个后来者拿副本”的分裂世界，还会把 adopted 置了压住回退，解锁全灭。
        // 这个探测必须走 oGetModuleHandleW（真 API / trampoline）：按名查询已被
        // 本模块重定向到副本，隔离激活时字面量调用永远命中副本（非 null），
        // 抢劫会被自己的重定向骗成"原版已在"，一次都抢不出去。
        auto realGetModuleHandleW = oGetModuleHandleW ? oGetModuleHandleW : &GetModuleHandleW;
        if (realGetModuleHandleW(lpLibFileName) || realGetModuleHandleW(L"steamclient64.dll"))
            return nullptr;
        // pin 住副本再交出去：Steam 之后 FreeLibrary 只会降到我们的 pin，
        // 副本常驻，绝不被卸（和“绝不 FreeLibrary 副本”是同一条命）。
        // 直接调真实 API（:: 前缀也绕不过 Detours 的函数体补丁，但 FROM_ADDRESS
        // 会让劫持直接放行走原函数，所以两种指针都安全；判空只是防野指针）。
        HMODULE dummy = nullptr;
        auto pinFn = oGetModuleHandleExW ? oGetModuleHandleExW : ::GetModuleHandleExW;
        pinFn(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
              reinterpret_cast<LPCWSTR>(client_hModule), &dummy);
        // 走 main logger：武装已提前到模块日志就绪之前，这条随时可能触发。
        LOG_INFO("FirstLoadRedirect: diverted steamclient first load -> diversion {} (adopted)",
                 static_cast<void*>(client_hModule));
        g_DiversionAdopted.store(true);
        return static_cast<HMODULE>(client_hModule);
    }

    HMODULE WINAPI hkLoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
    {
        // 资源型/数据文件型加载不能劫：那种要的是文件内容不是可执行模块。
        constexpr DWORD kDataFlags = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE |
                                     LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE;
        if (!hFile && !(dwFlags & kDataFlags)) {
            if (HMODULE diverted = TryDivertFirstLoad(lpLibFileName))
                return diverted;
        }
        return oLoadLibraryExW(lpLibFileName, hFile, dwFlags);
    }

    HMODULE WINAPI hkLoadLibraryW(LPCWSTR lpLibFileName)
    {
        if (HMODULE diverted = TryDivertFirstLoad(lpLibFileName))
            return diverted;
        return oLoadLibraryW(lpLibFileName);
    }

    // ANSI 镜像：steam.exe 的导入表里 A 系列 LoadLibrary 同样在用，
    // 只拦 W 版会漏掉走 A 版发起的首载。
    static HMODULE TryDivertFirstLoadA(LPCSTR lpLibFileName) {
        if (g_bypassFirstLoadRedirect.load(std::memory_order_acquire)) return nullptr;
        if (!g_IsDiversionActive.load() || !client_hModule) return nullptr;
        if (!IsSteamClientPath(lpLibFileName)) return nullptr;
        auto realGetModuleHandleA = oGetModuleHandleA ? oGetModuleHandleA : &GetModuleHandleA;
        if (realGetModuleHandleA(lpLibFileName) || realGetModuleHandleA("steamclient64.dll"))
            return nullptr;
        HMODULE dummy = nullptr;
        auto pinFn = oGetModuleHandleExA ? oGetModuleHandleExA : ::GetModuleHandleExA;
        pinFn(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
              reinterpret_cast<LPCSTR>(client_hModule), &dummy);
        LOG_INFO("FirstLoadRedirect: diverted steamclient first load (ANSI) -> diversion {} (adopted)",
                 static_cast<void*>(client_hModule));
        g_DiversionAdopted.store(true);
        return static_cast<HMODULE>(client_hModule);
    }

    HMODULE WINAPI hkLoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags)
    {
        constexpr DWORD kDataFlags = LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE |
                                     LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE;
        if (!hFile && !(dwFlags & kDataFlags)) {
            if (HMODULE diverted = TryDivertFirstLoadA(lpLibFileName))
                return diverted;
        }
        return oLoadLibraryExA(lpLibFileName, hFile, dwFlags);
    }

    HMODULE WINAPI hkLoadLibraryA(LPCSTR lpLibFileName)
    {
        if (HMODULE diverted = TryDivertFirstLoadA(lpLibFileName))
            return diverted;
        return oLoadLibraryA(lpLibFileName);
    }

    // ── API 面重定向（M3）：晚绑定的 GetProcAddress 也要罩住 ──────────
    // 有人先拿到原版句柄（FROM_ADDRESS 查询、启动期直连）再 GetProcAddress
    // 取 CreateInterface 等导出时，直接给副本的实现，否则会出现“模块走了
    // 副本、接口却调回原版”的半隔离世界。我只在隔离生效时改道，其余一律
    // 透传；用句柄等值比对定位原版，不读文件名，热路径上只多一次原子读。
    static decltype(&GetProcAddress) oGetProcAddress = nullptr;

    FARPROC WINAPI hkGetProcAddress(HMODULE hModule, LPCSTR lpProcName)
    {
        if (lpProcName && hModule && client_hModule &&
            g_IsDiversionActive.load(std::memory_order_acquire) &&
            !g_bypassFirstLoadRedirect.load(std::memory_order_acquire) &&
            hModule != static_cast<HMODULE>(client_hModule) && oGetProcAddress)
        {
            HMODULE orig = nullptr;
            if (oGetModuleHandleW) {
                orig = oGetModuleHandleW(L"steamclient64.dll");
                if (!orig)
                    orig = oGetModuleHandleW(L"steamclient.dll");
            }
            if (orig && hModule == orig) {
                if (FARPROC diverted = oGetProcAddress(
                        static_cast<HMODULE>(client_hModule), lpProcName))
                    return diverted;
            }
        }
        return oGetProcAddress(hModule, lpProcName);
    }

    HOOK_FUNC(LoadModuleWithPath, void*, const char* path, bool flags)
    {
        LOG_STEAMUI_INFO("LoadModuleWithPath called with path: {}, flags: {}",
                         path ? path : "(null)", flags);

        const bool isSteamClient = IsSteamClientPath(path);

        if (isSteamClient) {
            // 等 client 侧 hook 装完；超时则放弃劫持返回原句柄，绝不卡死。
            bool logged = false;
            for (int i = 0; i < kMaxRetry && !g_HooksInstalled.load(); ++i) {
                if (!logged) {
                    LOG_STEAMUI_DEBUG("LoadModuleWithPath: waiting for hooks to be installed...");
                    logged = true;
                }
                std::this_thread::sleep_for(kRetryInterval);
            }
        }

        void* h = oLoadModuleWithPath(path, flags);

        if (isSteamClient) {
            if (!g_HooksInstalled.load()) {
                LOG_STEAMUI_WARN("LoadModuleWithPath: hooks initialization timed out after {} ms, aborting diversion",
                                 kMaxRetry * static_cast<int>(kRetryInterval.count()));
                return h;
            }

            if (client_hModule) {
                if (g_IsDiversionActive.load()) {
                    LOG_STEAMUI_INFO("LoadModuleWithPath: diverted {} (original {}) -> diversion {}{}",
                                     path ? path : "steamclient64.dll", h, client_hModule,
                                     (h == client_hModule) ? " (already loaded)" : "");
                    // Steam 换到副本上 = 隔离生效，超时回退看门狗不再动作。
                    g_DiversionAdopted.store(true);
                } else {
                    LOG_STEAMUI_INFO("LoadModuleWithPath: returned fallback steamclient64.dll ({})",
                                     client_hModule);
                }
                return client_hModule;
            }
        }

        return h;
    }

    RESOLVE_FUNC(RepeatedFieldUint32_Add, void, void* field, const uint32* value);

    CAPTURE_THIS_FUNC(GetAppByID, CSteamApp*, g_pController,void* pThis, AppId_t appId, bool bCreate);
    CAPTURE_THIS_FUNC(MarkAppChange,void*,g_pAppChangeSource,void* pThis,AppId_t appId, EAppChangeFlags changeFlags);

    // 当前登录账号，取一次缓存住。界面刷新会反复走到补充归属那段，
    // 而账号在进程生命周期内不会变，没必要每次都读注册表。
    // 卸载时清掉，热重载后重新取，避免沿用上一轮的账号。
    std::atomic<uint32_t> g_selfAccountId{0};

    HOOK_FUNC(FillInAppOverview, void *, void *pThis, void *pAppOverview, CSteamApp *pApp)
    {
        if (pApp && LuaConfig::HasDepot(pApp->nAppID, false))
        {
            // 所有者为空身份时，详情页会挂一条"来自家庭库"。假入库的游戏本来就没有
            // 归属，Steam 填的是 accountid 为 0 的占位值，这里补成当前登录账号。
            // 真正从别人那借来的游戏不能碰：它们有出借方记录，界面就该显示共享来源。
            if (pApp->SteamID.GetAccountID() == 0 &&
                Hooks_Package::GetFamilySharedOwner(pApp->nAppID) == 0)
            {
                uint32_t selfAccount = g_selfAccountId.load(std::memory_order_relaxed);
                if (selfAccount == 0)
                {
                    std::wstring universe;
                    if (OSTPlatform::SteamCredentialStore::GetActiveUser(selfAccount, universe) ==
                        OSTPlatform::SteamCredentialStore::Status::Ok)
                        g_selfAccountId.store(selfAccount, std::memory_order_relaxed);
                    else
                        selfAccount = 0;
                }

                if (selfAccount != 0)
                {
                    pApp->SteamID.Set(selfAccount, k_EUniversePublic, k_EAccountTypeIndividual);
                    LOG_STEAMUI_DEBUG("FillInAppOverview: appId={} owner set to self ({})",
                                      pApp->nAppID, selfAccount);
                }
            }

            uint32_t t = LuaConfig::GetPurchaseTime(pApp->nAppID);
            if (t)
            {
                pApp->PurchasedTime = t;
                LOG_STEAMUI_TRACE("FillInAppOverview: set PurchasedTime={} for appId={}",
                                  pApp->PurchasedTime, pApp->nAppID);
            }
        }
        return oFillInAppOverview(pThis, pAppOverview, pApp);
    }

    // Apps to drop from the library: queued off-thread, marked on the UI thread.
    std::mutex g_removalMutex;
    std::vector<AppId_t> g_pendingRemovals;
    std::unordered_set<AppId_t> g_removedAppIds;

    // A full rebuild never lists removed_appid for apps still in the map
    // so re-assert our set after the snapshot is built.
    HOOK_FUNC(BuildCompleteAppOverviewChange, void, void *pController,
              CAppOverview_Change *pChange, void *optionalCallbackSlot)
    {
        oBuildCompleteAppOverviewChange(pController, pChange, optionalCallbackSlot);
        std::lock_guard<std::mutex> lock(g_removalMutex);
        if (pChange && !g_removedAppIds.empty() && oRepeatedFieldUint32_Add)
        {
            auto* field = pChange->mutable_removed_appid();
            for (AppId_t appId : g_removedAppIds){
                oRepeatedFieldUint32_Add(field, &appId);
            }
            LOG_STEAMUI_DEBUG("BuildCompleteAppOverviewChange: appended {} removed_appid entries",
                              g_removedAppIds.size());
        }
    }


    // Clearing ownership makes ShouldShowAppInLibrary() false (delta drops it,
    // the full snapshot skips it); MarkAppChange triggers the flush.
    HOOK_FUNC(CSteamUIAppControllerRunFrame, void *, void *pController)
    {
        if (CAPTURE_READY(GetAppByID) && CAPTURE_READY(MarkAppChange))
        {
            std::vector<AppId_t> draining;
            {
                std::lock_guard<std::mutex> lock(g_removalMutex);
                draining.swap(g_pendingRemovals);
            }
            for (AppId_t appId : draining)
            {
                if (LuaConfig::IsOwned(appId))
                {
                    LOG_STEAMUI_DEBUG("RunFrame: appId {} is owned again, skipping removal", appId);
                    continue;
                }
                if (CSteamApp *pApp = oGetAppByID(g_pController, appId, false))
                {
                    // Only remove from the library if it's not already uninstalled
                    pApp->OwnershipFlags = k_EAppOwnershipFlags_None;
                    if(pApp->AppStateFlags == k_EAppStateUninstalled){
                        std::lock_guard<std::mutex> lock(g_removalMutex);
                        g_removedAppIds.insert(appId);
                    }
                }
                
                oMarkAppChange(g_pAppChangeSource, appId, EAppChangeFlags::AppInfoOrConfig);
            }
        }
        return oCSteamUIAppControllerRunFrame(pController);
    }
}

namespace Hooks_SteamUI
{
    void Install()
    {
        ARM_CAPTURE_U(GetAppByID);
        ARM_CAPTURE_U(MarkAppChange);

        RESOLVE_U(RepeatedFieldUint32_Add);

        HOOK_BEGIN();
        INSTALL_HOOK_U(LoadModuleWithPath);
        INSTALL_HOOK_U(FillInAppOverview);
        INSTALL_HOOK_U(BuildCompleteAppOverviewChange);
        INSTALL_HOOK_U(CSteamUIAppControllerRunFrame);
        // 系统模块句柄重定向（Diversion 影子隔离）：必须在同一个事务里，
        // DetourAttach 脱离事务直接调会失败（DetourAttach failed），hook 等于没装。
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW));
        HOOK_END();
    }

    bool IsDiversionRedirectArmed()
    {
        return oLoadModuleWithPath != nullptr;
    }

    void InstallFirstLoadRedirect()
    {
        // kernel32 常驻，GetProcAddress 精确定位，不需要 pattern/toml；
        // 与 SteamUI hook 无依赖，注入后就越早装越好——武装越早，抢到
        // Steam 首载的窗口越大。日志一律走 main logger：它可能在模块
        // 日志就绪之前触发。
        const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) {
            LOG_WARN("FirstLoadRedirect: kernel32 handle missing, skipping");
            return;
        }
        oLoadLibraryExW = reinterpret_cast<decltype(oLoadLibraryExW)>(
            GetProcAddress(kernel32, "LoadLibraryExW"));
        oLoadLibraryW = reinterpret_cast<decltype(oLoadLibraryW)>(
            GetProcAddress(kernel32, "LoadLibraryW"));
        oLoadLibraryExA = reinterpret_cast<decltype(oLoadLibraryExA)>(
            GetProcAddress(kernel32, "LoadLibraryExA"));
        oLoadLibraryA = reinterpret_cast<decltype(oLoadLibraryA)>(
            GetProcAddress(kernel32, "LoadLibraryA"));
        oGetProcAddress = reinterpret_cast<decltype(oGetProcAddress)>(
            GetProcAddress(kernel32, "GetProcAddress"));
        if (!oLoadLibraryExW || !oLoadLibraryW) {
            LOG_WARN("FirstLoadRedirect: GetProcAddress failed, skipping");
            oLoadLibraryExW = nullptr;
            oLoadLibraryW = nullptr;
            oLoadLibraryExA = nullptr;
            oLoadLibraryA = nullptr;
            oGetProcAddress = nullptr;
            return;
        }
        HOOK_BEGIN();
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oLoadLibraryExW),
                                    reinterpret_cast<void*>(hkLoadLibraryExW));
        OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oLoadLibraryW),
                                    reinterpret_cast<void*>(hkLoadLibraryW));
        // A 系列是镜像补充，装不上不影响 W 版主路径。
        if (oLoadLibraryExA) {
            OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oLoadLibraryExA),
                                        reinterpret_cast<void*>(hkLoadLibraryExA));
        }
        if (oLoadLibraryA) {
            OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oLoadLibraryA),
                                        reinterpret_cast<void*>(hkLoadLibraryA));
        }
        // API 面兜底：晚绑定取原版导出时改道副本；缺了它也不影响首载逻辑。
        if (oGetProcAddress) {
            OSTPlatform::Detour::Attach(reinterpret_cast<void**>(&oGetProcAddress),
                                        reinterpret_cast<void*>(hkGetProcAddress));
        }
        HOOK_END();
        LOG_INFO("FirstLoadRedirect: armed (steamclient first load will be diverted)");
    }

    bool IsFirstLoadRedirectArmed()
    {
        return oLoadLibraryExW != nullptr;
    }

    void SetFirstLoadRedirectBypass(bool bypass)
    {
        g_bypassFirstLoadRedirect.store(bypass, std::memory_order_release);
    }

    void Uninstall()
    {
        // 热重载后要重新取账号，避免沿用上一轮缓存的值。
        g_selfAccountId.store(0, std::memory_order_relaxed);
        UNHOOK_BEGIN();
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleA), reinterpret_cast<void*>(hkGetModuleHandleA));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleW), reinterpret_cast<void*>(hkGetModuleHandleW));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExA), reinterpret_cast<void*>(hkGetModuleHandleExA));
        OSTPlatform::Detour::Detach(reinterpret_cast<void**>(&oGetModuleHandleExW), reinterpret_cast<void*>(hkGetModuleHandleExW));
        UNINSTALL_HOOK(LoadModuleWithPath);
        UNINSTALL_HOOK(LoadLibraryExW);
        UNINSTALL_HOOK(LoadLibraryW);
        UNINSTALL_HOOK(LoadLibraryExA);
        UNINSTALL_HOOK(LoadLibraryA);
        UNINSTALL_HOOK(GetProcAddress);
        UNINSTALL_HOOK(FillInAppOverview);
        UNINSTALL_HOOK(BuildCompleteAppOverviewChange);
        UNINSTALL_HOOK(CSteamUIAppControllerRunFrame);
        UNHOOK_END();
    }

    void QueueRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        g_pendingRemovals.push_back(appId);
    }

    void CancelRemoval(AppId_t appId)
    {
        std::lock_guard<std::mutex> lock(g_removalMutex);
        std::erase(g_pendingRemovals, appId);
        g_removedAppIds.erase(appId);
    }
}
