#include "ego_offline/config.hpp"

#include <cassert>
#include <filesystem>
#include <iostream>

int main() {
    namespace fs = std::filesystem;
    const fs::path stand = fs::path(__FILE__).parent_path().parent_path() / "config" / "stand.pc.yaml";
    assert(fs::exists(stand));

    const ego_offline::Config cfg = ego_offline::Config::from_yaml(stand);

    if (cfg.validation.min_valid_duration_s != 0.5) {
        std::cerr << "FAIL: min_valid_duration_s=" << cfg.validation.min_valid_duration_s
                  << " telemetry_gap_ms=" << cfg.telemetry_gap_ms << " path=" << stand << '\n';
        return 1;
    }
    if (cfg.telemetry_gap_ms != 200) {
        std::cerr << "FAIL: telemetry_gap_ms=" << cfg.telemetry_gap_ms << '\n';
        return 1;
    }
    if (cfg.sync.fail_on_sync_error != false) {
        std::cerr << "FAIL: fail_on_sync_error not false\n";
        return 1;
    }

    std::cout << "[PASS] stand.pc.yaml parsed: min_valid_duration_s="
              << cfg.validation.min_valid_duration_s << '\n';
    return 0;
}
