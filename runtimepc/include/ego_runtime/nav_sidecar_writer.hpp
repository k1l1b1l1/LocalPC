#pragma once

#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>

#include "ego_runtime/config.hpp"
#include "ego_runtime/nav_provider.hpp"

namespace ego_runtime {

class NavSidecarWriter {
public:
    explicit NavSidecarWriter(RuntimeConfig config);
    ~NavSidecarWriter();

    bool Open(const std::string& session_dir);
    void Close();
    bool Write(const NavSnapshot& snapshot, std::uint64_t ts_ns);

    std::string Path() const;
    std::uint64_t SamplesWritten() const;

private:
    bool FlushLocked(bool force_sync);

    RuntimeConfig config_;
    mutable std::mutex mu_{};
    std::FILE* file_ = nullptr;
    std::string path_;
    std::uint64_t last_sample_id_ = 0U;
    std::uint64_t last_ts_ns_ = 0U;
    std::uint64_t samples_written_ = 0U;
    std::uint32_t dirty_samples_ = 0U;
};

}  // namespace ego_runtime
