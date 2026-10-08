#include "dllmain.h"
#include "Hook/HookManager.h"
#include "Hook/Hooks_SteamUI.h"
#include "Utils/Config/ConfigFileWatcher.h"
#include "Utils/Config/LuaFileWatcher.h"
#include "Utils/CloudRedirect/CloudRedirectHost.h"
#include "Utils/SteamMetadata/IPCLoader.h"
#include "Utils/SteamMetadata/PatternLoader.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"
#include "OSTPlatform/include/DynamicLibrary.h"
#include "OSTPlatform/include/Thread.h"

#include <chrono>
#include <filesystem>
#include <thread>
#include <windows.h>

namespace {

// Diversion 影子模块的文件操作：全部跑在 InitThread worker 线程，绝不在 loader lock 下。
constexpr int kDiversionCopyRetries = 3;
constexpr std::chrono::milliseconds kDiversionCopyRetryDelay{50};

bool IsDiversionUpToDate() {
    WIN32_FILE_ATTRIBUTE_DATA origAttr{}, divAttr{};
    const bool origExists =
        GetFileAttributesExA(SteamclientPath, GetFileExInfoStandard, &origAttr) != 0;
    const bool divExists =
        GetFileAttributesExA(DiversionPath, GetFileExInfoStandard, &divAttr) != 0;
    if (!origExists || !divExists) return false;
    return origAttr.nFileSizeHigh == divAttr.nFileSizeHigh &&
           origAttr.nFileSizeLow == divAttr.nFileSizeLow &&
           origAttr.ftLastWriteTime.dwLowDateTime == divAttr.ftLastWriteTime.dwLowDateTime &&
           origAttr.ftLastWriteTime.dwHighDateTime == divAttr.ftLastWriteTime.dwHighDateTime;
}

bool CopySteamClientToDiversion() {
    std::error_code ec;
    std::filesystem::path diversionFsPath(DiversionPath);
    std::filesystem::create_directories(diversionFsPath.parent_path(), ec);
    if (ec) {
        LOG_WARN("Diversion: failed to create directory for {} ({})", DiversionPath, ec.message());
        return false;
    }
    // 先拷到临时文件再原子替换：两个 Steam 进程同时启动时，直接写同一目标
    // 会产生撕裂副本；MoveFileEx 替换是原子的，读到的要么是旧完整版要么是新完整版。
    char tmpPath[kRuntimePathCapacity] = {};
    sprintf_s(tmpPath, "%s.tmp", DiversionPath);
    // 旧 Steam 进程可能还占着句柄：只读属性先清掉，否则 CopyFile 必败。
    const DWORD attrs = GetFileAttributesA(tmpPath);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesA(tmpPath, FILE_ATTRIBUTE_NORMAL);
    }
    for (int attempt = 1; attempt <= kDiversionCopyRetries; ++attempt) {
        if (CopyFileA(SteamclientPath, tmpPath, FALSE) &&
            MoveFileExA(tmpPath, DiversionPath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            LOG_INFO("Diversion: cloned steamclient64.dll -> {}", DiversionPath);
            return true;
        }
        const DWORD gle = GetLastError();
        // 只有共享冲突/拒绝访问值得重试；路径错误等直接放弃，不烧时间。
        if (gle != ERROR_SHARING_VIOLATION && gle != ERROR_ACCESS_DENIED) {
            LOG_WARN("Diversion: CopyFile failed, not retryable (err={})", gle);
            break;
        }
        if (attempt < kDiversionCopyRetries) {
            std::this_thread::sleep_for(kDiversionCopyRetryDelay);
        } else {
            LOG_WARN("Diversion: CopyFile failed after {} attempts (err={})",
                     kDiversionCopyRetries, gle);
        }
    }
    return false;
}

} // namespace

// prepare key runtime paths.
bool InitializeSteamComponents()
{
    const std::string steamInstallPath = OSTPlatform::DynamicLibrary::GetCurrentDirectoryPath();
    if (steamInstallPath.empty()) {
        return false;
    }
    sprintf_s(SteamInstallPath, kRuntimePathCapacity, "%s", steamInstallPath.c_str());
    sprintf_s(SteamclientPath, kRuntimePathCapacity, "%s\\steamclient64.dll",  SteamInstallPath);
    sprintf_s(SteamUIPath,     kRuntimePathCapacity, "%s\\steamui.dll",        SteamInstallPath);
    sprintf_s(DiversionPath,   kRuntimePathCapacity, "%s\\bin\\diversion64.dll", SteamInstallPath);
    sprintf_s(LuaDir,          kRuntimePathCapacity, "%s\\config\\lua",        SteamInstallPath);
    sprintf_s(ConfigPath,      kRuntimePathCapacity, "%s\\opensteamtool.toml", SteamInstallPath);
    
    // Diversion 影子模块：所有 hook 只打在 steamclient64.dll 的完整副本上，
    // 原版模块保持 100% 干净，Denuvo/RE 引擎的完整性检查才能通过。
    // 副本加载失败很可能是 Steam 自更新中途拷出的撕裂文件，删掉重拷一次再试；
    // 还不行才回退原版并大声告警——静默回退等于安静裸奔。
    client_hModule = nullptr;
    // 只有"确认最新"或"刚拷成功"的副本才值得信任；拷都拷不出来就别碰旧文件，
    // 直接回退原版（陈旧副本不可信）。
    bool diversionFileUsable = IsDiversionUpToDate();
    if (diversionFileUsable) {
        LOG_DEBUG("Diversion module is already up to date ({}), skipping copy", DiversionPath);
    } else {
        diversionFileUsable = CopySteamClientToDiversion();
    }
    if (diversionFileUsable) {
        client_hModule = OSTPlatform::DynamicLibrary::Load(DiversionPath);
    }
    if (!client_hModule && diversionFileUsable) {
        LOG_WARN("Load diversion module failed (path={}, err={}), deleting possibly torn copy and cloning once more",
                 DiversionPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
        DeleteFileA(DiversionPath);
        if (CopySteamClientToDiversion()) {
            client_hModule = OSTPlatform::DynamicLibrary::Load(DiversionPath);
        }
    }
    if (client_hModule) {
        g_IsDiversionActive.store(true);
        LOG_INFO("Loaded diversion module from {}", DiversionPath);
    } else {
        g_IsDiversionActive.store(false);
        client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
        if (!client_hModule) {
            LOG_ERROR("Load steamclient64.dll failed: {} (err={})",
                      SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
            return false;
        }
        LOG_WARN("Diversion inactive, loaded fallback steamclient64.dll from {} — hooks run on the original module, integrity-checked games may crash",
                 SteamclientPath);
    }
    
    ui_hModule = OSTPlatform::DynamicLibrary::Load(SteamUIPath);
    if(!ui_hModule) {
        LOG_ERROR("Load failed for steamui.dll: err={}", OSTPlatform::DynamicLibrary::GetLastErrorCode());
        return false;
    }
    return true;
}

// All initialisation that touches the filesystem, loads modules, scans
// memory, or installs detours runs here on a worker thread — we MUST NOT do
// any of that from inside DllMain (loader lock).
static uint32_t InitThread(OSTPlatform::DynamicLibrary::ModuleHandle selfModule) {
    Log::Init(selfModule);
    LOG_INFO("OpenSteamTool init thread started");

    if (!InitializeSteamComponents()) {
        LOG_ERROR("InitializeSteamComponents failed");
        return 1;
    }

    Config::Load(ConfigPath);
    Log::InitModules();
    Log::InstallPlatformLogSink();
    SteamDiagnostics::Initialize(SteamclientPath, SteamUIPath);

    // Load pattern files for steamclient64.dll and steamui.dll.
    // Each call computes the SHA-256 of the DLL on disk, checks the local
    // cache, and downloads from GitHub if needed.  Both calls are synchronous
    // but run on this worker thread, never under the loader lock.
    PatternLoader::Load(ui_hModule, SteamUIPath, "steamui");
    PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");

    // SteamUI hook 先装：LoadModuleWithPath 劫持必须在 Steam 自己加载
    // steamclient 之前就位，它内部会等 g_HooksInstalled 屏障再放行。
    SteamUI::CoreHook();

    // 如果劫持没装上（pattern 缺失），影子模块就是没人用的摆设——
    // 此时必须换回原版再装 client hook，否则解锁静默全灭。
    // 注意 PatternLoader 的 map 按模块句柄缓存，换模块后必须重 Load 一次。
    if (g_IsDiversionActive.load() && !Hooks_SteamUI::IsDiversionRedirectArmed()) {
        LOG_WARN("Diversion redirect unavailable (LoadModuleWithPath pattern missing), "
                 "falling back to original steamclient64 — unlock keeps working, isolation disabled");
        ::FreeLibrary(static_cast<HMODULE>(client_hModule));
        client_hModule = OSTPlatform::DynamicLibrary::Load(SteamclientPath);
        g_IsDiversionActive.store(false);
        if (!client_hModule) {
            LOG_ERROR("Fallback Load steamclient64.dll failed: {} (err={})",
                      SteamclientPath, OSTPlatform::DynamicLibrary::GetLastErrorCode());
            return 1;
        }
        PatternLoader::Load(client_hModule, SteamclientPath, "steamclient");
    }

    // IPC method metadata (funcHash, fencepost, argc, ...)
    IPCLoader::Load(SteamclientPath);

    std::vector<std::string> watchDirs = Config::GetLuaPaths();
    watchDirs.push_back(std::string(LuaDir));
    for (const auto& dir : watchDirs)
        LuaConfig::ParseDirectory(dir);

    LuaFileWatcher::Start(watchDirs);
    ConfigFileWatcher::Start(ConfigPath, LuaDir);

    SteamClient::CoreHook();

    // Surface any functions that FindPattern() could not locate.
    PatternLoader::ReportMissingFunctions();

    // Optional Steam Cloud save redirection (CloudRedirect). No-op unless
    // [cloud].enabled is set and cloud_redirect.dll is present.
    CloudRedirectHost::Initialize(SteamInstallPath);

    g_HooksInstalled.store(true);
    LOG_INFO("OpenSteamTool init complete ({})",
             g_IsDiversionActive.load() ? "Diversion active" : "Diversion bypassed, using original steamclient64");
    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, PVOID pvReserved)
{
    if (dwReason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        // Hand off all real work to a worker thread to avoid running file I/O,
        // module loading and detour transactions under the loader lock.
        OSTPlatform::Thread::StartDetached([module = reinterpret_cast<OSTPlatform::DynamicLibrary::ModuleHandle>(hModule)] {
            return InitThread(module);
        });
    }
    else if (dwReason == DLL_PROCESS_DETACH)
    {
        g_HooksInstalled.store(false);
        g_IsDiversionActive.store(false);
        ConfigFileWatcher::Stop();
        LuaFileWatcher::Stop();
        SteamUI::CoreUnhook();
        SteamClient::CoreUnhook();
        CloudRedirectHost::Shutdown();
    }

    return TRUE;
}
