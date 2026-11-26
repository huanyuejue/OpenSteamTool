#include "HookManager.h"
#include "Hooks_Beta.h"
#include "Hooks_CallBack.h"
#include "Hooks_Decryption.h"
#include "Hooks_IPC.h"
#include "Hooks_KeyValues.h"
#include "Hooks_Manifest.h"
#include "Hooks_Misc.h"
#include "Hooks_NetPacket.h"
#include "Hooks_Package.h"
#include "Hooks_SteamUI.h"
#include "Utils/HookSupport/VehCommon.h"


namespace SteamUI {

    void CoreHook()   { 
        Hooks_SteamUI::Install(); 
    }
    void CoreUnhook() { 
        Hooks_SteamUI::Uninstall(); 
    }
}

namespace SteamClient {

    void CoreHook() {
        Hooks_CallBack::Install();
        Hooks_Decryption::Install();
        Hooks_IPC::Install();
        // Hooks_KeyValues::Install();
        Hooks_Manifest::Install();
        Hooks_Misc::Install();
        Hooks_NetPacket::Install();
        Hooks_Package::Install();
        // 只解析地址，不装 detour；必须在 client 模块加载后、UI 线程开始
        // 调用 OnUIThread 之前完成。
        Hooks_Beta::Install();
    }

    void CoreUnhook() {
        Hooks_CallBack::Uninstall();
        Hooks_Decryption::Uninstall();
        Hooks_IPC::Uninstall();
        // Hooks_KeyValues::Uninstall();
        Hooks_Manifest::Uninstall();
        Hooks_Misc::Uninstall();
        Hooks_NetPacket::Uninstall();
        Hooks_Package::Uninstall();
        Hooks_Beta::Uninstall();
        VehCommon::DisarmAll();
        VehCommon::RemoveHandler();
    }
}
