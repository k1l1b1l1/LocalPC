#include "ego_runtime/session_manager.hpp"

#include <filesystem>
#include <sstream>

#include "ego_runtime/util.hpp"

namespace ego_runtime {

const char* SessionStateToString(SessionState state) {
    switch (state) {
        case SessionState::kIdle:
            return "Idle";
        case SessionState::kRecording:
            return "Recording";
        case SessionState::kStopping:
            return "Stopping";
        case SessionState::kClosed:
            return "Closed";
        default:
            return "Error";
    }
}

SessionManager::SessionManager(RuntimeConfig config) : config_(std::move(config)) {}

SessionState SessionManager::State() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) {
        return SessionState::kIdle;
    }
    return active_->state;
}

std::optional<SessionInfo> SessionManager::ActiveSession() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_;
}

std::string SessionManager::SessionDir() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) {
        return {};
    }
    return active_->storage_path;
}

void SessionManager::SetSourceIp(const std::string& ip) {
    std::lock_guard<std::mutex> lock(mu_);
    if (active_) {
        active_->source_ip = ip;
    }
}

RuntimeErrorCode SessionManager::Start(const ScenarioMetadata& scenario, const std::string& source_ip) {
    std::lock_guard<std::mutex> lock(mu_);
    if (active_ && active_->state == SessionState::kRecording) {
        return RuntimeErrorCode::kSessionBusy;
    }
    scenario_ = scenario;
    SessionInfo info{};
    info.session_id = MakeSessionId();
    info.started_at_utc = UtcNowIso8601();
    info.source_ip = source_ip;
    info.state = SessionState::kRecording;
    info.storage_path =
        (std::filesystem::path(config_.data_root) / "sessions" / info.session_id).string();
    std::filesystem::create_directories(info.storage_path);
    std::filesystem::create_directories(info.storage_path + "/logs");
    active_ = info;
    return RuntimeErrorCode::kOk;
}

RuntimeErrorCode SessionManager::Stop(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_ || active_->state != SessionState::kRecording) {
        return RuntimeErrorCode::kNotRecording;
    }
    active_->state = SessionState::kStopping;
    active_->stop_reason = reason;
    return RuntimeErrorCode::kOk;
}

RuntimeErrorCode SessionManager::EmergencyStop(const std::string& reason) {
    return Stop(reason);
}

void SessionManager::MarkStopped(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) {
        return;
    }
    active_->stopped_at_utc = UtcNowIso8601();
    active_->stop_reason = reason;
    active_->state = SessionState::kClosed;
}

void SessionManager::SetError(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) {
        return;
    }
    active_->stop_reason = reason;
    active_->state = SessionState::kError;
}

void SessionManager::WriteSessionMetadata() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) {
        return;
    }
    std::ostringstream json;
    json << "{\n";
    json << "  \"session_id\": \"" << JsonEscape(active_->session_id) << "\",\n";
    json << "  \"started_at_utc\": \"" << JsonEscape(active_->started_at_utc) << "\",\n";
    json << "  \"stopped_at_utc\": \"" << JsonEscape(active_->stopped_at_utc) << "\",\n";
    json << "  \"vehicle_id\": \"" << JsonEscape(config_.vehicle_id) << "\",\n";
    json << "  \"test_stand_config\": \""
         << JsonEscape(config_.test_stand_config.empty() ? "default" : config_.test_stand_config) << "\",\n";
    json << "  \"software_version\": \"" << JsonEscape(config_.software_version) << "\",\n";
    json << "  \"runtime_version\": \"" << JsonEscape(config_.software_version) << "\",\n";
    json << "  \"protocol_version\": \"1\",\n";
    json << "  \"prod_protocol_version\": 1,\n";
    json << "  \"source_ip\": \"" << JsonEscape(active_->source_ip) << "\",\n";
    json << "  \"storage_path\": \"" << JsonEscape(active_->storage_path) << "\",\n";
    json << "  \"stop_reason\": \"" << JsonEscape(active_->stop_reason) << "\"\n";
    json << "}\n";
    WriteTextAtomic(active_->storage_path + "/session_metadata.json", json.str());
}

void SessionManager::WriteScenarioMetadata(const ScenarioMetadata& scenario) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!active_) {
        return;
    }
    std::ostringstream json;
    json << "{\n";
    json << "  \"scenario_id\": \"" << JsonEscape(scenario.scenario_id) << "\",\n";
    json << "  \"scenario_name\": \"" << JsonEscape(scenario.scenario_name) << "\",\n";
    json << "  \"operator\": \"" << JsonEscape(scenario.operator_name) << "\",\n";
    json << "  \"notes\": \"" << JsonEscape(scenario.notes) << "\"\n";
    json << "}\n";
    WriteTextAtomic(active_->storage_path + "/scenario_metadata.json", json.str());
}

}  // namespace ego_runtime
