#pragma once
// Pipeline orchestrator — runs all stages in order

#include "ego_offline/types.hpp"
#include "ego_offline/config.hpp"
#include "ego_offline/validate/log_validator.hpp"
#include "ego_offline/sync/time_sync.hpp"
#include "ego_offline/finalize/scene_builder.hpp"
#include "ego_offline/reports/report_builder.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ego_offline::pipeline {

struct RunOptions {
    std::filesystem::path session_dir;
    std::filesystem::path source_bin;     // optional override
    std::filesystem::path config_path;    // optional
    std::filesystem::path out_dir;        // default: session_dir/offline
    bool only_validate  = false;
    bool only_sync      = false;
    bool only_mdf4      = false;
    bool only_scenes    = false;
    bool skip_s3        = false;
    bool only_upload    = false;
    bool upload_dry_run = false;
    std::filesystem::path s3_config_path;
};

class Pipeline {
public:
    explicit Pipeline(const Config& cfg);

    // Main entry point
    ExitCode run(const RunOptions& opts);

    // Sub-commands
    ExitCode run_validate(const RunOptions& opts);
    ExitCode run_full(const RunOptions& opts);

private:
    Config cfg_;

    std::filesystem::path out_dir(const RunOptions& opts) const;
};

} // namespace ego_offline::pipeline
