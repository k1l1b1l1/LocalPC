#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "ego_runtime/config.hpp"

namespace ego_runtime {

struct NavSnapshot {
    std::uint64_t sample_id = 0U;
    std::uint64_t received_ms = 0U;
    double latitude_deg = 0.0;
    double longitude_deg = 0.0;
    double altitude_m = 0.0;
    float speed_mps = 0.0f;
    float heading_deg = 0.0f;
    std::uint8_t fix_quality = 0U;
    std::uint8_t satellites = 0U;
    float hdop = 99.9f;
    std::string source = "local_m2_tcp";
};

class NavProvider {
public:
    explicit NavProvider(RuntimeConfig config);
    ~NavProvider();

    bool Start();
    void Stop();

    bool IsEnabled() const;
    bool GetSnapshot(NavSnapshot* snapshot) const;
    std::string Status() const;

private:
    void ReaderLoop();
    void UpdateStatus(const std::string& status);
    bool ShouldStop() const;

    RuntimeConfig config_;
    mutable std::mutex mu_{};
    std::thread reader_thread_{};
    bool stop_requested_ = false;
    bool running_ = false;
    bool has_snapshot_ = false;
    std::uint64_t last_update_ms_ = 0U;
    std::uint64_t sample_counter_ = 0U;
    NavSnapshot latest_{};
    std::string status_ = "disabled";
    std::string input_label_;
};

}  // namespace ego_runtime
