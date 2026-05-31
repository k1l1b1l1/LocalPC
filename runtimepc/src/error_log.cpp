#include "ego_runtime/error_log.hpp"

#include <filesystem>
#include <sstream>

#include "ego_runtime/util.hpp"

namespace ego_runtime {
namespace {

const char* LevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::kInfo:
            return "INFO";
        case LogLevel::kWarning:
            return "WARN";
        default:
            return "ERROR";
    }
}

}  // namespace

ErrorLog::ErrorLog(std::string path) : path_(std::move(path)) {
    std::filesystem::create_directories(std::filesystem::path(path_).parent_path());
    if (std::filesystem::exists(path_)) {
        current_size_ = static_cast<std::uint64_t>(std::filesystem::file_size(path_));
    }
}

void ErrorLog::RotateIfNeeded() {
    if (current_size_ < kMaxFileBytes) {
        return;
    }
    for (int i = kMaxRotations - 1; i >= 1; --i) {
        const std::string src = path_ + "." + std::to_string(i);
        const std::string dst = path_ + "." + std::to_string(i + 1);
        if (std::filesystem::exists(src)) {
            std::filesystem::rename(src, dst);
        }
    }
    if (std::filesystem::exists(path_)) {
        std::filesystem::rename(path_, path_ + ".1");
    }
    current_size_ = 0U;
}

void ErrorLog::Write(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mu_);
    RotateIfNeeded();
    std::ofstream out(path_, std::ios::app);
    if (!out.good()) {
        return;
    }
    const std::string line = UtcNowIso8601() + " [" + LevelToString(level) + "] " + message + "\n";
    out << line;
    current_size_ += line.size();
}

}  // namespace ego_runtime
