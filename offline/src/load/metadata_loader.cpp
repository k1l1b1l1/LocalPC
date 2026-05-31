#include "ego_offline/load/metadata_loader.hpp"
#include "ego_offline/json_reader.hpp"

#include <stdexcept>

namespace ego_offline::load {

SessionMetadata SessionMetadata::from_json(const std::filesystem::path& path) {
    auto j = json_load(path.string());
    SessionMetadata m;
    m.session_id       = j.value<std::string>("session_id",       "");
    m.vehicle_id       = j.value<std::string>("vehicle_id",        "");
    m.started_at_utc   = j.value<std::string>("started_at_utc",    "");
    m.stopped_at_utc   = j.value<std::string>("stopped_at_utc",    "");
    m.runtime_version  = j.value<std::string>("runtime_version",   "");
    m.protocol_version = j.value<std::string>("protocol_version",  "");
    m.duration_s       = j.value<double>("duration_s", 0.0);
    return m;
}

ScenarioMetadata ScenarioMetadata::from_json(const std::filesystem::path& path) {
    auto j = json_load(path.string());
    ScenarioMetadata m;
    m.scenario_id = j.value<std::string>("scenario_id", "");
    {
        std::string op = j.value<std::string>("operator_id", "");
        if (op.empty()) op = j.value<std::string>("operator", "");
        m.operator_id = op;
    }
    m.notes       = j.value<std::string>("notes",       "");
    return m;
}

SourceMetadata SourceMetadata::from_json(const std::filesystem::path& path) {
    auto j = json_load(path.string());
    SourceMetadata m;
    m.source_id        = j.value<std::string>("source_id",        "");
    m.config_version   = j.value<std::string>("config_version",   "");
    m.software_version = j.value<std::string>("software_version", "");
    return m;
}

SessionBundle SessionBundle::load(const std::filesystem::path& session_dir) {
    SessionBundle b;
    b.session = SessionMetadata::from_json(session_dir / "session_metadata.json");

    auto scenario_path = session_dir / "scenario_metadata.json";
    if (std::filesystem::exists(scenario_path)) {
        try { b.scenario = ScenarioMetadata::from_json(scenario_path); } catch (...) {}
    }
    auto source_path = session_dir / "source_metadata.json";
    if (std::filesystem::exists(source_path)) {
        try { b.source = SourceMetadata::from_json(source_path); } catch (...) {}
    }
    return b;
}

} // namespace ego_offline::load
