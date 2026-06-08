#include "ego_runtime/config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ego_runtime {
namespace {

std::string Trim(const std::string& s) {
    std::size_t start = 0U;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
        ++start;
    }
    std::size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1U])) != 0) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string Unquote(const std::string& s) {
    if (s.size() >= 2U && s.front() == '"' && s.back() == '"') {
        return s.substr(1U, s.size() - 2U);
    }
    return s;
}

void ParseScalar(const std::string& key, const std::string& value, RuntimeConfig& cfg) {
    if (key == "network.transport") {
        cfg.transport = ParseTransportMode(Unquote(value));
    } else if (key == "network.ego_host") {
        cfg.ego_host = Unquote(value);
    } else if (key == "network.board_control_port" || key == "network.control_port") {
        cfg.board_control_port = static_cast<std::uint16_t>(std::stoul(value));
    } else if (key == "network.data_port") {
        cfg.data_port = static_cast<std::uint16_t>(std::stoul(value));
    } else if (key == "network.max_payload_bytes") {
        cfg.max_payload_bytes = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "ipc.port") {
        cfg.control_port = static_cast<std::uint16_t>(std::stoul(value));
    } else if (key == "control.port") {
        cfg.control_port = static_cast<std::uint16_t>(std::stoul(value));
    } else if (key == "storage.data_root") {
        cfg.data_root = Unquote(value);
    } else if (key == "storage.chunk_max_bytes") {
        cfg.chunk_max_bytes = std::stoull(value);
    } else if (key == "storage.chunk_max_sec") {
        cfg.chunk_max_sec = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "storage.flush_packets") {
        cfg.flush_packets = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "storage.flush_interval_sec") {
        cfg.flush_interval_sec = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "storage.warn_free_gb") {
        cfg.warn_free_gb = std::stod(value);
    } else if (key == "storage.critical_free_gb") {
        cfg.critical_free_gb = std::stod(value);
    } else if (key == "storage.warn_free_percent") {
        cfg.warn_free_percent = std::stod(value);
    } else if (key == "storage.critical_free_percent") {
        cfg.critical_free_percent = std::stod(value);
    } else if (key == "session.auto_start_on_session_started") {
        cfg.auto_start_on_session_started = (value == "true" || value == "1");
    } else if (key == "session.vehicle_id") {
        cfg.vehicle_id = Unquote(value);
    } else if (key == "session.test_stand_config") {
        cfg.test_stand_config = Unquote(value);
    } else if (key == "diagnostics.report_interval_sec") {
        cfg.report_interval_sec = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "diagnostics.time_gap_threshold_ms") {
        cfg.time_gap_threshold_ms = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "offline.enabled") {
        cfg.offline.enabled = (value == "true" || value == "1");
    } else if (key == "offline.binary") {
        cfg.offline.binary = Unquote(value);
    } else if (key == "offline.config") {
        cfg.offline.config_path = Unquote(value);
    } else if (key == "offline.s3_config") {
        cfg.offline.s3_config_path = Unquote(value);
    } else if (key == "offline.skip_s3") {
        cfg.offline.skip_s3 = (value == "true" || value == "1");
    } else if (key == "network.reconnect_enabled") {
        cfg.reconnect_enabled = (value == "true" || value == "1");
    } else if (key == "network.reconnect_interval_ms") {
        cfg.reconnect_interval_ms = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "network.reconnect_max_attempts") {
        cfg.reconnect_max_attempts = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "network.checkpoint_packets") {
        cfg.checkpoint_packets = static_cast<std::uint32_t>(std::stoul(value));
    } else if (key == "session.auto_resume_on_run") {
        cfg.auto_resume_on_run = (value == "true" || value == "1");
    } else if (key == "session.backfill_enabled") {
        cfg.backfill_enabled = (value == "true" || value == "1");
    }
}

RuntimeConfig LoadConfigFromFile(const std::string& path, RuntimeConfig base) {
    std::ifstream in(path);
    if (!in.good()) {
        base.config_path.clear();
        base.config_file_loaded = false;
        return base;
    }
    std::string section;
    std::string line;
    while (std::getline(in, line)) {
        const auto hash = line.find('#');
        if (hash != std::string::npos) {
            line = line.substr(0U, hash);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.back() == ':') {
            section = line.substr(0U, line.size() - 1U);
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = Trim(line.substr(0U, colon));
        std::string val = Trim(line.substr(colon + 1U));
        if (!section.empty()) {
            key = section + "." + key;
        }
        ParseScalar(key, val, base);
    }
    base.config_path = path;
    base.config_file_loaded = true;
    return base;
}

}  // namespace

void ApplyEnvOverrides(RuntimeConfig& config) {
    if (const char* v = std::getenv("EGO_RUNTIME_DATA_ROOT")) {
        config.data_root = v;
    }
    if (const char* v = std::getenv("EGO_RUNTIME_EGO_HOST")) {
        config.ego_host = v;
    }
    if (const char* v = std::getenv("EGO_RUNTIME_VEHICLE_ID")) {
        config.vehicle_id = v;
    }
    if (const char* v = std::getenv("EGO_OFFLINE_ENABLED")) {
        config.offline.enabled = (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y');
    }
    if (const char* v = std::getenv("EGO_OFFLINE_BINARY")) {
        config.offline.binary = v;
    }
    if (const char* v = std::getenv("EGO_OFFLINE_CONFIG")) {
        config.offline.config_path = v;
    }
    if (const char* v = std::getenv("EGO_OFFLINE_S3_CONFIG")) {
        config.offline.s3_config_path = v;
    }
}

RuntimeConfig LoadConfig(int argc, char** argv) {
    RuntimeConfig cfg{};

    {
        const std::string etc_path = "/etc/ego-runtime/config.yaml";
        std::ifstream probe(etc_path);
        if (probe.good()) {
            cfg = LoadConfigFromFile(etc_path, cfg);
        }
    }

    std::string cli_data_root;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            cfg = LoadConfigFromFile(argv[++i], cfg);
        } else if (arg == "--data-root" && i + 1 < argc) {
            cli_data_root = argv[++i];
        } else if (arg == "--input" && i + 1 < argc) {
            cfg.input_file = argv[++i];
        } else if (arg == "--scenario-id" && i + 1 < argc) {
            cfg.scenario_id = argv[++i];
        } else if (arg == "--scenario-name" && i + 1 < argc) {
            cfg.scenario_name = argv[++i];
        } else if (arg == "--operator" && i + 1 < argc) {
            cfg.operator_name = argv[++i];
        } else if (arg == "--notes" && i + 1 < argc) {
            cfg.notes = argv[++i];
        } else if (arg == "--daemon") {
            cfg.daemon_mode = true;
        }
    }

    if (!cli_data_root.empty()) {
        cfg.data_root = cli_data_root;
    }
    ApplyEnvOverrides(cfg);
    return cfg;
}

}  // namespace ego_runtime
