#include "ego_runtime/offline_trigger.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace ego_runtime {
namespace {

namespace fs = std::filesystem;

bool file_exists(const std::string& path) {
    return !path.empty() && fs::is_regular_file(path);
}

std::vector<std::string> build_argv(const RuntimeConfig& config, const std::string& session_dir) {
    const auto& hook = config.offline;
    std::vector<std::string> argv;
    argv.push_back(hook.binary);
    argv.push_back("process");
    argv.push_back("--session-dir");
    argv.push_back(session_dir);
    if (!hook.config_path.empty()) {
        argv.push_back("--config");
        argv.push_back(hook.config_path);
    }
    if (!hook.skip_s3 && file_exists(hook.s3_config_path)) {
        argv.push_back("--s3-config");
        argv.push_back(hook.s3_config_path);
    } else if (hook.skip_s3) {
        argv.push_back("--skip-s3");
    }
    return argv;
}

#ifdef _WIN32
OfflineTriggerResult spawn_detached(const std::vector<std::string>& argv) {
    OfflineTriggerResult result;
    std::ostringstream cmd;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) {
            cmd << ' ';
        }
        cmd << '"';
        for (char c : argv[i]) {
            if (c == '"') {
                cmd << "\\\"";
            } else {
                cmd << c;
            }
        }
        cmd << '"';
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string mutable_cmd = cmd.str();
    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        result.error = "CreateProcess failed";
        return result;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    result.started = true;
    result.pid = static_cast<int>(pi.dwProcessId);
    return result;
}
#else
OfflineTriggerResult spawn_detached(const std::vector<std::string>& argv) {
    OfflineTriggerResult result;
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1U);
    for (const auto& arg : argv) {
        cargv.push_back(const_cast<char*>(arg.c_str()));
    }
    cargv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        return result;
    }
    if (pid == 0) {
        setsid();
        execv(argv.front().c_str(), cargv.data());
        _exit(127);
    }
    result.started = true;
    result.pid = static_cast<int>(pid);
    return result;
}
#endif

void write_trigger_log(const std::string& session_dir, const OfflineTriggerResult& result,
                       const std::vector<std::string>& argv) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"started\": " << (result.started ? "true" : "false") << ",\n";
    json << "  \"pid\": " << result.pid << ",\n";
    json << "  \"error\": \"" << result.error << "\",\n";
    json << "  \"command\": \"";
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) {
            json << ' ';
        }
        json << argv[i];
    }
    json << "\"\n";
    json << "}\n";
    const fs::path log_path = fs::path(session_dir) / "logs" / "offline_trigger.json";
    std::error_code ec;
    fs::create_directories(log_path.parent_path(), ec);
    std::ofstream out(log_path);
    if (out) {
        out << json.str();
    }
}

}  // namespace

OfflineTriggerResult TriggerOfflinePipeline(const RuntimeConfig& config,
                                            const std::string& session_dir) {
    OfflineTriggerResult result;
    const auto& hook = config.offline;

    if (!hook.enabled) {
        result.error = "offline hook disabled";
        return result;
    }
    if (session_dir.empty()) {
        result.error = "empty session_dir";
        return result;
    }
    if (!file_exists(hook.binary)) {
        result.error = "ego-offline binary not found: " + hook.binary;
        return result;
    }
    if (!hook.config_path.empty() && !file_exists(hook.config_path)) {
        result.error = "offline config not found: " + hook.config_path;
        return result;
    }

    const auto argv = build_argv(config, session_dir);
    result = spawn_detached(argv);
    write_trigger_log(session_dir, result, argv);

    if (result.started) {
        std::cerr << "[ego-runtime] offline pipeline started pid=" << result.pid
                  << " session=" << session_dir << '\n';
    } else if (!result.error.empty()) {
        std::cerr << "[ego-runtime] offline pipeline failed: " << result.error << '\n';
    }
    return result;
}

}  // namespace ego_runtime
