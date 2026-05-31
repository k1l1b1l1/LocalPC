#pragma once
// LD-03: metadata_loader — загружает JSON-метаданные сессии и источника

#include <filesystem>
#include <string>
#include <optional>

namespace ego_offline::load {

struct SessionMetadata {
    std::string session_id;
    std::string vehicle_id;
    std::string started_at_utc;
    std::string stopped_at_utc;
    std::string runtime_version;
    std::string protocol_version;
    double      duration_s = 0.0;

    static SessionMetadata from_json(const std::filesystem::path& path);
};

struct ScenarioMetadata {
    std::string scenario_id;
    std::string operator_id;
    std::string notes;

    static ScenarioMetadata from_json(const std::filesystem::path& path);
};

struct SourceMetadata {
    std::string source_id;
    std::string config_version;
    std::string software_version;

    static SourceMetadata from_json(const std::filesystem::path& path);
};

// Loads all metadata from a session directory.
// Missing optional files → empty/default structs.
struct SessionBundle {
    SessionMetadata                session;
    std::optional<ScenarioMetadata> scenario;
    std::optional<SourceMetadata>   source;

    static SessionBundle load(const std::filesystem::path& session_dir);
};

} // namespace ego_offline::load
