#include "Hooks_Package.h"
#include "HookMacros.h"
#include "Hooks_SteamUI.h"
#include "dllmain.h"
#include "Utils/HookSupport/VehCommon.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

namespace {
    RESOLVE_FUNC(CUtlMemoryGrow,               void*, CUtlVector<AppId_t>*, int);
    RESOLVE_FUNC(MarkLicenseAsChanged,         int64, void*, uint32, bool);
    RESOLVE_FUNC(ProcessPendingLicenseUpdates, bool,  void*);

    CAPTURE_THIS_FUNC(GetPackageInfo, PackageInfo*,g_pCPackageInfo,void* pThis, uint32 packageId, uint64 accessToken);
    
    void* g_pCUser = nullptr;
    PackageInfo* g_pInjectedPackageInfo = nullptr;

    // 家庭共享的出借方 accountid，按 AppId 记录。
    // Steam 在 bFamilyShared 置位期间把 AppOwnership::SteamId32 填成出借人，
    // 我这边把标记清掉之后，只有这个字段还能反查归属。上报游玩状态时要用它
    // 把 owner 还原成真实出借方，否则服务器会认为是一个没有许可的账号在跑
    // 共享游戏。
    std::unordered_map<AppId_t, AccountID_t> g_familySharedOwners;
    std::mutex g_familySharedOwnersMutex;

    // 这个 AppId 有没有出借方记录。判定自购时要用：共享游戏的 bFamilyShared
    // 在首轮就被清掉了，后面再查标记认不出来，只能靠这张表兜。
    bool HasFamilySharedOwner(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_familySharedOwnersMutex);
        return g_familySharedOwners.find(appId) != g_familySharedOwners.end();
    }

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

        // 记下出借方归属。必须排在下面清标记之前：清完就再也认不出来了。
        // 这张表有两个用途：
        //   1. 非 lua 的共享游戏，上报时把 owner 还原成出借方，避免倒计时踢线
        //   2. 判断某个 AppId 是不是家庭库里的共享游戏（lua 游戏也要记，
        //      上报时据此改写成替代 AppId，避免占用出借方名额）
        // 只认借用标记：清掉之后 SteamId32 会被 Steam 改写，拿它做判断不可信。
        // 记录写下后不再改动——漏记会让共享修复整体失效，宁可留着多余的。
        if (pOwn && (pOwn->bFamilyShared || pOwn->bBorrowed) && pOwn->SteamId32 != 0) {
            bool recorded = false;
            {
                std::lock_guard<std::mutex> lock(g_familySharedOwnersMutex);
                AccountID_t& slot = g_familySharedOwners[appId];
                if (slot != pOwn->SteamId32) {
                    slot = pOwn->SteamId32;
                    recorded = true;
                }
            }
            if (recorded) {
                LOG_PACKAGE_INFO("FamilySharing: recorded lender {} for AppId={} (familyShared={} borrowed={})",
                                 pOwn->SteamId32, appId, pOwn->bFamilyShared, pOwn->bBorrowed);
            }
        }

        // 第二个参数传 true：已经标记为真拥有的 AppId 直接跳过，不再重复判定。
        if (pOwn && LuaConfig::HasDepot(appId, true)) {
            // 真拥有的游戏写进 lua 通常是为了补 DLC，主游戏本身不该改走假入库：
            // 那样会把 Steam 的许可判定覆盖掉，购买状态、成就、时长都会挂到假许可上。
            //
            // 判据要同时满足三条，缺一不可：
            //   1. result：Steam 认这条许可（假入库的也认，所以单看它不够）
            //   2. ExistInPackageNums > 1：真实包和假许可包同时覆盖
            //   3. 没有出借方记录：共享游戏也会因为注入假包让这个值涨到 2，
            //      而且它的 bFamilyShared 在首轮就被我们清掉了，光看标记认不出来。
            //      g_familySharedOwners 里的记录在清标记之前就写下了，不会漏。
            const bool borrowed = pOwn->bFamilyShared || pOwn->bBorrowed;
            const bool knownShared = HasFamilySharedOwner(appId);
            if (result && pOwn->ExistInPackageNums > 1 && !borrowed && !knownShared) {
                LuaConfig::MarkOwned(appId);
                pOwn->ReleaseState = EAppReleaseState::Released;
                LOG_PACKAGE_INFO("FakeLicense: AppId={} 自己拥有，按正版处理", appId);
                return result;
            }

            // 剩下的走假入库：家庭库里也有的游戏因此不会占用出借方名额。
            // 借用相关的标记必须一并抹掉，否则 Steam 后续仍会按共享许可
            // 追踪这条 AppId，占用和到期收回照旧发生。
            pOwn->PackageId          = kInjectedPackageId;
            pOwn->ReleaseState       = EAppReleaseState::Released;
            pOwn->bOwnsLicense       = true;
            pOwn->bFreeLicense       = false;
            pOwn->bFamilyShared      = false;
            pOwn->bBorrowed          = false;
            pOwn->bLicenseLocked     = false;
            pOwn->bLicensePending    = false;
            pOwn->bLicenseExpired    = false;
            pOwn->bGuestPass         = false;
            pOwn->bLicensePermanent  = true;
            return true;
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

    AccountID_t GetFamilySharedOwner(AppId_t appId) {
        std::lock_guard<std::mutex> lock(g_familySharedOwnersMutex);
        auto it = g_familySharedOwners.find(appId);
        return it == g_familySharedOwners.end() ? 0 : it->second;
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
