// IT-03: integration test on ego-runtime e2e session directory
#include "ego_offline/config.hpp"
#include "ego_offline/pipeline/pipeline.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static fs::path repo_root() {
    return fs::path(__FILE__).parent_path().parent_path().parent_path().parent_path();
}

static fs::path find_e2e_session() {
    const fs::path base = repo_root() / "runtimepc" / "e2e_file_test" / "sessions";
    if (!fs::exists(base)) return {};
    for (const auto& entry : fs::directory_iterator(base)) {
        if (entry.is_directory() && entry.path().filename().string().rfind("session-", 0) == 0)
            return entry.path();
    }
    return {};
}

int main() {
    const fs::path session = find_e2e_session();
    if (session.empty()) {
        std::cout << "[SKIP] no runtimepc/e2e_file_test/sessions/session-* directory\n";
        return 0;
    }

    const fs::path cfg_path = repo_root() / "pipeline" / "offline" / "config" / "stand.pc.yaml";
    if (!fs::exists(cfg_path)) {
        std::cerr << "[FAIL] missing config: " << cfg_path << '\n';
        return 1;
    }

    ego_offline::Config cfg = ego_offline::Config::from_yaml(cfg_path);
    if (cfg.validation.min_valid_duration_s != 0.5) {
        std::cerr << "[FAIL] stand.pc.yaml not loaded (min_valid_duration_s="
                  << cfg.validation.min_valid_duration_s << ")\n";
        return 1;
    }

    ego_offline::pipeline::RunOptions opts;
    opts.session_dir = session;
    opts.config_path = cfg_path.string();
    opts.only_validate = true;

    ego_offline::pipeline::Pipeline pipeline(cfg);
    const ego_offline::ExitCode code = pipeline.run(opts);
    if (code != ego_offline::ExitCode::success) {
        std::cerr << "[FAIL] validate exit code " << static_cast<int>(code)
                  << " for " << session << '\n';
        return 1;
    }

    std::cout << "[PASS] e2e validate " << session.filename().string() << '\n';
    return 0;
}
