#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>

#include "ego_runtime/config.hpp"

namespace ego_runtime {

std::string ControlEndpointPath(const RuntimeConfig& config);

class ControlServer {
public:
    using Handler = std::function<std::string(const std::string& command_line)>;

    ControlServer(RuntimeConfig config, Handler handler);
    ~ControlServer();

    bool Start();
    void Stop();

private:
    void ServeLoop();
    std::string ReadRequest(int client_fd) const;
    void WriteResponse(int client_fd, const std::string& body) const;

    RuntimeConfig config_;
    Handler handler_;
    std::thread thread_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
#if defined(_WIN32)
    using SocketHandle = std::uintptr_t;
#else
    using SocketHandle = int;
#endif
    SocketHandle listen_fd_ = static_cast<SocketHandle>(-1);
};

class ControlClient {
public:
    explicit ControlClient(RuntimeConfig config);

    std::string SendCommand(const std::string& command_line) const;
    bool DaemonReachable() const;

private:
    RuntimeConfig config_;
};

void WritePidFile(const RuntimeConfig& config);
void RemovePidFile(const RuntimeConfig& config);
bool IsDaemonRunning(const RuntimeConfig& config);

}  // namespace ego_runtime
