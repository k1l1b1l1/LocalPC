#pragma once

#include <cstdint>
#include <string>

namespace ego_runtime {

enum class IntegrityResult {
    kOk,
    kWarning,
    kFailed
};

struct IntegrityReport {
    IntegrityResult result = IntegrityResult::kFailed;
    std::string detail;
};

IntegrityReport CheckSessionIntegrity(const std::string& session_dir,
                                      std::uint32_t max_payload_bytes = 65536U);

}  // namespace ego_runtime
