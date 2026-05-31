#include "ego_runtime/storage_monitor.hpp"

#include <chrono>
#include <filesystem>

#if !defined(_WIN32)
#include <sys/statvfs.h>
#endif

namespace ego_runtime {

StorageMonitor::StorageMonitor(RuntimeConfig config, AlertHandler handler)
    : config_(std::move(config)), handler_(std::move(handler)) {}

StorageMonitor::~StorageMonitor() { Stop(); }

StorageStatus StorageMonitor::Query() const {
    StorageStatus status{};
#if defined(_WIN32)
    std::filesystem::space_info space = std::filesystem::space(config_.data_root);
    status.free_gb = static_cast<double>(space.available) / (1024.0 * 1024.0 * 1024.0);
    if (space.capacity > 0U) {
        status.free_percent = 100.0 * static_cast<double>(space.available) / static_cast<double>(space.capacity);
    }
#else
    struct statvfs vfs {};
    if (statvfs(config_.data_root.c_str(), &vfs) == 0) {
        const auto free_bytes = static_cast<double>(vfs.f_bavail) * static_cast<double>(vfs.f_frsize);
        const auto total_bytes = static_cast<double>(vfs.f_blocks) * static_cast<double>(vfs.f_frsize);
        status.free_gb = free_bytes / (1024.0 * 1024.0 * 1024.0);
        if (total_bytes > 0.0) {
            status.free_percent = 100.0 * free_bytes / total_bytes;
        }
    }
#endif
    if (status.free_gb < config_.critical_free_gb || status.free_percent < config_.critical_free_percent) {
        status.level = StorageLevel::kCritical;
    } else if (status.free_gb < config_.warn_free_gb || status.free_percent < config_.warn_free_percent) {
        status.level = StorageLevel::kWarning;
    }
    return status;
}

void StorageMonitor::Start() {
    if (running_.load()) {
        return;
    }
    stop_requested_ = false;
    running_ = true;
    thread_ = std::thread([this]() { MonitorLoop(); });
}

void StorageMonitor::Stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void StorageMonitor::MonitorLoop() {
    StorageLevel last_level = StorageLevel::kOk;
    while (!stop_requested_.load()) {
        const StorageStatus status = Query();
        latest_ = status;
        if (status.level != last_level) {
            handler_(status.level, status);
            last_level = status.level;
        }
        for (int i = 0; i < 50 && !stop_requested_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

StorageStatus StorageMonitor::Latest() const { return latest_; }

}  // namespace ego_runtime
