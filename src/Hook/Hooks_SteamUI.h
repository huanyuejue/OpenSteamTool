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

    // 首载抢劫（1791）：Steam 不再走 LoadModuleWithPath 拿 steamclient，
    // 而是启动期直接 LoadLibraryExW。diversion 就绪后把 kernel32 的首载也挂上，
    // 抢到就是隔离生效；来晚了（首载已发生）它自然打空，不影响现有流程。
    // 顺带罩住 GetProcAddress：晚绑定取原版导出的一律改道副本，默认透传。
    void InstallFirstLoadRedirect();

    // 首载抢劫是否已武装（kernel32 LoadLibrary hook 是否装上）。
    // 与 IsDiversionRedirectArmed 合起来判断"隔离是否还有换机路径"：
    // 两条路都没了才该回退原版。
    bool IsFirstLoadRedirectArmed();

    // 我们自己要 Load 原版时（回退）必须短暂 bypass，不然 Load 会被劫回副本。
    void SetFirstLoadRedirectBypass(bool bypass);

    // Queues an appId for removal from the library UI
    void QueueRemoval(AppId_t appId);
    // Cancels a queued removal when the app is added again before the UI drains it.
    void CancelRemoval(AppId_t appId);

    // 新入库的 app 排进队列，等界面线程把它补进库里。
    // 光改许可列表 Steam 不会自己重新扫一遍，得在界面线程上主动通知。
    void QueueAddition(AppId_t appId);
}
