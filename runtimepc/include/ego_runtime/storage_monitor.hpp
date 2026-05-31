#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "ego_runtime/config.hpp"

namespace ego_runtime {

enum class StorageLevel {
    kOk,
    kWarning,
    kCritical
};

struct StorageStatus {
    double free_gb = 0.0;
    double free_percent = 100.0;
    StorageLevel level = StorageLevel::kOk;
};

class StorageMonitor {
public:
    using AlertHandler = std::function<void(StorageLevel, const StorageStatus&)>;

    StorageMonitor(RuntimeConfig config, AlertHandler handler);
    ~StorageMonitor();

    void Start();
    void Stop();
    StorageStatus Latest() const;

private:
    void MonitorLoop();
    StorageStatus Query() const;

    RuntimeConfig config_;
    AlertHandler handler_;
    std::thread thread_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    StorageStatus latest_{};
};

}  // namespace ego_runtime
