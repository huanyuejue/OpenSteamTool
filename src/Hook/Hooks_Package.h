#pragma once

#include "dllmain.h"

namespace Hooks_Package {
    // LoadPackage + CheckAppOwnership — patches the package store so that
    // user-supplied depots appear owned and accessible.
    void Install();
    void Uninstall();

    // Mark package 0 as changed and trigger CClientAppManager_ProcessPendingLicenseUpdates.
    void NotifyLicenseChanged();

    // 回退场景自举：Steam 在 hook 生效前已查完 ownership，之后不再主动重查。
    // 下一次 CheckAppOwnership 调用时用已就位的 CUser 强制刷一次 license，
    // 只生效一次，避免循环。正常路径（自然查询不断）不受影响。
    void RequestRequeryOnce();

    // 当前账号是否有该 App 的家庭共享许可（CheckAppOwnership 见过 bFamilyShared/bBorrowed）。
    bool IsSharedLicense(AppId_t appId);

    // 有效许可 = 真拥有（LuaConfig::IsOwned）或家庭共享许可。
    // Denuvo 授权收尾等需要身份对齐的地方用它，不用 IsOwned，避免共享游戏被排除。
    bool HasValidLicense(AppId_t appId);

}
