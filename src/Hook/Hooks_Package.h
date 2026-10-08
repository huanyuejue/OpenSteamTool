#pragma once

#include "dllmain.h"

namespace Hooks_Package {
    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // 当前账号是否有该 App 的家庭共享许可（CheckAppOwnership 见过 bFamilyShared/bBorrowed）。
    bool IsSharedLicense(AppId_t appId);

    // 有效许可 = 真拥有（LuaConfig::IsOwned）或家庭共享许可。
    // Denuvo 授权收尾等需要身份对齐的地方用它，不用 IsOwned，避免共享游戏被排除。
    bool HasValidLicense(AppId_t appId);

}
