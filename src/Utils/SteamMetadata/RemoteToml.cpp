#include "RemoteToml.h"
#include "OSTPlatform/include/Http.h"
#include "Utils/Config/Config.h"
#include "Utils/Logging/Log.h"
#include "Utils/SteamMetadata/SteamDiagnostics.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace RemoteToml {

namespace {
    // 签名源：huanyuejue/steam-monitor（定时采集 stable+beta，文件更全）。
    // 与 OpenSteam001 版布局差异：channel 不是分支而是 main 下的目录，
    // 因此分支固定 main，{channel} 走路径段——占位符集合不变，模板机制兼容。
    constexpr const char* kGithubTemplate =
        "https://raw.githubusercontent.com/huanyuejue/steam-monitor/"
        "main/{channel}/{component}/{sha256}.toml";
    // gh-proxy 套在 raw 直连外面：对大仓库的 raw 文件比 jsDelivr 稳定，
    // 失败才回退 GitHub 直连保底。
    constexpr const char* kGhProxyTemplate =
        "https://gh-proxy.com/https://raw.githubusercontent.com/huanyuejue/steam-monitor/"
        "main/{channel}/{component}/{sha256}.toml";

    static bool HasPlaceholder(std::string_view text, std::string_view placeholder)
    {
        return text.find(placeholder) != std::string_view::npos;
    }

    static bool IsValidTemplate(std::string_view urlTemplate)
    {
        return HasPlaceholder(urlTemplate, "{channel}") &&
               HasPlaceholder(urlTemplate, "{component}") &&
               HasPlaceholder(urlTemplate, "{sha256}");
    }

    static void ReplaceAll(std::string& text,
                           std::string_view from,
                           std::string_view to)
    {
        size_t pos = 0;
        while ((pos = text.find(from, pos)) != std::string::npos) {
            text.replace(pos, from.size(), to);
            pos += to.size();
        }
    }

    static std::string ExpandTemplate(std::string urlTemplate,
                                      const Request& request,
                                      std::string_view sha256)
    {
        ReplaceAll(urlTemplate, "{channel}", request.channel);
        ReplaceAll(urlTemplate, "{component}", request.component);
        ReplaceAll(urlTemplate, "{sha256}", sha256);
        return urlTemplate;
    }

    // 带硬性上限的请求。
    // WinHTTP 自带的超时只管到"等待数据"那一步：服务器接了连接却迟迟不开始
    // 应答时，调用会一直挂在那里。平时无害，但这段代码跑在装 hook 的过程里，
    // 一挂就把整个启动流程钉死，用户只能去任务管理器结束 Steam。
    // 所以这里把请求放独立线程跑，主线程到点就放弃，让调用方走失败分支。
    static OSTPlatform::Http::Result ExecuteWithDeadline(const char* url,
                                                          uint32_t budgetMs) {
        struct State {
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
            OSTPlatform::Http::Result result;
        };

        // 用共享状态而不是栈对象：超时后工作线程可能还在跑。
        auto state = std::make_shared<State>();
        const std::string urlCopy = url ? url : "";

        std::thread worker([state, urlCopy] {
            auto r = OSTPlatform::Http::Execute(L"GET", urlCopy.c_str(),
                                                nullptr, 0, nullptr);
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->result = std::move(r);
                state->done = true;
            }
            state->cv.notify_all();
        });

        {
            std::unique_lock<std::mutex> lock(state->mutex);
            if (state->cv.wait_for(lock, std::chrono::milliseconds(budgetMs),
                                   [&] { return state->done; })) {
                lock.unlock();
                worker.join();
                return state->result;
            }
        }

        LOG_WARN("RemoteToml: request exceeded {}ms deadline, abandoning URL {}",
                 budgetMs, urlCopy);
        // 底层调用还卡在里面，没法安全终止，只能放手让它自己退出。
        worker.detach();
        return OSTPlatform::Http::Result{};
    }

    // WinHTTP 在部分机器上光是建立连接就要好几秒，同一条链接换系统自带的 curl
    // 只要一两百毫秒——差别在连接过程，不在网络本身。
    // curl 跑在独立进程里，不跟 Steam 自己的网络初始化抢资源，所以拿它兜底。
    static bool FetchWithCurl(const std::string& url,
                              const std::filesystem::path& outPath,
                              std::string& body) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::remove(outPath, ec);

        auto toWide = [](const std::string& s) {
            if (s.empty()) return std::wstring();
            const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                              static_cast<int>(s.size()), nullptr, 0);
            if (n <= 0) return std::wstring();
            std::wstring w(static_cast<size_t>(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                w.data(), n);
            return w;
        };

        // 干净的收尾：不管成功失败都把这个临时文件清掉，
        // 免得缓存目录里堆一圈没用的 .curl 副本。
        // 存值不存引用：守卫活到函数结束，拿引用等于把自己和参数的寿命绑死。
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() {
                std::error_code cleanupEc;
                std::filesystem::remove(path, cleanupEc);
            }
        } cleanup{outPath};

        // 走绝对路径：不依赖 PATH 里有没有 System32，也不怕被同名的 curl
        // 抢先解析到。Win10 1803 起系统自带这个程序，找不到就当兜底不可用。
        wchar_t sysDir[MAX_PATH] = {};
        const UINT sysLen = GetSystemDirectoryW(sysDir, MAX_PATH);
        if (sysLen == 0 || sysLen >= MAX_PATH) {
            LOG_WARN("RemoteToml: GetSystemDirectoryW failed (error={})", GetLastError());
            return false;
        }
        std::wstring curlExe(sysDir, sysLen);
        curlExe += L"\\curl.exe";
        if (GetFileAttributesW(curlExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LOG_WARN("RemoteToml: curl.exe not present in system directory, fallback unavailable");
            return false;
        }

        // --max-time 压在外部等待时限之下，让 curl 自己先超时退出，
        // 走到 12 秒那一步说明进程真的卡住了，才需要强制结束。
        std::wstring cmd = L"\"" + curlExe + L"\" -sS -L --max-time 10 -o \"";
        cmd += toWide(outPath.string());
        cmd += L"\" \"";
        cmd += toWide(url);
        cmd += L"\"";

        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            LOG_WARN("RemoteToml: curl fallback failed to start (error={})", GetLastError());
            return false;
        }

        // 比 curl 自己的 --max-time 多一点余量就够，超出说明进程卡住了。
        if (WaitForSingleObject(pi.hProcess, 12000) != WAIT_OBJECT_0) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            LOG_WARN("RemoteToml: curl fallback timed out {}", url);
            return false;
        }

        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);

        if (exitCode != 0) {
            LOG_WARN("RemoteToml: curl fallback exited {} ({})", exitCode, url);
            return false;
        }

        {
            std::ifstream ifs(outPath, std::ios::binary);
            if (ifs) {
                body.assign((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());
            }
        }   // 先让流析构：Windows 上文件被占用时删不掉
        fs::remove(outPath, ec);
        if (body.empty()) {
            LOG_WARN("RemoteToml: curl fallback produced empty body ({})", url);
            return false;
        }
        return true;
    }

    static std::vector<std::string> BuildUrlTemplates()
    {
        const std::string remoteUrlTemplate = Config::GetRemoteUrlTemplate();
        if (!remoteUrlTemplate.empty()) {
            if (!IsValidTemplate(remoteUrlTemplate)) {
                LOG_WARN("RemoteToml: remote.url_template must contain "
                         "{channel}, {component}, and {sha256}; remote fetch disabled");
                return {};
            }

            return { remoteUrlTemplate };
        }

        // 默认优先请求 gh-proxy 镜像，失败再回退 GitHub 直连
        if (Config::GetRemoteOrder() == "github-first")
            return { kGithubTemplate, kGhProxyTemplate };
        return { kGhProxyTemplate, kGithubTemplate };
    }
} // namespace

Result Fetch(const Request& request)
{
    namespace fs = std::filesystem;
    Result out;

    // 1. SHA-256 of the DLL.
    const auto hashStart = std::chrono::steady_clock::now();
    out.sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    const auto hashMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - hashStart).count();

    if (out.sha256.empty()) {
        LOG_WARN("RemoteToml({}/{}): Sha256OfFile failed for {} ({} ms)",
                 request.channel, request.component, request.dllPath, hashMs);
        return out;
    }
    LOG_INFO("RemoteToml({}/{}): sha256 = {} ({} ms)",
             request.channel, request.component, out.sha256, hashMs);

    // 2. Cache path & dir.
    fs::path steamRoot = fs::path(request.dllPath).parent_path();
    fs::path cacheDir  = steamRoot / "opensteamtool" / request.channel / request.component;
    fs::path cachePath = cacheDir / (out.sha256 + ".toml");
    const std::string cachePathText = cachePath.string();

    std::error_code mkdirEc;
    fs::create_directories(cacheDir, mkdirEc);
    if (mkdirEc) {
        LOG_WARN("RemoteToml({}/{}): could not create cache dir {} ({})",
                 request.channel, request.component, cacheDir.string(), mkdirEc.message());
    }

    // 缓存优先：pattern/IPC 文件以 DLL SHA-256 命名，内容不可变，命中直接返回，避免远端超时阻塞 Hook 安装
    {
        std::error_code ec;
        if (fs::exists(cachePath, ec) && !ec) {
            const auto sz = fs::file_size(cachePath, ec);
            if (!ec && sz > 0) {
                std::ifstream ifs(cachePath, std::ios::binary);
                if (ifs) {
                    std::string buf((std::istreambuf_iterator<char>(ifs)),
                                     std::istreambuf_iterator<char>());
                    if (!buf.empty()) {
                        LOG_INFO("RemoteToml({}/{}): loaded from cache {}",
                                 request.channel, request.component, cachePathText);
                        out.body = std::move(buf);
                        out.ok = true;
                        out.fromCache = true;
                        return out;
                    }
                }
                LOG_WARN("RemoteToml({}/{}): cache file exists but failed to read or empty: {}",
                         request.channel, request.component, cachePathText);
            }
        }
    }

    // 3. Cache miss -> try remote (poll all mirrors in order).
    // 逐个尝试全部镜像才判定失败：CDN 同步存在延迟，首个镜像 404 时后一个镜像可能已有文件。
    // transport 失败（非 404）每个镜像多打一次：跨国链路抖一下很常见，一次重试专治抖动；
    // 404 不重试（重下也是没有）。单次重试封顶，不在启动路径上烧时间。
    const std::vector<std::string> urlTemplates = BuildUrlTemplates();
    OSTPlatform::Http::Result http;
    std::string lastUrl;
    size_t notFoundCount = 0;

    for (size_t i = 0; i < urlTemplates.size(); ++i) {
        lastUrl = ExpandTemplate(urlTemplates[i], request, out.sha256);
        LOG_INFO("RemoteToml({}/{}): downloading {}",
                 request.channel, request.component, lastUrl);

        // 单次请求封顶 5 秒：启动路径上不能久等，卡住就换下一个镜像。
        constexpr uint32_t kRequestBudgetMs = 5000;
        http = ExecuteWithDeadline(lastUrl.c_str(), kRequestBudgetMs);

        if (http.ok && http.status == 200 && !http.body.empty()) break;

        // WinHTTP 在这类机器上光是建连接就要好几秒，同一条链接交给系统自带的
        // curl 只要一两百毫秒。所以每个镜像内部先走 WinHTTP，没拿到东西就立刻
        // 用 curl 补一次，还不行再换下一个镜像。404 说明上游确实没有这个文件，
        // 换方式重试也没用，直接跳到下一个镜像。
        if (!(http.ok && http.status == 404)) {
            const fs::path tmpOut = cachePath.string() + ".curl";
            std::string body;
            LOG_INFO("RemoteToml({}/{}): trying curl fallback {}",
                     request.channel, request.component, lastUrl);
            if (FetchWithCurl(lastUrl, tmpOut, body)) {
                std::error_code writeEc;
                {
                    std::ofstream ofs(cachePath, std::ios::binary | std::ios::trunc);
                    if (ofs) {
                        ofs.write(body.data(), static_cast<std::streamsize>(body.size()));
                        ofs.close();
                    }
                    if (!ofs) writeEc = std::make_error_code(std::errc::io_error);
                }
                if (writeEc) {
                    LOG_WARN("RemoteToml({}/{}): curl fallback could not write cache {}",
                             request.channel, request.component, cachePathText);
                } else {
                    LOG_INFO("RemoteToml({}/{}): cached to {} (curl)",
                             request.channel, request.component, cachePathText);
                }
                out.body = std::move(body);
                out.ok = true;
                return out;
            }
        }

        const bool hasNext = (i + 1 < urlTemplates.size());
        if (http.ok && http.status == 200) {
            LOG_WARN("RemoteToml({}/{}): mirror returned empty body: {}{}",
                     request.channel, request.component, lastUrl,
                     hasNext ? ", trying next mirror" : "");
        } else if (http.ok && http.status == 404) {
            ++notFoundCount;
            LOG_WARN("RemoteToml({}/{}): mirror has no such file (HTTP 404): {}{}",
                     request.channel, request.component, lastUrl,
                     hasNext ? ", trying next mirror" : "");
        } else if (hasNext) {
            LOG_WARN("RemoteToml({}/{}): mirror failed ({} ok={} HTTP={}), falling back",
                     request.channel, request.component, lastUrl, http.ok, http.status);
        }
    }

    // 4. Remote OK → write cache, return body.
    if (http.ok && http.status == 200 && !http.body.empty()) {
        // 先落临时文件再替换，保证缓存文件不会因崩溃停留在截断状态
        fs::path tmpPath = cachePath;
        tmpPath += ".tmp";
        std::error_code writeEc;
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (ofs) {
                ofs.write(http.body.data(),
                          static_cast<std::streamsize>(http.body.size()));
                ofs.close();
            }
            if (!ofs)
                writeEc = std::make_error_code(std::errc::io_error);
        }
        if (!writeEc) {
            fs::remove(cachePath, writeEc);
            if (!writeEc)
                fs::rename(tmpPath, cachePath, writeEc);
        }
        if (writeEc) {
            LOG_WARN("RemoteToml({}/{}): could not write cache {} ({})",
                     request.channel, request.component, cachePathText, writeEc.message());
            std::error_code cleanupEc;
            fs::remove(tmpPath, cleanupEc);
        } else {
            LOG_INFO("RemoteToml({}/{}): cached to {}",
                     request.channel, request.component, cachePathText);
        }
        out.body = std::move(http.body);
        out.ok = true;
        return out;
    }

    // 5. Total failure — caller handles popup / degraded mode.
    // 区分全部 404 与网络故障：前者表示上游未发布，后者表示本机网络问题。
    // allNotFound 语义收紧：必须至少尝试过一个镜像且全 404 才算（模板配错导致
    // 零尝试不算"未发布"，算配置错，同样不弹窗）。
    out.allNotFound = !urlTemplates.empty() && notFoundCount == urlTemplates.size();
    if (out.allNotFound) {
        LOG_WARN("RemoteToml({}/{}): upstream has not published (all {} mirrors 404, last URL: {})",
                 request.channel, request.component, urlTemplates.size(),
                 lastUrl.empty() ? "<none>" : lastUrl);
    } else {
        LOG_WARN("RemoteToml({}/{}): no source available (last URL: {} HTTP {})",
                 request.channel, request.component,
                 lastUrl.empty() ? "<none>" : lastUrl, http.status);
    }
    return out;
}

bool Invalidate(const Request& request)
{
    namespace fs = std::filesystem;
    const std::string sha256 = SteamDiagnostics::Sha256Of(request.dllPath);
    if (sha256.empty()) return false;
    fs::path cachePath = fs::path(request.dllPath).parent_path() /
                         "opensteamtool" / request.channel / request.component /
                         (sha256 + ".toml");
    std::error_code ec;
    const bool removed = fs::remove(cachePath, ec);
    if (ec) {
        LOG_WARN("RemoteToml({}/{}): failed to remove stale cache {} ({})",
                 request.channel, request.component, cachePath.string(), ec.message());
        return false;
    }
    if (removed) {
        LOG_WARN("RemoteToml({}/{}): removed stale cache {}",
                 request.channel, request.component, cachePath.string());
    }
    return removed;
}

} // namespace RemoteToml
