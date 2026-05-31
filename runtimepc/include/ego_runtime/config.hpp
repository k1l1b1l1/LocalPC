#pragma once

#include <cstdint>
#include <string>

namespace ego_runtime {

enum class TransportMode {
    kEgoContractTcp,
};

inline TransportMode ParseTransportMode(const std::string& value) {
    (void)value;
    return TransportMode::kEgoContractTcp;
}

struct RuntimeConfig {
    TransportMode transport = TransportMode::kEgoContractTcp;

    std::string ego_host = "192.168.10.50";
    std::uint16_t board_control_port = 5000U;
    std::uint16_t data_port = 5001U;

    std::uint16_t control_port = 19002U;
    std::string control_pipe_name = "ego-runtime-ctl";

    std::string data_root = "/data/ego-sessions";
    std::uint64_t chunk_max_bytes = 4294967296ULL;
    std::uint32_t chunk_max_sec = 3600U;
    std::uint32_t flush_packets = 1000U;
    std::uint32_t flush_interval_sec = 1U;
    double warn_free_gb = 50.0;
    double critical_free_gb = 10.0;
    double warn_free_percent = 15.0;
    double critical_free_percent = 5.0;

    bool auto_start_on_session_started = false;
    std::string vehicle_id = "CAR-001";
    std::string software_version = "1.0.0";

    std::uint32_t report_interval_sec = 10U;
    std::uint32_t time_gap_threshold_ms = 50U;
    std::size_t packet_buffer_capacity = 4096U;
    std::uint64_t packet_buffer_max_bytes = 512ULL * 1024ULL * 1024ULL;
    std::uint32_t max_payload_bytes = 65536U;

    std::string scenario_id;
    std::string scenario_name;
    std::string operator_name;
    std::string notes;
    std::string test_stand_config;

    std::string config_path;
    bool config_file_loaded = false;
    std::string input_file;
    bool daemon_mode = false;

    struct OfflineHook {
        bool enabled = true;
        std::string binary = "/usr/local/bin/ego-offline";
        std::string config_path = "/etc/ego-offline/config.yaml";
        std::string s3_config_path = "/etc/ego-offline/s3.local.yaml";
        bool skip_s3 = false;
    } offline;
};

RuntimeConfig LoadConfig(int argc, char** argv);
void ApplyEnvOverrides(RuntimeConfig& config);

}  // namespace ego_runtime
