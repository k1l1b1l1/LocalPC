#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "ego_runtime/chunk_writer.hpp"
#include "ego_runtime/config.hpp"
#include "ego_runtime/control_ipc.hpp"
#include "ego_runtime/contract_tcp_client.hpp"
#include "ego_runtime/diagnostics.hpp"
#include "ego_runtime/ego_raw_writer.hpp"
#include "ego_runtime/error_log.hpp"
#include "ego_runtime/packet_buffer.hpp"
#include "ego_runtime/session_integrity.hpp"
#include "ego_runtime/session_manager.hpp"
#include "ego_runtime/storage_monitor.hpp"

namespace ego_runtime {

struct RuntimeStatus {
    SessionState session_state = SessionState::kIdle;
    RuntimeMetrics metrics{};
    std::string session_id;
    std::string session_dir;
};

class RuntimeService {
public:
    explicit RuntimeService(RuntimeConfig config);
    ~RuntimeService();

    RuntimeErrorCode StartRecording(const ScenarioMetadata& scenario);
    RuntimeErrorCode StopRecording(const std::string& reason = "user_stop");
    RuntimeErrorCode EmergencyStop(const std::string& reason);

    bool StartDaemon();
    void StopDaemon();
    bool StartControlServer();
    void ProcessFileInput(const std::string& path);

    RuntimeStatus Status() const;
    std::string HandleControlCommand(const std::string& command_line);
    std::string BuildRuntimeReportJson() const;
    std::string BuildDiagnosticsText() const;

private:
    void OnContractFrame(ContractFrame frame);
    void WriterLoop();
    void ReportLoop();
    void FinalizeActiveSession(const std::string& reason);
    void DrainPacketBuffer();
    void WriteRuntimeReport() const;
    void WriteFinalSummary(const IntegrityReport& integrity, const std::string& reason) const;
    void TrackContractSeq(std::uint64_t seq);
    ScenarioMetadata DefaultScenario() const;
    double RecordingDurationSec() const;

    RuntimeConfig config_;
    SessionManager sessions_;
    std::unique_ptr<ErrorLog> error_log_;
    std::unique_ptr<PacketBuffer> buffer_;
    std::unique_ptr<ChunkWriter> writer_;
    std::unique_ptr<ContractTcpClient> contract_client_;
    std::unique_ptr<StorageMonitor> storage_;
    std::unique_ptr<ControlServer> control_server_;
    DiagnosticsCollector diagnostics_;

    std::thread writer_thread_{};
    std::thread report_thread_{};
    std::atomic<bool> daemon_running_{false};
    std::atomic<bool> stop_threads_{false};
    bool sync_file_mode_ = false;
    std::chrono::steady_clock::time_point rate_start_{};
    std::chrono::steady_clock::time_point recording_start_{};
    mutable std::recursive_mutex service_mu_{};
    std::atomic<bool> finalize_in_progress_{false};
    std::uint64_t last_ts_ns_ = 0U;
    bool seen_ts_ = false;
    bool seq_initialized_ = false;
    std::uint64_t last_seq_ = 0U;
};

}  // namespace ego_runtime
