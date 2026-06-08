#include <iostream>
#include <string>

#include "ego_runtime/config.hpp"
#include "ego_runtime/control_ipc.hpp"
#include "ego_runtime/runtime_service.hpp"
#include "ego_runtime/shutdown.hpp"
#include "ego_runtime/version.hpp"

namespace {

int ExitFromError(ego_runtime::RuntimeErrorCode code) {
    switch (code) {
        case ego_runtime::RuntimeErrorCode::kOk:
            return 0;
        case ego_runtime::RuntimeErrorCode::kSessionBusy:
            return 1;
        case ego_runtime::RuntimeErrorCode::kNotRecording:
            return 2;
        case ego_runtime::RuntimeErrorCode::kStorageCritical:
            return 3;
        case ego_runtime::RuntimeErrorCode::kConfigError:
            return 4;
        default:
            return 5;
    }
}

void PrintUsage() {
    std::cout << "ego-runtime run|start|stop|status|stats|diagnostics [options]\n\n";
    std::cout << "Commands:\n";
    std::cout << "  run         Long-lived daemon (systemd); waits for SIGTERM/SIGINT\n";
    std::cout << "  start       Start recording in running daemon (IPC)\n";
    std::cout << "  resume      Resume interrupted session (IPC)\n";
    std::cout << "  reconnect   Reconnect Data TCP to board (IPC)\n";
    std::cout << "  stop        Stop recording in running daemon (IPC)\n";
    std::cout << "  status      Session status (IPC if daemon running)\n";
    std::cout << "  diagnostics Detailed metrics (IPC if daemon running)\n\n";
    std::cout << "Options:\n";
    std::cout << "  --config path       Required unless /etc/ego-runtime/config.yaml exists\n";
    std::cout << "  --data-root path\n";
    std::cout << "  --input file.bin   Dev file ingest (in-process, not via IPC)\n";
    std::cout << "  --version\n\n";
    std::cout << "Exit codes: 0=ok 1=session_busy 2=not_recording 3=storage 4=config 5=internal\n";
}

ego_runtime::ScenarioMetadata ScenarioFromConfig(const ego_runtime::RuntimeConfig& config) {
    ego_runtime::ScenarioMetadata scenario{};
    scenario.scenario_id = config.scenario_id.empty() ? "default" : config.scenario_id;
    scenario.scenario_name = config.scenario_name.empty() ? "session" : config.scenario_name;
    scenario.operator_name = config.operator_name.empty() ? "operator" : config.operator_name;
    scenario.notes = config.notes;
    return scenario;
}

bool RunDaemon(ego_runtime::RuntimeService& service, ego_runtime::RuntimeConfig& config) {
    ego_runtime::InstallShutdownHandlers();

    if (!service.StartDaemon()) {
        std::cerr << "failed to start network receiver\n";
        return false;
    }
    if (!service.StartControlServer()) {
        std::cerr << "failed to start control IPC server\n";
        service.StopDaemon();
        return false;
    }

    if (!config.input_file.empty()) {
        const auto scenario = ScenarioFromConfig(config);
        const auto rc = service.StartRecording(scenario);
        if (rc != ego_runtime::RuntimeErrorCode::kOk) {
            std::cerr << "start recording failed\n";
            service.StopDaemon();
            return false;
        }
        service.ProcessFileInput(config.input_file);
        service.StopRecording("file_eof");
        service.StopDaemon();
        return true;
    }

    std::cout << "ego-runtime daemon running (SIGTERM/SIGINT to stop)\n";
    if (config.auto_resume_on_run) {
        const auto resume_rc = service.ResumeRecording("");
        if (resume_rc == ego_runtime::RuntimeErrorCode::kOk) {
            std::cout << "auto-resume: active session restored\n";
        }
    }
    ego_runtime::WaitForShutdown();
    service.StopRecording(ego_runtime::ShutdownReason());
    service.StopDaemon();
    return true;
}

int RunIpcClient(const ego_runtime::RuntimeConfig& config, const std::string& command) {
    if (!ego_runtime::IsDaemonRunning(config)) {
        std::cerr << "ego-runtime daemon is not running\n";
        return ExitFromError(ego_runtime::RuntimeErrorCode::kNotRecording);
    }
    ego_runtime::ControlClient client(config);
    std::string ipc_cmd;
    if (command == "stop") {
        ipc_cmd = "STOP cli_stop";
    } else if (command == "start") {
        ipc_cmd = "START";
    } else if (command == "resume") {
        ipc_cmd = "RESUME";
    } else if (command == "reconnect") {
        ipc_cmd = "RECONNECT";
    } else if (command == "status") {
        ipc_cmd = "STATUS";
    } else {
        ipc_cmd = "DIAGNOSTICS";
    }
    const std::string response = client.SendCommand(ipc_cmd);
    if (response.empty()) {
        std::cerr << "IPC request failed\n";
        return ExitFromError(ego_runtime::RuntimeErrorCode::kInternalError);
    }
    std::cout << response;
    if (response.rfind("ERR", 0) == 0) {
        return ExitFromError(ego_runtime::RuntimeErrorCode::kNotRecording);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--version") {
            std::cout << ego_runtime::VersionString() << "\n";
            return 0;
        }
        if (std::string(argv[i]) == "--help" || std::string(argv[i]) == "-h") {
            PrintUsage();
            return 0;
        }
    }

    ego_runtime::RuntimeConfig config = ego_runtime::LoadConfig(argc, argv);
    if (!config.config_file_loaded) {
        std::cerr << "Configuration required: install /etc/ego-runtime/config.yaml "
                     "or pass --config <path> (see config/board.yaml)\n";
        return ExitFromError(ego_runtime::RuntimeErrorCode::kConfigError);
    }

    std::string command;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) == 0) {
            if (arg == "--config" || arg == "--data-root" || arg == "--input" || arg == "--scenario-id" ||
                arg == "--scenario-name" || arg == "--operator" || arg == "--notes") {
                ++i;
            }
            continue;
        }
        command = arg;
        break;
    }

    if (command.empty() || command == "run") {
        ego_runtime::RuntimeService service(config);
        return RunDaemon(service, config) ? 0 : ExitFromError(ego_runtime::RuntimeErrorCode::kInternalError);
    }

    if (command == "start" || command == "stop" || command == "resume" || command == "reconnect" ||
        command == "status" || command == "stats" || command == "diagnostics") {
        if (!config.input_file.empty()) {
            ego_runtime::RuntimeService service(config);
            if (!service.StartDaemon()) {
                return ExitFromError(ego_runtime::RuntimeErrorCode::kInternalError);
            }
            const auto rc = service.StartRecording(ScenarioFromConfig(config));
            if (rc != ego_runtime::RuntimeErrorCode::kOk) {
                service.StopDaemon();
                return ExitFromError(rc);
            }
            service.ProcessFileInput(config.input_file);
            const auto stop_rc = service.StopRecording("file_eof");
            service.StopDaemon();
            return ExitFromError(stop_rc);
        }
        return RunIpcClient(config, command);
    }

    PrintUsage();
    return ExitFromError(ego_runtime::RuntimeErrorCode::kConfigError);
}
