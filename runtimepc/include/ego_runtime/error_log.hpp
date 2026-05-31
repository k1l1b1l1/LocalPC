#pragma once

#include <fstream>
#include <mutex>
#include <string>

namespace ego_runtime {

enum class LogLevel {
    kInfo,
    kWarning,
    kError
};

class ErrorLog {
public:
    explicit ErrorLog(std::string path);

    void Write(LogLevel level, const std::string& message);

private:
    void RotateIfNeeded();

    std::string path_;
    std::mutex mu_{};
    std::uint64_t current_size_ = 0U;
    static constexpr std::uint64_t kMaxFileBytes = 50ULL * 1024ULL * 1024ULL;
    static constexpr int kMaxRotations = 10;
};

}  // namespace ego_runtime
