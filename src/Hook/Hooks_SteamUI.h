#pragma once

#include "dllmain.h"

// Hooks targeting steamui.dll:

namespace Hooks_SteamUI {
    void Install();
    void Uninstall();

    // Diversion 劫持是否就绪（LoadModuleWithPath pattern 是否命中）。
    // 没命中 = 无法把 Steam 重定向到影子模块，此时必须回退原版，
    // 否则 client hook 全打在没人用的副本上，解锁会静默全灭。
    bool IsDiversionRedirectArmed();

    // Queues an appId for removal from the library UI
    void QueueRemoval(AppId_t appId);
    // Cancels a queued removal when the app is added again before the UI drains it.
    void CancelRemoval(AppId_t appId);
}
