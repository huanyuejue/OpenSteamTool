#ifndef DLLMAIN_H
#define DLLMAIN_H

#include "OSTPlatform/include/DynamicLibrary.h"

#include <string>
#include <fstream>
#include <filesystem>
#include <array>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <regex>
#include <memory>
#include <atomic>
#include <format>

#include "Steam/Types.h"
#include "Steam/Enums.h"
#include "Steam/Structs.h"
#include "Steam/Callback.h"
#include "Utils/Config/LuaConfig.h"
#include "Utils/Logging/Log.h"
#include "Utils/Config/Config.h"


inline OSTPlatform::DynamicLibrary::ModuleHandle client_hModule = nullptr;
inline OSTPlatform::DynamicLibrary::ModuleHandle ui_hModule = nullptr;

// SteamUI 的 LoadModuleWithPath 劫持会等这个屏障，确保 client 侧 hook
// 装完才放 Steam 自己去拿 steamclient 句柄，否则会拿到半初始化状态。
inline std::atomic<bool> g_HooksInstalled{false};
// Diversion 影子模块是否生效。false = 回退到原版 steamclient（hook 就地打在
// 原模块上，Denuvo/RE 完整性检查会看到，敏感游戏可能闪退），必须大声告警。
inline std::atomic<bool> g_IsDiversionActive{false};
// Steam 是否真的换到了 diversion 副本上（LoadModuleWithPath 劫持分发成功）。
// 15s 超时还没换 = Steam 在用启动时加载的原版，副本上的 hook 全打空，
// 此时必须回退原版保解锁（1.5.2.1 行为），否则静默全灭。
inline std::atomic<bool> g_DiversionAdopted{false};

inline constexpr size_t kRuntimePathCapacity = 260;

inline char SteamInstallPath[kRuntimePathCapacity] = {};
inline char SteamclientPath[kRuntimePathCapacity]  = {};
inline char SteamUIPath[kRuntimePathCapacity]      = {};
inline char DiversionPath[kRuntimePathCapacity]    = {};
inline char LuaDir[kRuntimePathCapacity]           = {};
inline char ConfigPath[kRuntimePathCapacity]       = {};

// The fake AppId used by -onlinefix (SpaceWar).
constexpr AppId_t kOnlineFixAppId = 480;

#endif // DLLMAIN_H
