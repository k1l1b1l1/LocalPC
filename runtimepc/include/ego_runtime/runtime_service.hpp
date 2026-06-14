#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ego_runtime/chunk_writer.hpp"
#include "ego_runtime/config.hpp"
#include "ego_runtime/contract_control_client.hpp"
#include "ego_runtime/control_ipc.hpp"
#include "ego_runtime/contract_tcp_client.hpp"
#include "ego_runtime/diagnostics.hpp"
#include "ego_runtime/ego_raw_writer.hpp"
#include "ego_runtime/error_log.hpp"
#include "ego_runtime/nav_provider.hpp"
#include "ego_runtime/nav_sidecar_writer.hpp"
#include "ego_runtime/packet_buffer.hpp"
#include "ego_runtime/session_checkpoint.hpp"
#include "ego_runtime/session_integrity.hpp"
#include "ego_runtime/session_manager.hpp"
#include "ego_runtime/storage_monitor.hpp"

namespace ego_runtime {

enum class DataLinkState {
    kUp,
    kDown,
    kReconnecting,
};

inline const char* DataLinkStateToString(DataLinkState state) {
    switch (state) {
        case DataLinkState::kUp:
            return "up";
        case DataLinkState::kDown:
            return "down";
        case DataLinkState::kReconnecting:
            return "reconnecting";
        default:
            return "unknown";
    }
}

struct RuntimeStatus {
    SessionState session_state = SessionState::kIdle;
    RuntimeMetrics metrics{};
    std::string session_id;
    std::string board_session_id;
    std::string session_dir;
    DataLinkState data_link = DataLinkState::kUp;
    std::uint64_t last_seq = 0U;
};

class RuntimeService {
public:
    explicit RuntimeService(RuntimeConfig config);
    ~RuntimeService();

    RuntimeErrorCode StartRecording(const ScenarioMetadata& scenario);
    RuntimeErrorCode StopRecording(const std::string& reason = "user_stop");
    RuntimeErrorCode EmergencyStop(const std::string& reason);
    RuntimeErrorCode ResumeRecording(const std::string& session_dir = "");
    RuntimeErrorCode ReconnectDataLinkCommand();

    bool StartDaemon();
    void StopDaemon();
    bool StartControlServer();
    void ProcessFileInput(const std::string& path);

    RuntimeStatus Status() const;
    std::string HandleControlCommand(const std::string& command_line);
    std::string BuildRuntimeReportJson() const;
    std::string BuildDiagnosticsText() const;

private:
    RuntimeErrorCode StartBoardSession(const ScenarioMetadata& scenario);
    RuntimeErrorCode StartLocalRecording(const ScenarioMetadata& scenario);
    RuntimeErrorCode StopBoardSession(const std::string& reason);
    RuntimeErrorCode ResumeBoardSession(const std::string& session_dir);
    bool WaitForDataFrame(std::uint32_t frame_type, std::chrono::milliseconds timeout);
    void ResetFrameWaitFlags();
    bool EnsureDataClient();
    bool ReconnectDataLink();

    void OnContractFrame(ContractFrame frame);
    void WriterLoop();
    void ReportLoop();
    void DataLinkWatchdogLoop();
    void FinalizeActiveSession(const std::string& reason);
    void DrainPacketBuffer();
    void NavSidecarLoop();
    void OpenNavSidecarWriter(const std::string& session_dir);
    void CloseNavSidecarWriter();
    void WriteRuntimeReport() const;
    void WriteFinalSummary(const IntegrityReport& integrity, const std::string& reason) const;
    void TrackContractSeq(std::uint64_t seq);
    void SetDataLinkState(DataLinkState state);
    void MaybeWriteCheckpoint();
    void UpdateDataLinkDownMetric();
    ScenarioMetadata DefaultScenario() const;
    double RecordingDurationSec() const;

    RuntimeConfig config_;
    SessionManager sessions_;
    std::unique_ptr<ErrorLog> error_log_;
    std::unique_ptr<NavProvider> nav_provider_;
    std::unique_ptr<NavSidecarWriter> nav_sidecar_writer_;
    std::unique_ptr<PacketBuffer> buffer_;
    std::unique_ptr<ChunkWriter> writer_;
    std::unique_ptr<ContractTcpClient> contract_client_;
    std::unique_ptr<ContractControlClient> control_client_;
    std::unique_ptr<StorageMonitor> storage_;
    std::unique_ptr<ControlServer> control_server_;
    DiagnosticsCollector diagnostics_;

    std::thread writer_thread_{};
    std::thread report_thread_{};
    std::thread reconnect_thread_{};
    std::thread nav_sidecar_thread_{};
    std::atomic<bool> daemon_running_{false};
    std::atomic<bool> stop_threads_{false};
    std::atomic<bool> reconnect_watchdog_stop_{false};
    std::atomic<bool> nav_sidecar_stop_{false};
    bool sync_file_mode_ = false;
    bool cold_data_session_ = true;
    std::chrono::steady_clock::time_point rate_start_{};
    std::chrono::steady_clock::time_point recording_start_{};
    mutable std::recursive_mutex service_mu_{};
    std::atomic<bool> finalize_in_progress_{false};
    std::uint64_t last_ts_ns_ = 0U;
    bool seen_ts_ = false;
    std::atomic<std::uint64_t> latest_contract_ts_ns_{0U};
    std::atomic<bool> latest_contract_ts_valid_{false};
    bool seq_initialized_ = false;
    std::uint64_t last_seq_ = 0U;
    std::uint64_t packets_since_checkpoint_ = 0U;
    std::string board_session_id_;
    std::atomic<DataLinkState> data_link_state_{DataLinkState::kUp};
    std::chrono::steady_clock::time_point data_link_down_since_{};
    bool data_link_down_since_valid_ = false;
    std::mutex frame_wait_mu_{};
    std::condition_variable frame_wait_cv_{};
    bool session_started_seen_ = false;
    bool config_snapshot_seen_ = false;
    bool session_ended_seen_ = false;
    std::string nav_sidecar_path_;
    std::uint64_t nav_samples_written_ = 0U;
};

}  // namespace ego_runtime
