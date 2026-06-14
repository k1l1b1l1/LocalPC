#include "ego_offline/config.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace ego_offline {

namespace {

std::string trim(const std::string& s) {
    size_t l = s.find_first_not_of(" \t\r\n");
    if (l == std::string::npos) return {};
    size_t r = s.find_last_not_of(" \t\r\n");
    return s.substr(l, r - l + 1);
}

std::string strip_comment(const std::string& s) {
    bool in_str = false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '"') in_str = !in_str;
        if (!in_str && s[i] == '#') {
            size_t end = i;
            while (end > 0 && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
            return s.substr(0, end);
        }
    }
    return s;
}

int leading_spaces(const std::string& s) {
    int n = 0;
    for (char c : s) {
        if (c == ' ') ++n;
        else break;
    }
    return n;
}

const char* env_or_empty(const char* name) {
    if (!name || !*name) return "";
    const char* v = std::getenv(name);
    return v ? v : "";
}

void apply_yaml_line(Config& cfg, const std::string& full_key, const std::string& val) {
    auto parse_bool = [](const std::string& v) {
        return v == "true" || v == "yes" || v == "1";
    };
    auto parse_int = [](const std::string& v) { return std::stoi(v); };

    try {
        if (full_key == "offline_pipeline_version")       cfg.offline_pipeline_version = val;
        else if (full_key == "grid_dt_ms")                cfg.grid_dt_ms = parse_int(val);
        else if (full_key == "join_tolerance_ms")         cfg.join_tolerance_ms = parse_int(val);
        else if (full_key == "telemetry_gap_ms")          cfg.telemetry_gap_ms = parse_int(val);
        else if (full_key == "audio_gap_factor")          cfg.audio_gap_factor = std::stod(val);
        else if (full_key == "max_timestamp_jump_ms")     cfg.max_timestamp_jump_ms = parse_int(val);
        else if (full_key == "source_contract")           cfg.source_contract = val;
        else if (full_key == "validation.min_valid_duration_s")    cfg.validation.min_valid_duration_s = std::stod(val);
        else if (full_key == "validation.fail_on_no_valid_ranges")  cfg.validation.fail_on_no_valid_ranges = parse_bool(val);
        else if (full_key == "sync.min_confidence")       cfg.sync.min_confidence = std::stod(val);
        else if (full_key == "sync.max_offset_search_s")  cfg.sync.max_offset_search_s = std::stod(val);
        else if (full_key == "sync.master_clock")         cfg.sync.master_clock = val;
        else if (full_key == "sync.fail_on_sync_error")   cfg.sync.fail_on_sync_error = parse_bool(val);
        else if (full_key == "mdf4.include_audio")        cfg.mdf4.include_audio = parse_bool(val);
        else if (full_key == "mdf4.audio_preview_rms")    cfg.mdf4.audio_preview_rms = parse_bool(val);
        else if (full_key == "mdf4.output_filename")      cfg.mdf4.output_filename = val;
        else if (full_key == "scene.segment_min_duration_ms")        cfg.scene.segment_min_duration_ms = parse_int(val);
        else if (full_key == "scene.classify_unknown_when_degraded")  cfg.scene.classify_unknown_when_degraded = parse_bool(val);
        else if (full_key == "fallback.ego_nav_sidecar_enabled")     cfg.fallback.ego_nav_sidecar_enabled = parse_bool(val);
        else if (full_key == "fallback.ego_nav_sidecar_filename")    cfg.fallback.ego_nav_sidecar_filename = val;
        else if (full_key == "debug.keep_intermediate")   cfg.debug.keep_intermediate = parse_bool(val);
        else if (full_key == "debug.intermediate_dir")    cfg.debug.intermediate_dir = val;
        else if (full_key == "s3.enabled")                cfg.s3.enabled = parse_bool(val);
        else if (full_key == "s3.endpoint")               cfg.s3.endpoint = val;
        else if (full_key == "s3.region")                 cfg.s3.region = val;
        else if (full_key == "s3.bucket")                 cfg.s3.bucket = val;
        else if (full_key == "s3.prefix")                 cfg.s3.prefix = val;
        else if (full_key == "s3.path_style")             cfg.s3.path_style = parse_bool(val);
        else if (full_key == "s3.access_key_env")         cfg.s3.access_key_env = val;
        else if (full_key == "s3.secret_key_env")         cfg.s3.secret_key_env = val;
        else if (full_key == "s3.access_key")            cfg.s3.access_key = val;
        else if (full_key == "s3.secret_key")             cfg.s3.secret_key = val;
        else if (full_key == "s3.require_pipeline_success") cfg.s3.require_pipeline_success = parse_bool(val);
        else if (full_key == "s3.require_real_mdf4")      cfg.s3.require_real_mdf4 = parse_bool(val);
        else if (full_key == "s3.max_retries")            cfg.s3.max_retries = parse_int(val);
        else if (full_key == "s3.retry_backoff_ms")       cfg.s3.retry_backoff_ms = parse_int(val);
        else if (full_key == "s3.include_session_files")  cfg.s3.include_session_files = parse_bool(val);
    } catch (...) {
    }
}

void parse_yaml_file(Config& cfg, const std::filesystem::path& path) {
    if (path.empty() || !std::filesystem::exists(path)) return;
    std::ifstream f(path);
    if (!f) return;

    std::string cur_section;
    std::string line;

    auto unquote = [](std::string v) {
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        return v;
    };

    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || trim(line)[0] == '#') continue;

        const int indent = leading_spaces(line);
        line = strip_comment(line);

        if (indent == 0) cur_section = "";
        std::string tl = trim(line);
        if (tl.empty()) continue;

        auto colon = tl.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(tl.substr(0, colon));
        std::string val = trim(tl.substr(colon + 1));

        if (val.empty() && indent == 0) {
            cur_section = key;
            continue;
        }
        if (val.empty()) continue;

        val = unquote(val);
        const std::string full_key = cur_section.empty() ? key : cur_section + "." + key;
        apply_yaml_line(cfg, full_key, val);
    }
}

} // namespace

Config Config::from_yaml(const std::filesystem::path& path) {
    Config cfg;
    parse_yaml_file(cfg, path);
    cfg.resolve_s3_credentials();
    return cfg;
}

Config Config::merge_yaml(Config base, const std::filesystem::path& path) {
    parse_yaml_file(base, path);
    base.resolve_s3_credentials();
    return base;
}

void Config::resolve_s3_credentials() {
    if (s3.include_files.empty()) {
        s3.include_files = {
            "session.mf4",
            "session.mf4.sha256",
            "session_report.json",
            "validation_report.json",
            "sync_report.json",
            "scene_events.json",
            "scene_segments.json",
            "data_quality_report.json",
            "events_report.json",
            "upload_manifest.json",
        };
    }

    if (s3.session_include_files.empty()) {
        s3.session_include_files = {
            "ego_manifest.json",
            "session_metadata.json",
            "scenario_metadata.json",
            "source_metadata.json",
            "final_runtime_summary.json",
            "source.bin",
            "ego.index",
        };
        if (!fallback.ego_nav_sidecar_filename.empty()) {
            s3.session_include_files.push_back(fallback.ego_nav_sidecar_filename);
        }
    }

    if (s3.access_key.empty()) {
        const char* v = env_or_empty(s3.access_key_env.c_str());
        if (v[0]) s3.access_key = v;
    }
    if (s3.secret_key.empty()) {
        const char* v = env_or_empty(s3.secret_key_env.c_str());
        if (v[0]) s3.secret_key = v;
    }
    if (s3.access_key.empty()) {
        const char* v = env_or_empty("EGO_S3_ACCESS_KEY");
        if (v[0]) s3.access_key = v;
    }
    if (s3.secret_key.empty()) {
        const char* v = env_or_empty("EGO_S3_SECRET_KEY");
        if (v[0]) s3.secret_key = v;
    }
}

} // namespace ego_offline
