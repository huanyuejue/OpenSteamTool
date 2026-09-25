#pragma once
#include <string>

namespace RemoteToml {

    struct Request {
        std::string channel;    // "pattern" or "ipc"
        std::string component;  // "steamclient" or "steamui"
        std::string dllPath;
    };

    struct Result {
        bool        ok        = false;
        bool        fromCache = false;
        std::string body;
        std::string sha256;
    };

    // 优先复用以 SHA 命名的本地缓存，缺失时按序轮询全部远端镜像，全部失败才返回空
    Result Fetch(const Request& request);

} // namespace RemoteToml
