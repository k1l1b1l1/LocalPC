#pragma once

#include <cstdint>
#include <string>

#include "ego_runtime/config.hpp"

namespace ego_runtime {

struct NavHistoryMaterializeResult {
    std::string history_path;
    std::string sidecar_path;
    std::uint64_t copied_samples = 0U;
    std::uint64_t skipped_lines = 0U;
    bool used_existing_sidecar = false;
    bool history_available = false;
};

std::string ResolveNavHistoryPath(const RuntimeConfig& config);
std::string ResolveNavHistoryPath(const std::string& runtime_root);
std::uint64_t UtcNowNs();

NavHistoryMaterializeResult MaterializeNavHistoryWindow(const std::string& runtime_root,
                                                        const std::string& session_dir,
                                                        const std::string& sidecar_filename,
                                                        std::uint64_t start_ts_ns,
                                                        std::uint64_t end_ts_ns);

}  // namespace ego_runtime
