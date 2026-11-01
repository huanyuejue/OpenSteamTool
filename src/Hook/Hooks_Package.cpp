#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "dllmain.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include <atomic>
#include <unordered_set>
#include <windows.h>

namespace {
    RESOLVE_FUNC(CUtlMemoryGrow,               void*, CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    RESOLVE_FUNC(ProcessPendingLicenseUpdates, bool,  void*);

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*,g_pCPackageInfo,void* pThis, uint32 packageId, uint64 accessToken);
    
    void* g_pCUser = nullptr;
    PackageInfo* g_pInjectedPackageInfo = nullptr;
    bool  g_licenseInitialized = false;
    bool  g_licenseRefreshPending = false;
    // 回退自举一次性标志：看门狗线程置位，Steam IPC 线程消费（原子，跨线程）。
    std::atomic<bool> g_forceRequeryOnce{false};
    // 注入熔断：异常触发即永久停注本会话（游戏顶多不解锁，Steam 活着）。
    std::atomic<bool> g_injectDisabled{false};
    static DWORD g_injectExCode = 0;
    static void* g_injectExAddr = nullptr;

    static LONG WINAPI InjectionFilter(EXCEPTION_POINTERS* ep) {
        g_injectExCode = ep->ExceptionRecord->ExceptionCode;
        g_injectExAddr = ep->ExceptionRecord->ExceptionAddress;
        return EXCEPTION_EXECUTE_HANDLER;
    }

    constexpr PackageId_t kInjectedPackageId = 0;
    constexpr uint64_t kInjectedPkgAccessToken = 10660652434190618804ull;

    bool MarkLicenseAsChangedAndProcessUpdates() {
        if (!g_pCUser || !oMarkLicenseAsChanged || !oProcessPendingLicenseUpdates) {
            LOG_PACKAGE_WARN("MarkLicenseAsChangedAndProcessUpdates: dependencies not ready, skipping");
            return false;
        }
        oMarkLicenseAsChanged(g_pCUser, kInjectedPackageId, true);
        oProcessPendingLicenseUpdates(g_pCUser);
        LOG_PACKAGE_DEBUG("MarkLicenseAsChangedAndProcessUpdates: marked package {} as changed and processed updates", kInjectedPackageId);
        return true;
    }

    void TryProcessPendingLicenseRefresh() {
        if (!g_licenseRefreshPending)
            return;
        if (MarkLicenseAsChangedAndProcessUpdates())
            g_licenseRefreshPending = false;
    }

    bool CUtlMemoryGrowWrap(CUtlVector<AppId_t>* pVec, int grow_size) {
        if (!oCUtlMemoryGrow) {
            LOG_PACKAGE_WARN("CUtlMemoryGrow: oCUtlMemoryGrow not ready, cannot grow");
            return false;
        }
        return oCUtlMemoryGrow(pVec, grow_size);
    }

    bool InitFakeLicenseOnce(PackageInfo* pPkg) {
        // check package status before injecting
        if (pPkg->Status != EPackageStatus::Available) {
            LOG_PACKAGE_WARN("InitFakeLicenseOnce: package status is not Available ({}), skipping injection", static_cast<int>(pPkg->Status));
            return false;
        }

        // Inject all depots from config into the fake license. 
        std::vector<AppId_t> appIds = LuaConfig::GetAllDepotIds();
        if (!appIds.empty()) {
            uint32 oldSize = pPkg->AppIdVec.m_Size;
            uint32 numToAdd = static_cast<uint32>(appIds.size());
            LOG_PACKAGE_INFO("InitFakeLicense(PackageId={}): adding {} apps, oldSize={}", kInjectedPackageId, numToAdd, oldSize);
            // 容量门：新构建若改了 CUtlVector/PackageInfo 布局，按旧偏移读写就是堆破坏。
            // 写之前验两遍（grow 前后），离谱直接放弃注入并大声报错，绝不碰内存。
            // 上限按"正常包几百个"取极宽松值，只拦错乱值，不拦正常波动。
            constexpr uint32 kMaxSaneAppCount = 100000;
            const uint32 allocBefore = pPkg->AppIdVec.m_Memory.m_nAllocationCount;
            if (oldSize > allocBefore || numToAdd > kMaxSaneAppCount || allocBefore > kMaxSaneAppCount) {
                LOG_PACKAGE_ERROR("InitFakeLicense: implausible vector state (size={} alloc={} add={}), skipping injection (struct layout may have changed)",
                                  oldSize, allocBefore, numToAdd);
                return false;
            }
            if (!CUtlMemoryGrowWrap(&pPkg->AppIdVec, numToAdd)) {
                LOG_PACKAGE_WARN("InitFakeLicense(PackageId={}): failed to grow AppId vector", kInjectedPackageId);
                return false;
            }
            const uint32 allocAfter = pPkg->AppIdVec.m_Memory.m_nAllocationCount;
            if (allocAfter < oldSize + numToAdd || allocAfter > kMaxSaneAppCount) {
                LOG_PACKAGE_ERROR("InitFakeLicense: grow result implausible (size={} alloc={} add={}), skipping write (keeping original length)",
                                  oldSize, allocAfter, numToAdd);
                return false;
            }
            for (uint32 i = 0; i < numToAdd; i++)
                pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = appIds[i];
            // Grow 只扩容量不改长度，回写长度保证注入的 AppId 对按长度遍历的逻辑可见
            pPkg->AppIdVec.m_Size = oldSize + numToAdd;
        }

        g_licenseInitialized = true;
        g_licenseRefreshPending = true;
        TryProcessPendingLicenseRefresh();
        return true;
    }

    bool TryInitFakeLicenseOnce() {
        if (g_licenseInitialized) return true;
        if(CAPTURE_READY(GetPackageInfo)){
            PackageInfo* pPkg = oGetPackageInfo(g_pCPackageInfo, kInjectedPackageId, kInjectedPkgAccessToken);
            if(!pPkg) {
                LOG_PACKAGE_WARN("TryInitFakeLicenseOnce: GetPackageInfo returned null for injected package");
                return false;
            }
            if(!g_pInjectedPackageInfo) g_pInjectedPackageInfo = pPkg;
            return InitFakeLicenseOnce(pPkg);
        }
        return false;
    }


    // 注入路径全兜底：布局漂移的新构建上，注入及后续处理任何异常都不能带走 Steam。
    // 崩一次就记码记地址、永久停注本会话（游戏顶多不解锁，Steam 活着 + 日志可查）。
} // namespace

namespace {

    static bool CheckAppOwnershipBody(void* pObj, AppId_t appId, AppOwnership* pOwn, bool result) {
        if (!g_pCUser) {
            g_pCUser = pObj;
            LOG_PACKAGE_DEBUG("CheckAppOwnership: captured CUser {}", g_pCUser);
        }

        TryInitFakeLicenseOnce();

        // 自举刷新（只一次）：回退场景下这次调用大概率是第一次，
        // GetPackageInfo capture 还没就绪，注入做了一半。g_pCUser 本次已就位，
        // 强制刷一次 license，第二波查询到来时 capture 就绪，注入完成。
        // exchange 原子取反清零，不可能循环。
        if (!g_licenseInitialized && g_forceRequeryOnce.exchange(false)) {
            LOG_PACKAGE_INFO("CheckAppOwnership: priming license re-query after late hook install");
            MarkLicenseAsChangedAndProcessUpdates();
        }

        if (LuaConfig::HasDepot(appId,false)) {
            // 家庭共享借来的游戏不计入真拥有，避免后续被排除在注入之外
            bool isTrulyOwned = result && (pOwn->ExistInPackageNums > 1) && !pOwn->bFamilyShared && !pOwn->bBorrowed;
            if (isTrulyOwned) {
                // Actually owned — record so HasDepot excludes it going forward
                LuaConfig::MarkOwned(appId);
                pOwn->ReleaseState = EAppReleaseState::Released;
            } else {
                pOwn->PackageId    = kInjectedPackageId;
                pOwn->ReleaseState = EAppReleaseState::Released;
                pOwn->bOwnsLicense = true; // This forces DLCs and enables decoupled family shared games
                // Setting this free flag to false will hide it from the library UI.
                pOwn->bFreeLicense = false;
                pOwn->bFamilyShared   = false;
                pOwn->bBorrowed       = false;
                pOwn->bLicenseLocked  = false;
                // SteamId32 保持原始值：库详情页按所有者是否为本人判定家庭横幅，清零会误触发
                return true;
            }
        }

        // 非 Lua 名单的纯借用游戏同样去锁去标记，压制库锁定与玩法限制
        if (pOwn && (pOwn->bFamilyShared || pOwn->bBorrowed)) {
            if (pOwn->bLicenseLocked) {
                LOG_PACKAGE_DEBUG("CheckAppOwnership: Clearing bLicenseLocked for shared AppId={}", appId);
                pOwn->bLicenseLocked = false;
            }
            pOwn->bBorrowed = false;
            pOwn->bOwnsLicense = true;
            result = true;
        }

        return result;
    }

    HOOK_FUNC(CheckAppOwnership, bool, void* pObj, AppId_t appId, AppOwnership* pOwn) {
        bool result = oCheckAppOwnership(pObj, appId, pOwn);
        // 熔断后直接返回原结果，不再碰任何内存。
        if (g_injectDisabled.load()) return result;
        // 本函数只有 POD 局部量、无 C++ 对象，__try 在此合法（/EHsc）。
        bool out = result;
        __try {
            out = CheckAppOwnershipBody(pObj, appId, pOwn, result);
        } __except (InjectionFilter(GetExceptionInformation())) {
            g_injectDisabled.store(true);
            LOG_PACKAGE_ERROR("CheckAppOwnership: license path crashed (code=0x{:X} addr={}), injection disabled for session",
                              g_injectExCode, g_injectExAddr);
            return result;
        }
        return out;
    }
}

namespace Hooks_Package {
    void Install() {
        RESOLVE_C(CUtlMemoryGrow);
        RESOLVE_C(MarkLicenseAsChanged);
        RESOLVE_C(ProcessPendingLicenseUpdates);

        ARM_CAPTURE_C(GetPackageInfo);

        HOOK_BEGIN();
        INSTALL_HOOK_C(CheckAppOwnership);
        HOOK_END();
    }

    void Uninstall() {
        UNHOOK_BEGIN();
        UNINSTALL_HOOK_C(CheckAppOwnership);
        UNHOOK_END();
    }

    void RequestRequeryOnce() {
        g_forceRequeryOnce.store(true);
    }

    void NotifyLicenseChanged() {
        // 熔断后热重载也不碰内存：直接返回，pending 队列留给下次（量极小，可忽略）。
        if (g_injectDisabled.load()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injection disabled after crash, skipping");
            return;
        }
        PackageInfo* pPkg = g_pInjectedPackageInfo;
        if (!pPkg) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: injected PackageInfo not ready, cannot notify");
            return;
        }

        // ── Remove depots that were unloaded ──
        std::vector<AppId_t> removals = LuaConfig::TakePendingRemovals();
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} removals", removals.size());
        uint32_t removedCount = 0;
        for (AppId_t id : removals) {
            if (pPkg->AppIdVec.FindAndFastRemove(id)) {
                ++removedCount;
                LOG_PACKAGE_DEBUG("NotifyLicenseChanged: removed AppId {}", id);
            }else {
                LOG_PACKAGE_WARN("NotifyLicenseChanged: AppId {} not found in package AppIdVec during removal", id);
            }
        }

        // ── Add depots that are newly loaded ──
        std::vector<AppId_t> additions = LuaConfig::TakePendingAdditions();
        std::unordered_set<AppId_t> addedIds;
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: processing {} additions", additions.size());
        if (!additions.empty()) {
            uint32_t oldSize = pPkg->AppIdVec.m_Size;
            if (CUtlMemoryGrowWrap(&pPkg->AppIdVec, additions.size())) {
                // An applied addition invalidates any UI removal that has not
                // reached the UI thread yet.
                for (AppId_t id : additions)
                    Hooks_SteamUI::CancelRemoval(id);

                for (size_t i = 0; i < additions.size(); ++i) {
                    pPkg->AppIdVec.m_Memory.m_pMemory[oldSize + i] = additions[i];
                    addedIds.insert(additions[i]);
                    LOG_PACKAGE_DEBUG("NotifyLicenseChanged: inserted AppId {} at [{}]", additions[i], oldSize + i);
                }
                // Grow 只扩容量不改长度，回写长度保证注入的 AppId 对按长度遍历的逻辑可见
                pPkg->AppIdVec.m_Size = oldSize + static_cast<uint32_t>(additions.size());
            }else {
                LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to grow AppId vector for additions");
            }
        }

        if (addedIds.empty() && removedCount == 0) {
            LOG_PACKAGE_DEBUG("NotifyLicenseChanged: no changes");
            return;
        }

        // Mark package 0 as changed and trigger library refresh.
        if (!MarkLicenseAsChangedAndProcessUpdates()) {
            LOG_PACKAGE_WARN("NotifyLicenseChanged: failed to mark license as changed");
            return;
        }
        LOG_PACKAGE_INFO("NotifyLicenseChanged: {} added, {} removed", addedIds.size(), removedCount);

        // Queue UI removals for the main-thread RunFrame hook to drain.
        // Never touch MarkAppChange from this (FileWatcher) thread.
        size_t queuedRemovalCount = 0;
        for (AppId_t id : removals) {
            // ParseFile unloads the old file before parsing the replacement.
            // Do not queue that transient removal when the id was added again.
            if (!addedIds.contains(id)) {
                Hooks_SteamUI::QueueRemoval(id);
                ++queuedRemovalCount;
            }
        }
        LOG_PACKAGE_DEBUG("NotifyLicenseChanged: queued {} UI removals, skipped {} transient removals",
                          queuedRemovalCount, removals.size() - queuedRemovalCount);
    }
}
