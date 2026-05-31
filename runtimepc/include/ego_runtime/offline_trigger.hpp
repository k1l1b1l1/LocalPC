#pragma once

#include "ego_runtime/config.hpp"

#include <string>

namespace ego_runtime {

struct OfflineTriggerResult {
    bool started = false;
    std::string error;
    int pid = 0;
};

// Spawn ego-offline process after session finalize (detached by default).
OfflineTriggerResult TriggerOfflinePipeline(const RuntimeConfig& config,
                                            const std::string& session_dir);

}  // namespace ego_runtime
