#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "ego_runtime/config.hpp"

namespace ego_runtime {

enum class SessionState {
    kIdle,
    kRecording,
    kStopping,
    kClosed,
    kError
};

enum class RuntimeErrorCode {
    kOk = 0,
    kSessionBusy = 1,
    kNotRecording = 2,
    kStorageCritical = 3,
    kConfigError = 4,
    kInternalError = 5
};

struct ScenarioMetadata {
    std::string scenario_id;
    std::string scenario_name;
    std::string operator_name;
    std::string notes;
};

struct SessionInfo {
    std::string session_id;
    std::string storage_path;
    std::string started_at_utc;
    std::string stopped_at_utc;
    std::string source_ip;
    std::string stop_reason;
    SessionState state = SessionState::kIdle;
};

class SessionManager {
public:
    explicit SessionManager(RuntimeConfig config);

    RuntimeErrorCode Start(const ScenarioMetadata& scenario,
                           const std::string& source_ip = "",
                           const std::string& session_id = "");
    RuntimeErrorCode Resume(const ScenarioMetadata& scenario,
                            const std::string& session_id,
                            const std::string& storage_path,
                            const std::string& started_at_utc = "");
    RuntimeErrorCode Stop(const std::string& reason = "user_stop");
    RuntimeErrorCode EmergencyStop(const std::string& reason);

    SessionState State() const;
    std::optional<SessionInfo> ActiveSession() const;
    std::string SessionDir() const;

    void SetSourceIp(const std::string& ip);
    void WriteSessionMetadata() const;
    void WriteScenarioMetadata(const ScenarioMetadata& scenario) const;
    void MarkStopped(const std::string& reason);
    void SetError(const std::string& reason);

    const RuntimeConfig& Config() const { return config_; }

private:
    mutable std::mutex mu_{};
    RuntimeConfig config_;
    std::optional<SessionInfo> active_{};
    ScenarioMetadata scenario_{};
};

const char* SessionStateToString(SessionState state);

}  // namespace ego_runtime
