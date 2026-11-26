// SPDX-License-Identifier: GPL-3.0-only
// Exercise the real package and manifest hooks; replace only Steam/platform calls.
#include "../src/Hook/Hooks_Package.cpp"
#include "../src/Hook/Hooks_Manifest.cpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace {
    constexpr AppId_t kApp = 1962700;
    constexpr AppId_t kDepot = 1962701;
    int user, otherUser, packageStore;
    PackageInfo package{};
    AppId_t packageApps[64]{};
    std::vector<AppId_t> configured{kApp, kDepot}, additions, removals, queuedRemovals;
    std::unordered_map<uint64_t, LuaConfig::ManifestOverride> overrides;
    bool metadataReady, indexReady, branchRemoved, originalBuildResult, owned;
    bool nativeDepotLicenseGranted;
    int processCalls, markCalls;
    bool (*processHook)(void*);
    bool (*ownershipHook)(void*, AppId_t, AppOwnership*);
    bool (*depotLicenseHook)(void*, AppId_t, AppId_t);

    void Require(bool condition, const char* message) {
        if (!condition) {
            std::fprintf(stderr, "FAIL: %s\n", message);
            std::exit(1);
        }
    }

    void* Grow(CUtlVector<AppId_t>* vector, int count) {
        Require(vector->m_Size + count <= vector->m_Memory.m_nAllocationCount,
                "test package capacity");
        return vector->m_Memory.m_pMemory;
    }
    int64 Mark(void*, uint32, bool) { ++markCalls; return 0; }
    bool Process(void* owner) {
        ++processCalls;
        if (metadataReady && owner == &user) indexReady = true;
        return metadataReady;
    }
    PackageInfo* GetPackage(void*, uint32, uint64) { return &package; }
    bool Ownership(void*, AppId_t, AppOwnership*) { return false; }
    bool DepotLicense(void*, AppId_t, AppId_t) { return nativeDepotLicenseGranted; }
    bool Build(void*, AppId_t app, void*, CUtlVector<DepotEntry>*,
               CUtlVector<DepotEntry>*, void*, uint32*, bool* fallback) {
        AppOwnership ownership{};
        ownershipHook(&user, app, &ownership);
        *fallback = branchRemoved || !indexReady;
        return originalBuildResult;
    }

    bool ProcessFrame(void* owner = &user) {
        return processHook ? processHook(owner) : Process(owner);
    }
    bool Update(AppId_t app, std::string& selected, DepotEntry* entries = nullptr,
                uint32 count = 0) {
        CUtlVector<DepotEntry> depots{{entries, count, 0}, count};
        bool fallback = false;
        bool result = hkBuildDepotDependency(nullptr, app, nullptr, &depots,
                                             nullptr, nullptr, nullptr, &fallback);
        // Steam only commits BetaKey removal after a successful dependency build.
        if (result && fallback) selected = "public";
        return result;
    }
    void Reset() {
        package = {};
        package.Status = EPackageStatus::Available;
        package.AppIdVec = {{packageApps, 64, 0}, 0};
        configured = {kApp, kDepot};
        additions.clear(); removals.clear(); queuedRemovals.clear(); overrides.clear();
        metadataReady = indexReady = branchRemoved = owned = false;
        nativeDepotLicenseGranted = false;
        originalBuildResult = true;
        processCalls = markCalls = 0;
        g_pCUser = nullptr;
        g_pInjectedPackageInfo = nullptr;
        g_licenseInitialized = false;
        g_licenseRefreshPending = false;
        g_injectDisabled = false;
        g_forceRequeryOnce = false;
    }
}

namespace LuaConfig {
    bool HasDepot(AppId_t id, bool excludeOwned) {
        return std::find(configured.begin(), configured.end(), id) != configured.end()
            && !(excludeOwned && owned);
    }
    void MarkOwned(AppId_t) {}
    std::vector<AppId_t> GetAllDepotIds() { return configured; }
    std::vector<AppId_t> TakePendingAdditions() { return std::exchange(additions, {}); }
    std::vector<AppId_t> TakePendingRemovals() { return std::exchange(removals, {}); }
    const std::unordered_map<uint64_t, ManifestOverride>& GetManifestOverrides() { return overrides; }
}
namespace Hooks_SteamUI {
    void CancelRemoval(AppId_t) {}
    void QueueRemoval(AppId_t id) { queuedRemovals.push_back(id); }
}
namespace PatternLoader {
    void* FindPattern(OSTPlatform::DynamicLibrary::ModuleHandle, const char* name) {
        const std::string_view value(name);
        if (value == "CUtlMemoryGrow") return reinterpret_cast<void*>(Grow);
        if (value == "MarkLicenseAsChanged") return reinterpret_cast<void*>(Mark);
        if (value == "ProcessPendingLicenseUpdates") return reinterpret_cast<void*>(Process);
        if (value == "GetPackageInfo") return reinterpret_cast<void*>(GetPackage);
        if (value == "CheckAppOwnership") return reinterpret_cast<void*>(Ownership);
        if (value == "DepotLicenseGate") return reinterpret_cast<void*>(DepotLicense);
        if (value == "BuildDepotDependency") return reinterpret_cast<void*>(Build);
        Require(false, "unexpected new hook dependency");
        return nullptr;
    }
}
namespace OSTPlatform::Detour {
    bool BeginTransaction() { return true; }
    bool CommitTransaction() { return true; }
    bool Attach(void** original, void* replacement) {
        if (*original == reinterpret_cast<void*>(Process))
            processHook = reinterpret_cast<bool(*)(void*)>(replacement);
        if (*original == reinterpret_cast<void*>(Ownership))
            ownershipHook = reinterpret_cast<bool(*)(void*, AppId_t, AppOwnership*)>(replacement);
        if (*original == reinterpret_cast<void*>(DepotLicense))
            depotLicenseHook = reinterpret_cast<bool(*)(void*, AppId_t, AppId_t)>(replacement);
        return true;
    }
    bool Detach(void** original, void*) {
        if (*original == reinterpret_cast<void*>(Process)) processHook = nullptr;
        if (*original == reinterpret_cast<void*>(Ownership)) ownershipHook = nullptr;
        if (*original == reinterpret_cast<void*>(DepotLicense)) depotLicenseHook = nullptr;
        return true;
    }
}
namespace VehCommon {
    void Arm(Int3Site site) { *static_cast<void**>(site.callbackData) = &packageStore; }
}
namespace OSTPlatform::Trap {
    uint64_t Context::Argument(int) const { return 0; }
}

int main() {
    Hooks_Package::Install();
    Hooks_Manifest::Install();
    Require(processHook && ownershipHook && depotLicenseHook,
            "install retains readiness, ownership and depot license hooks");
    nativeDepotLicenseGranted = true;
    Require(depotLicenseHook(&user, 999, kApp),
            "depot gate retains native grants for unrelated depots");
    Reset();
    std::string selected = "experimental";
    Require(!Update(kApp, selected), "unpublished license index must defer beta fallback");
    Require(selected == "experimental", "pending license must preserve the selected beta");
    Require(g_licenseRefreshPending, "false refresh result must remain pending");
    Require(package.AppIdVec.m_Size == 2 && processCalls == 1,
            "initialization appends once and attempts one refresh");
    Require(!ProcessFrame() && g_licenseRefreshPending, "natural retry can still be pending");
    metadataReady = true;
    Require(ProcessFrame(&otherUser) && g_licenseRefreshPending,
            "another user's refresh must not acknowledge this package");
    Require(ProcessFrame() && !g_licenseRefreshPending,
            "Steam's successful retry must publish readiness");
    Require(Update(kApp, selected) && selected == "experimental", "resolved beta must proceed");
    Require(package.AppIdVec.m_Size == 2 && markCalls == 1, "no duplicate injection or forced retry loop");

    branchRemoved = true;
    Require(Update(kApp, selected) && selected == "public",
            "real fallback must proceed after readiness even with no depot targets");

    Reset();
    selected = "experimental";
    Update(kApp, selected);
    // Public selection produces no beta-fallback request, even during pending refresh.
    indexReady = true;
    selected = "public";
    Require(Update(kApp, selected) && selected == "public", "explicit public selection must proceed");
    indexReady = false;
    selected = "removed-beta";
    Require(Update(999, selected) && selected == "public", "unrelated games keep native behavior");
    owned = true;
    selected = "removed-beta";
    Require(Update(kApp, selected) && selected == "public", "owned games keep native fallback");
    owned = false;

    originalBuildResult = false;
    selected = "experimental";
    Require(!Update(kApp, selected) && selected == "experimental", "native build failure is preserved");
    originalBuildResult = true;
    DepotEntry mixed[2]{};
    mixed[0].bNotNewTarget = 1;
    Require(!Update(kApp, selected, mixed, 2), "pending refresh also covers partially resolved targets");

    indexReady = true;
    mixed[0].DepotId = kDepot;
    mixed[0].ManifestGid = 10;
    mixed[0].ManifestSize = 100;
    overrides[kDepot] = {20, 0};
    Require(Update(kApp, selected, mixed, 2) && mixed[0].ManifestGid == 20 && mixed[0].ManifestSize == 100,
            "ordinary manifest overrides retain their behavior");

    metadataReady = true;
    ProcessFrame();
    metadataReady = false;
    removals = {kDepot};
    additions = {1962702};
    configured = {kApp, 1962702};
    Hooks_Package::NotifyLicenseChanged();
    Require(!g_licenseRefreshPending, "hot reload must not reopen the startup gate");
    Require(queuedRemovals == std::vector<AppId_t>{kDepot},
            "pending refresh must not lose the existing UI removal queue");
    metadataReady = true;
    Require(ProcessFrame() && !g_licenseRefreshPending, "hot reload still completes naturally");

    Reset();
    metadataReady = true;
    selected = "experimental";
    Require(Update(kApp, selected) && !g_licenseRefreshPending && processCalls == 1,
            "synchronous initialization adds no extra update pass");
    Reset();
    selected = "experimental";
    Update(kApp, selected);
    g_injectDisabled = true;
    Require(Update(kApp, selected) && selected == "public",
            "disabled injection must not leave an active startup gate");
    Hooks_Manifest::Uninstall();
    Hooks_Package::Uninstall();
    Require(!processHook && !ownershipHook && !depotLicenseHook,
            "uninstall removes all package hooks");
    std::puts("License readiness hook regression scenarios passed");
}
