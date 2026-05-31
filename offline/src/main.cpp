// ego-offline — Offline pipeline for ego + source binary log processing
// CLI: ego-offline process|validate|upload|export-mdf4 [options]
//
// Exit codes: 0=success, 1=validation, 2=sync, 3=internal, 4=upload_failed, 5=upload_blocked

#include "ego_offline/config.hpp"
#include "ego_offline/types.hpp"
#include "ego_offline/pipeline/pipeline.hpp"
#include "ego_offline/upload/s3_uploader.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#include <cstdlib>

namespace fs = std::filesystem;

static void print_usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog << " process     --session-dir <path> [--source-bin <path>] [--config <yaml>] [--s3-config <yaml>] [--skip-s3]\n"
        << "  " << prog << " validate    --session-dir <path> [--config <yaml>] [--out <dir>]\n"
        << "  " << prog << " upload      --session-dir <path> [--s3-config <yaml>] [--dry-run]\n"
        << "  " << prog << " export-mdf4 --session-dir <path> [--from-stage <aligned|mdf>]\n"
        << "\nOptions:\n"
        << "  --only validate|sync|mdf4|scenes   Run only up to that stage\n"
        << "  --skip-s3                          Skip automatic S3 upload after process\n"
        << "  --s3-config <yaml>                 S3 settings (merge into config)\n"
        << "  --dry-run                          Plan S3 keys without upload (upload cmd)\n"
        << "\nExit codes: 0=ok 1=validation 2=sync 3=internal 4=upload_failed 5=upload_blocked\n";
}

static std::string get_opt(const std::vector<std::string>& args,
                           const std::string& flag,
                           const std::string& def = {}) {
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (args[i] == flag) return args[i+1];
    return def;
}

static bool has_flag(const std::vector<std::string>& args, const std::string& flag) {
    return std::find(args.begin(), args.end(), flag) != args.end();
}

static ego_offline::Config load_config(const ego_offline::pipeline::RunOptions& opts) {
    ego_offline::Config cfg = opts.config_path.empty()
        ? ego_offline::Config::defaults()
        : ego_offline::Config::from_yaml(opts.config_path);

    if (!opts.s3_config_path.empty())
        cfg = ego_offline::Config::merge_yaml(std::move(cfg), opts.s3_config_path);
    else if (!opts.config_path.empty()) {
        const fs::path sibling_s3 = opts.config_path.parent_path() / "s3.local.yaml";
        if (fs::exists(sibling_s3))
            cfg = ego_offline::Config::merge_yaml(std::move(cfg), sibling_s3);
    }

    // Fallback: config/s3.local.yaml from cwd (gitignored secrets)
    const fs::path local_s3 = fs::path("config") / "s3.local.yaml";
    if (fs::exists(local_s3) && opts.s3_config_path.empty())
        cfg = ego_offline::Config::merge_yaml(std::move(cfg), local_s3);

    return cfg;
}

int main(int argc, char** argv) {
    if (argc < 2) { print_usage(argv[0]); return 3; }

    std::string cmd = argv[1];
    std::vector<std::string> args(argv + 2, argv + argc);

    if (cmd == "--help" || cmd == "-h") { print_usage(argv[0]); return 0; }
    if (cmd == "--version") { std::cout << "ego-offline 1.0\n"; return 0; }

    ego_offline::pipeline::RunOptions opts;

    opts.session_dir      = get_opt(args, "--session-dir");
    opts.source_bin       = get_opt(args, "--source-bin");
    opts.config_path      = get_opt(args, "--config");
    opts.s3_config_path   = get_opt(args, "--s3-config");
    opts.out_dir          = get_opt(args, "--out");
    opts.skip_s3          = has_flag(args, "--skip-s3");
    opts.upload_dry_run   = has_flag(args, "--dry-run");

    if (opts.session_dir.empty()) {
        std::cerr << "Error: --session-dir is required\n";
        print_usage(argv[0]);
        return static_cast<int>(ego_offline::ExitCode::internal_error);
    }

    if (!fs::exists(opts.session_dir)) {
        std::cerr << "Error: session-dir does not exist: " << opts.session_dir << '\n';
        return static_cast<int>(ego_offline::ExitCode::internal_error);
    }

    std::string only = get_opt(args, "--only");
    if (!only.empty()) {
        opts.only_validate = (only == "validate");
        opts.only_sync     = (only == "sync");
        opts.only_mdf4     = (only == "mdf4");
        opts.only_scenes   = (only == "scenes");
    }

    ego_offline::Config cfg = load_config(opts);
    ego_offline::pipeline::Pipeline pipeline(cfg);

    if (cmd == "upload") {
        opts.only_upload = true;
        if (!cfg.s3.enabled) cfg.s3.enabled = true;
        ego_offline::upload::S3Uploader uploader(cfg);
        const fs::path offline = opts.out_dir.empty()
            ? opts.session_dir / "offline"
            : fs::path(opts.out_dir);
        return static_cast<int>(uploader.upload_session(opts.session_dir, offline, opts.upload_dry_run));
    }

    ego_offline::ExitCode code;
    if (cmd == "validate") {
        opts.only_validate = true;
        code = pipeline.run(opts);
    } else if (cmd == "process") {
        code = pipeline.run(opts);
    } else if (cmd == "export-mdf4") {
        opts.only_mdf4 = true;
        code = pipeline.run(opts);
    } else {
        std::cerr << "Unknown command: " << cmd << '\n';
        print_usage(argv[0]);
        return static_cast<int>(ego_offline::ExitCode::internal_error);
    }

    return static_cast<int>(code);
}
