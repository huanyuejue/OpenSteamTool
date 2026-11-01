#pragma once

// Denuvo-specific module scanner. ProcessInspector stays generic; this file
// owns only the current "is this process protected" policy.

#include "Steam/Types.h"

#include <cstddef>
#include <format>
#include <string>

namespace PipeManager::DenuvoAuth {

    enum class DetectionMethod {
        None,
        LegacySectionString,
        OepPattern,
        // 结构特征识别：legacy 保护壳段名与可写可执行大段同时具备。
        // 用于兜住既不携带 OEP 特征串、段内也没有 DENUVO 字样的构建。
        ProtectedBlobSection,
    };

    const char* ToString(DetectionMethod method);

    struct ProtectionScanReport {
        bool denuvoDetected = false;
        DetectionMethod method = DetectionMethod::None;
        std::string modulePath;
        std::string sectionName;
        uint32 pid = 0;
        uint32 moduleSize = 0;
        uint32 entryPointRva = 0;
        uint32 matchRva = 0;
        size_t matchRawOffset = 0;
        size_t scannedModules = 0;
        double elapsedMs = 0.0;

        std::string DebugString() const {
            return std::format(
                "[pid={}] denuvoDetected={} method={} module={} section={} moduleSize={} entryPointRva=0x{:X} matchRva=0x{:X} matchRaw=0x{:X} scannedModules={} elapsedMs={:.3f}",
                pid,
                denuvoDetected,
                ToString(method),
                modulePath.empty() ? "-" : modulePath,
                sectionName.empty() ? "-" : sectionName,
                moduleSize,
                entryPointRva,
                matchRva,
                matchRawOffset,
                scannedModules,
                elapsedMs);
        }
    };

    ProtectionScanReport ScanProtection(PID_t pid);

} // namespace PipeManager::DenuvoAuth
