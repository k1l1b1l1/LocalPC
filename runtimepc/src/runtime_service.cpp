#include "ego_runtime/runtime_service.hpp"

#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#include "ego_runtime/contract_frame_io.hpp"
#include "ego_runtime/session_integrity.hpp"
#include "ego_runtime/offline_trigger.hpp"
#include "ego_runtime/util.hpp"
#include "ego_protocol_packets.hpp"

namespace ego_runtime {
namespace {

using ego::protocol::v1::EgoFrameHeader;
using ego::protocol::v1::EGO_FRAME_MAGIC;
using ego::protocol::v1::FramePayloadType;

}  // namespace

RuntimeService::RuntimeService(RuntimeConfig config) : config_(std::move(config)), sessions_(config_) {}

RuntimeService::~RuntimeService() {
    StopDaemon();
    RemovePidFile(config_);
}

ScenarioMetadata RuntimeService::DefaultScenario() const {
    ScenarioMetadata s{};
    s.scenario_id = config_.scenario_id.empty() ? "default" : config_.scenario_id;
    s.scenario_name = config_.scenario_name.empty() ? "session" : config_.scenario_name;
    s.operator_name = config_.operator_name.empty() ? "operator" : config_.operator_name;
    s.notes = config_.notes;
    return s;
}

double RuntimeService::RecordingDurationSec() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - recording_start_).count();
}

RuntimeErrorCode RuntimeService::StartRecording(const ScenarioMetadata& scenario) {
    if (!config_.input_file.empty()) {
        return StartLocalRecording(scenario);
    }
    return StartBoardSession(scenario);
}

RuntimeErrorCode RuntimeService::StartLocalRecording(const ScenarioMetadata& scenario) {
    std::lock_guard<std::recursive_mutex> lock(service_mu_);
    const RuntimeErrorCode rc = sessions_.Start(scenario);
    if (rc != RuntimeErrorCode::kOk) {
        return rc;
    }
    const std::string dir = sessions_.SessionDir();
    error_log_ = std::make_unique<ErrorLog>(dir + "/logs/runtime_error.log");
    buffer_ = std::make_unique<PacketBuffer>(config_.packet_buffer_capacity, config_.packet_buffer_max_bytes);
    writer_ = std::make_unique<ChunkWriter>(dir, config_);
    if (!writer_->Open()) {
        error_log_->Write(LogLevel::kError, "failed to open chunk writer");
        sessions_.SetError("chunk_open_failed");
        return RuntimeErrorCode::kInternalError;
    }
    sessions_.WriteScenarioMetadata(scenario);
    seen_ts_ = false;
    last_ts_ns_ = 0U;
    seq_initialized_ = false;
    last_seq_ = 0U;
    rate_start_ = std::chrono::steady_clock::now();
    recording_start_ = rate_start_;
    return RuntimeErrorCode::kOk;
}

RuntimeErrorCode RuntimeService::StartBoardSession(const ScenarioMetadata& scenario) {
    std::lock_guard<std::recursive_mutex> lock(service_mu_);
    if (sessions_.State() == SessionState::kRecording) {
        return RuntimeErrorCode::kSessionBusy;
    }

    const std::string session_id = MakeSessionId();
    control_client_ = std::make_unique<ContractControlClient>(config_);
    if (!control_client_->Connect()) {
        std::cerr << "control connect failed: " << control_client_->LastError() << "\n";
        control_client_.reset();
        return RuntimeErrorCode::kInternalError;
    }

    const ControlHelloResult hello = control_client_->Hello("ego-runtime");
    if (!hello.ok) {
        std::cerr << "control hello failed: " << hello.error << "\n";
        control_client_.reset();
        return RuntimeErrorCode::kInternalError;
    }

    const ControlStartSessionResult start = control_client_->StartSession(scenario, session_id);
    if (!start.ok) {
        std::cerr << "start_session rejected: " << start.error << "\n";
        control_client_.reset();
        return RuntimeErrorCode::kInternalError;
    }
    board_session_id_ = start.session_id.empty() ? session_id : start.session_id;

    ResetFrameWaitFlags();
    const RuntimeErrorCode rc = sessions_.Start(scenario, "", board_session_id_);
    if (rc != RuntimeErrorCode::kOk) {
        control_client_.reset();
        return rc;
    }
    const std::string dir = sessions_.SessionDir();
    error_log_ = std::make_unique<ErrorLog>(dir + "/logs/runtime_error.log");
    buffer_ = std::make_unique<PacketBuffer>(config_.packet_buffer_capacity, config_.packet_buffer_max_bytes);
    writer_ = std::make_unique<ChunkWriter>(dir, config_);
    if (!writer_->Open()) {
        error_log_->Write(LogLevel::kError, "failed to open chunk writer");
        sessions_.SetError("chunk_open_failed");
        control_client_.reset();
        return RuntimeErrorCode::kInternalError;
    }
    sessions_.WriteScenarioMetadata(scenario);
    seen_ts_ = false;
    last_ts_ns_ = 0U;
    seq_initialized_ = false;
    last_seq_ = 0U;
    rate_start_ = std::chrono::steady_clock::now();
    recording_start_ = rate_start_;

    if (!EnsureDataClient()) {
        error_log_->Write(LogLevel::kError, "data connect failed after control start");
        sessions_.SetError("data_connect_failed");
        writer_->Close();
        writer_.reset();
        buffer_.reset();
        error_log_.reset();
        sessions_.MarkStopped("data_connect_failed");
        control_client_.reset();
        return RuntimeErrorCode::kInternalError;
    }

    if (!WaitForDataFrame(static_cast<std::uint32_t>(FramePayloadType::SESSION_STARTED),
                          std::chrono::seconds(5))) {
        error_log_->Write(LogLevel::kWarning, "timeout waiting for SessionStarted frame");
    }
    if (!WaitForDataFrame(static_cast<std::uint32_t>(FramePayloadType::CONFIG_SNAPSHOT),
                          std::chrono::seconds(5))) {
        error_log_->Write(LogLevel::kWarning, "timeout waiting for ConfigSnapshot frame");
    }

    return RuntimeErrorCode::kOk;
}

bool RuntimeService::EnsureDataClient() {
    if (config_.input_file.empty() && !contract_client_) {
        contract_client_ = std::make_unique<ContractTcpClient>(
            config_, [this](ContractFrame f) { OnContractFrame(std::move(f)); });
    }
    if (contract_client_ && !contract_client_->Running()) {
        return contract_client_->Start();
    }
    return contract_client_ != nullptr && contract_client_->Running();
}

void RuntimeService::ResetFrameWaitFlags() {
    std::lock_guard<std::mutex> lock(frame_wait_mu_);
    session_started_seen_ = false;
    config_snapshot_seen_ = false;
    session_ended_seen_ = false;
}

bool RuntimeService::WaitForDataFrame(const std::uint32_t frame_type,
                                      const std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(frame_wait_mu_);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto is_ready = [this, frame_type]() {
        if (frame_type == static_cast<std::uint32_t>(FramePayloadType::SESSION_STARTED)) {
            return session_started_seen_;
        }
        if (frame_type == static_cast<std::uint32_t>(FramePayloadType::CONFIG_SNAPSHOT)) {
            return config_snapshot_seen_;
        }
        if (frame_type == static_cast<std::uint32_t>(FramePayloadType::SESSION_ENDED)) {
            return session_ended_seen_;
        }
        return false;
    };
    return frame_wait_cv_.wait_until(lock, deadline, is_ready);
}

RuntimeErrorCode RuntimeService::StopRecording(const std::string& reason) {
    return StopBoardSession(reason);
}

RuntimeErrorCode RuntimeService::StopBoardSession(const std::string& reason) {
    std::string board_id;
    {
        std::lock_guard<std::recursive_mutex> lock(service_mu_);
        if (finalize_in_progress_.load()) {
            return RuntimeErrorCode::kOk;
        }
        const SessionState state = sessions_.State();
        if (state != SessionState::kRecording && state != SessionState::kStopping) {
            return RuntimeErrorCode::kNotRecording;
        }
        board_id = board_session_id_;
    }

    bool control_stop_ok = false;
    if (control_client_ && !board_id.empty()) {
        const ControlStopSessionResult stop = control_client_->StopSession(board_id, reason);
        control_stop_ok = stop.ok;
        if (!stop.ok && error_log_) {
            error_log_->Write(LogLevel::kWarning, "control stop_session: " + stop.error);
        }
    }

    if (contract_client_ && contract_client_->Running()) {
        // PC GUI may close ego TCP before Pi stop — don't block finalize on SESSION_ENDED.
        const std::chrono::seconds ended_wait =
            control_stop_ok ? std::chrono::seconds(3) : std::chrono::seconds(1);
        if (!WaitForDataFrame(static_cast<std::uint32_t>(FramePayloadType::SESSION_ENDED), ended_wait)) {
            if (error_log_) {
                error_log_->Write(LogLevel::kWarning, "timeout waiting for SessionEnded frame");
            }
        }
    }

    if (contract_client_) {
        contract_client_->Stop();
        contract_client_.reset();
    }
    control_client_.reset();

    std::lock_guard<std::recursive_mutex> lock(service_mu_);
    board_session_id_.clear();
    finalize_in_progress_ = true;
    sessions_.Stop(reason);
    if (!sync_file_mode_ && buffer_) {
        for (int i = 0; i < 500; ++i) {
            if (buffer_->Size() == 0U) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    } else if (sync_file_mode_) {
        DrainPacketBuffer();
    }
    FinalizeActiveSession(reason);
    finalize_in_progress_ = false;
    return RuntimeErrorCode::kOk;
}

RuntimeErrorCode RuntimeService::EmergencyStop(const std::string& reason) {
    return StopRecording(reason);
}

bool RuntimeService::StartDaemon() {
    if (daemon_running_.load()) {
        return true;
    }
    stop_threads_ = false;
    daemon_running_ = true;
    sync_file_mode_ = !config_.input_file.empty();

    storage_ = std::make_unique<StorageMonitor>(config_, [this](StorageLevel level, const StorageStatus& st) {
        diagnostics_.SetDiskFreeGb(st.free_gb);
        if (level == StorageLevel::kWarning) {
            if (error_log_) {
                error_log_->Write(LogLevel::kWarning, "storage warning");
            }
            diagnostics_.SetHealth("degraded");
        }
        if (level == StorageLevel::kCritical) {
            if (error_log_) {
                error_log_->Write(LogLevel::kError, "storage critical - emergency stop");
            }
            EmergencyStop("storage_critical");
        }
    });
    storage_->Start();

    if (!sync_file_mode_) {
        writer_thread_ = std::thread([this]() { WriterLoop(); });
        report_thread_ = std::thread([this]() { ReportLoop(); });
    }
    return true;
}

bool RuntimeService::StartControlServer() {
    control_server_ = std::make_unique<ControlServer>(config_, [this](const std::string& cmd) {
        return HandleControlCommand(cmd);
    });
    if (!control_server_->Start()) {
        return false;
    }
    WritePidFile(config_);
    return true;
}

void RuntimeService::StopDaemon() {
    stop_threads_ = true;
    if (control_server_) {
        control_server_->Stop();
        control_server_.reset();
    }
    RemovePidFile(config_);
    if (control_client_) {
        control_client_->Disconnect();
        control_client_.reset();
    }
    if (contract_client_) {
        contract_client_->Stop();
        contract_client_.reset();
    }
    if (storage_) {
        storage_->Stop();
        storage_.reset();
    }
    if (writer_thread_.joinable()) {
        writer_thread_.join();
    }
    if (report_thread_.joinable()) {
        report_thread_.join();
    }
    daemon_running_ = false;
}

void RuntimeService::ProcessFileInput(const std::string& path) {
    std::string error;
    ReadContractFramesFromFile(
        path, config_.max_payload_bytes,
        [this](const std::vector<std::uint8_t>& frame) {
            OnContractFrame(ContractFrame{frame});
            return true;
        },
        &error);
    if (!error.empty() && error_log_) {
        error_log_->Write(LogLevel::kWarning, error);
    }
}

void RuntimeService::TrackContractSeq(const std::uint64_t seq) {
    if (!seq_initialized_) {
        seq_initialized_ = true;
        last_seq_ = seq;
        return;
    }
    if (seq <= last_seq_) {
        return;
    }
    if (seq > last_seq_ + 1U) {
        diagnostics_.OnSeqGap(seq - last_seq_ - 1U);
    }
    last_seq_ = seq;
}

void RuntimeService::OnContractFrame(ContractFrame frame) {
    std::string error;
    if (!ValidateContractFrame(frame.bytes, config_.max_payload_bytes, &error)) {
        diagnostics_.OnBadPacket(error.empty() ? "invalid_frame" : error);
        if (error_log_) {
            error_log_->Write(LogLevel::kWarning, error);
        }
        return;
    }

    EgoFrameHeader header{};
    std::memcpy(&header, frame.bytes.data(), sizeof(header));

    if (config_.auto_start_on_session_started && sessions_.State() == SessionState::kIdle &&
        header.frame_type == static_cast<std::uint32_t>(FramePayloadType::SESSION_STARTED)) {
        StartRecording(DefaultScenario());
    }

    {
        std::lock_guard<std::mutex> lock(frame_wait_mu_);
        if (header.frame_type == static_cast<std::uint32_t>(FramePayloadType::SESSION_STARTED)) {
            session_started_seen_ = true;
        } else if (header.frame_type == static_cast<std::uint32_t>(FramePayloadType::CONFIG_SNAPSHOT)) {
            config_snapshot_seen_ = true;
        } else if (header.frame_type == static_cast<std::uint32_t>(FramePayloadType::SESSION_ENDED)) {
            session_ended_seen_ = true;
        }
        frame_wait_cv_.notify_all();
    }

    if (header.frame_type == static_cast<std::uint32_t>(FramePayloadType::SESSION_ENDED)) {
        return;
    }

    diagnostics_.OnPacketReceived();

    bool is_replay = false;
    if (seq_initialized_ && header.seq <= last_seq_) {
        is_replay = true;
        diagnostics_.OnPacketReplayed();
    } else {
        TrackContractSeq(header.seq);
        if (seen_ts_) {
            const std::uint64_t gap_ns = config_.time_gap_threshold_ms * 1'000'000ULL;
            if (header.t0_ns > last_ts_ns_ && (header.t0_ns - last_ts_ns_) > gap_ns) {
                diagnostics_.OnTimeGap();
            }
        }
        last_ts_ns_ = header.t0_ns;
        seen_ts_ = true;
    }

    if (sessions_.State() != SessionState::kRecording || is_replay) {
        return;
    }

    if (sync_file_mode_ && writer_) {
        if (writer_->WriteContractFrame(frame.bytes)) {
            diagnostics_.OnPacketWritten(frame.bytes.size());
        }
        return;
    }
    if (!buffer_) {
        return;
    }
    BufferedPacket item{};
    item.bytes = std::move(frame.bytes);
    buffer_->Push(std::move(item));
    diagnostics_.SetBufferDropped(buffer_->Dropped());
    if (buffer_->Degraded()) {
        diagnostics_.SetHealth("degraded");
    }
}

void RuntimeService::DrainPacketBuffer() {
    if (!buffer_ || !writer_) {
        return;
    }
    EgoRawWriter raw(*writer_);
    for (int attempt = 0; attempt < 200; ++attempt) {
        bool wrote_any = false;
        while (auto packet = buffer_->Pop()) {
            wrote_any = true;
            if (raw.Write(packet->bytes)) {
                diagnostics_.OnPacketWritten(packet->bytes.size());
            } else if (error_log_) {
                error_log_->Write(LogLevel::kError, "disk write failed during drain");
                sessions_.SetError("disk_write_failed");
                diagnostics_.SetHealth("failed");
                return;
            }
        }
        if (!wrote_any && buffer_->Size() == 0U) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void RuntimeService::WriterLoop() {
    while (!stop_threads_.load()) {
        const SessionState state = sessions_.State();
        if (state != SessionState::kRecording && state != SessionState::kStopping) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (!buffer_ || !writer_) {
            if (state == SessionState::kStopping) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        EgoRawWriter raw(*writer_);
        auto packet = buffer_->Pop();
        if (!packet.has_value()) {
            if (state == SessionState::kStopping) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (raw.Write(packet->bytes)) {
            diagnostics_.OnPacketWritten(packet->bytes.size());
        } else if (error_log_) {
            error_log_->Write(LogLevel::kError, "disk write failed");
            sessions_.SetError("disk_write_failed");
            diagnostics_.SetHealth("failed");
        }
    }
}

void RuntimeService::ReportLoop() {
    while (!stop_threads_.load()) {
        WriteRuntimeReport();
        for (std::uint32_t i = 0U; i < config_.report_interval_sec * 10U && !stop_threads_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void RuntimeService::WriteRuntimeReport() const {
    if (sessions_.SessionDir().empty()) {
        return;
    }
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - rate_start_).count();
    const RuntimeMetrics m = diagnostics_.Snapshot(elapsed);
    std::ostringstream json;
    json << "{\n";
    json << "  \"packets_received\": " << m.packets_received << ",\n";
    json << "  \"packets_written\": " << m.packets_written << ",\n";
    json << "  \"bad_packets\": " << m.bad_packets << ",\n";
    json << "  \"packet_loss\": " << m.packets_lost << ",\n";
    json << "  \"seq_gaps\": " << m.seq_gaps << ",\n";
    json << "  \"out_of_order\": " << m.out_of_order << ",\n";
    json << "  \"packets_replayed\": " << m.packets_replayed << ",\n";
    json << "  \"time_gaps\": " << m.time_gaps << ",\n";
    json << "  \"buffer_dropped\": " << m.buffer_dropped << ",\n";
    json << "  \"disk_free_gb\": " << m.disk_free_gb << ",\n";
    json << "  \"write_mbps\": " << m.write_mbps << ",\n";
    json << "  \"health\": \"" << JsonEscape(m.health) << "\",\n";
    json << "  \"adsp_status\": \"" << JsonEscape(m.adsp_status) << "\",\n";
    json << "  \"reject_by_reason\": {\n";
    bool first = true;
    for (const auto& entry : m.reject_by_reason) {
        if (!first) {
            json << ",\n";
        }
        first = false;
        json << "    \"" << JsonEscape(entry.first) << "\": " << entry.second;
    }
    json << "\n  }\n";
    json << "}\n";
    WriteTextAtomic(sessions_.SessionDir() + "/runtime_report.json", json.str());
}

void RuntimeService::FinalizeActiveSession(const std::string& reason) {
    const std::string session_dir = sessions_.SessionDir();
    if (!sync_file_mode_) {
        stop_threads_ = true;
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
        if (report_thread_.joinable()) {
            report_thread_.join();
        }
        DrainPacketBuffer();
    }
    if (writer_) {
        writer_->Close();
    }
    sessions_.MarkStopped(reason);
    sessions_.WriteSessionMetadata();
    const IntegrityReport integrity = CheckSessionIntegrity(sessions_.SessionDir(), config_.max_payload_bytes);
    WriteFinalSummary(integrity, reason);
    WriteRuntimeReport();
    writer_.reset();
    buffer_.reset();
    error_log_.reset();
    stop_threads_ = false;

    if (!session_dir.empty()) {
        TriggerOfflinePipeline(config_, session_dir);
    }

    if (!sync_file_mode_ && daemon_running_.load()) {
        writer_thread_ = std::thread([this]() { WriterLoop(); });
        report_thread_ = std::thread([this]() { ReportLoop(); });
    }
}

void RuntimeService::WriteFinalSummary(const IntegrityReport& integrity, const std::string& reason) const {
    const std::string integrity_str =
        integrity.result == IntegrityResult::kOk
            ? "ok"
            : (integrity.result == IntegrityResult::kWarning ? "warning" : "failed");
    const RuntimeMetrics m = diagnostics_.Snapshot(1.0);
    const double duration_sec = RecordingDurationSec();
    std::ostringstream json;
    json << "{\n";
    json << "  \"stop_reason\": \"" << JsonEscape(reason) << "\",\n";
    json << "  \"duration_sec\": " << duration_sec << ",\n";
    json << "  \"packets_received\": " << m.packets_received << ",\n";
    json << "  \"packets_written\": " << m.packets_written << ",\n";
    json << "  \"bad_packets\": " << m.bad_packets << ",\n";
    json << "  \"packet_loss\": " << m.packets_lost << ",\n";
    json << "  \"integrity\": \"" << integrity_str << "\",\n";
    json << "  \"integrity_detail\": \"" << JsonEscape(integrity.detail) << "\"\n";
    json << "}\n";
    WriteTextAtomic(sessions_.SessionDir() + "/final_runtime_summary.json", json.str());
}

RuntimeStatus RuntimeService::Status() const {
    RuntimeStatus st{};
    st.session_state = sessions_.State();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - rate_start_).count();
    st.metrics = diagnostics_.Snapshot(elapsed > 0.0 ? elapsed : 1.0);
    if (const auto active = sessions_.ActiveSession()) {
        st.session_id = active->session_id;
        st.session_dir = active->storage_path;
    }
    st.board_session_id = board_session_id_;
    return st;
}

std::string RuntimeService::HandleControlCommand(const std::string& command_line) {
    if (command_line == "PING") {
        return "OK\n\n";
    }
    if (command_line == "STATUS" || command_line == "DIAGNOSTICS" || command_line == "STATS") {
        return BuildDiagnosticsText() + "\n";
    }
    if (command_line.rfind("STOP", 0) == 0) {
        std::string reason = "ipc_stop";
        if (command_line.size() > 5U) {
            reason = command_line.substr(5U);
            if (!reason.empty() && reason[0] == ' ') {
                reason = reason.substr(1U);
            }
        }
        const auto rc = StopRecording(reason);
        if (rc == RuntimeErrorCode::kOk) {
            return "OK stopped\n\n";
        }
        return "ERR not_recording\n\n";
    }
    if (command_line.rfind("START", 0) == 0) {
        const auto rc = StartRecording(DefaultScenario());
        if (rc == RuntimeErrorCode::kOk) {
            return "OK recording\n\n";
        }
        if (rc == RuntimeErrorCode::kSessionBusy) {
            return "ERR session_busy\n\n";
        }
        return "ERR start_failed\n\n";
    }
    return "ERR unknown_command\n\n";
}

std::string RuntimeService::BuildRuntimeReportJson() const {
    const RuntimeMetrics m = diagnostics_.Snapshot(1.0);
    std::ostringstream json;
    json << "{\n  \"health\": \"" << JsonEscape(m.health) << "\"\n}\n";
    return json.str();
}

std::string RuntimeService::BuildDiagnosticsText() const {
    const RuntimeStatus st = Status();
    std::ostringstream out;
    out << "session_state=" << SessionStateToString(st.session_state) << "\n";
    out << "session_id=" << st.session_id << "\n";
    out << "board_session_id=" << st.board_session_id << "\n";
    out << "ego_host=" << config_.ego_host << "\n";
    out << "board_control_port=" << config_.board_control_port << "\n";
    out << "data_port=" << config_.data_port << "\n";
    out << "packets_received=" << st.metrics.packets_received << "\n";
    out << "packets_written=" << st.metrics.packets_written << "\n";
    out << "bad_packets=" << st.metrics.bad_packets << "\n";
    out << "packets_lost=" << st.metrics.packets_lost << "\n";
    out << "seq_gaps=" << st.metrics.seq_gaps << "\n";
    out << "disk_free_gb=" << st.metrics.disk_free_gb << "\n";
    out << "health=" << st.metrics.health << "\n";
    for (const auto& entry : st.metrics.reject_by_reason) {
        out << "reject_" << entry.first << "=" << entry.second << "\n";
    }
    return out.str();
}

}  // namespace ego_runtime
