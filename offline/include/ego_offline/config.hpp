#pragma once
// Pipeline configuration -- parsed from offline.yaml (or defaults)

#include <string>
#include <vector>
#include <filesystem>

namespace ego_offline {

struct Config {
    std::string offline_pipeline_version = "1.0";

    int    grid_dt_ms        = 10;
    int    join_tolerance_ms = 5;
    int    telemetry_gap_ms       = 50;
    double audio_gap_factor       = 1.5;
    int    max_timestamp_jump_ms  = 1000;

    struct Validation {
        double min_valid_duration_s    = 10.0;
        bool   fail_on_no_valid_ranges = true;
    } validation;

    struct Sync {
        double      min_confidence      = 0.7;
        double      max_offset_search_s = 30.0;
        std::string master_clock        = "ego";
        bool        fail_on_sync_error  = false; // P1-3: exit code 2 on sync_failed
    } sync;

    std::string source_contract = "v1";

    struct Mdf4 {
        bool        include_audio     = true;
        bool        audio_preview_rms = false;
        std::string output_filename   = "session.mf4";
    } mdf4;

    struct Scene {
        int  segment_min_duration_ms         = 500;
        bool classify_unknown_when_degraded  = true;
    } scene;

    struct Debug {
        bool        keep_intermediate = false;
        std::string intermediate_dir  = "stages";
    } debug;

    struct S3Upload {
        bool        enabled                 = true;
        std::string endpoint                = "https://s3.twcstorage.ru";
        std::string region                  = "ru-1";
        std::string bucket;
        std::string prefix                  = "ego-sessions";
        bool        path_style              = true;
        std::string access_key_env          = "EGO_S3_ACCESS_KEY";
        std::string secret_key_env          = "EGO_S3_SECRET_KEY";
        std::string access_key;             // optional inline (local yaml only)
        std::string secret_key;
        bool        require_pipeline_success = false;
        bool        require_real_mdf4       = true;
        int         max_retries             = 5;
        int         retry_backoff_ms        = 1000;
        bool        include_session_files   = false;  // TZ MVP: only offline/
        std::vector<std::string> include_files;
        std::vector<std::string> session_include_files;
    } s3;

    static Config from_yaml(const std::filesystem::path& path);
    static Config merge_yaml(Config base, const std::filesystem::path& path);
    static Config defaults() { return {}; }

    void resolve_s3_credentials();
};

} // namespace ego_offline
