#include "ego_runtime/control_ipc.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace ego_runtime {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNativeSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidNativeSocket = -1;
#endif

std::string PidFilePath(const RuntimeConfig& config) {
    return (std::filesystem::path(config.data_root) / "ego-runtime.pid").string();
}

}  // namespace

std::string ControlEndpointPath(const RuntimeConfig& config) {
#if defined(_WIN32)
    return "\\\\.\\pipe\\" + config.control_pipe_name;
#else
    return (std::filesystem::path(config.data_root) / "ego-runtime.sock").string();
#endif
}

void WritePidFile(const RuntimeConfig& config) {
    std::error_code ec;
    std::filesystem::create_directories(config.data_root, ec);
    std::ofstream out(PidFilePath(config), std::ios::trunc);
#if defined(_WIN32)
    out << _getpid();
#else
    out << getpid();
#endif
}

void RemovePidFile(const RuntimeConfig& config) {
    std::error_code ec;
    std::filesystem::remove(PidFilePath(config), ec);
}

bool IsDaemonRunning(const RuntimeConfig& config) {
    const std::string path = PidFilePath(config);
    if (!std::filesystem::exists(path)) {
        return false;
    }
    ControlClient client(config);
    return client.DaemonReachable();
}

ControlServer::ControlServer(RuntimeConfig config, Handler handler)
    : config_(std::move(config)), handler_(std::move(handler)) {}

ControlServer::~ControlServer() { Stop(); }

bool ControlServer::Start() {
    if (running_.load()) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(config_.data_root, ec);

#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    listen_fd_ = static_cast<SocketHandle>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listen_fd_ == kInvalidNativeSocket) {
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(config_.control_port);
    int reuse = 1;
    setsockopt(static_cast<NativeSocket>(listen_fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    if (bind(static_cast<NativeSocket>(listen_fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return false;
    }
    if (listen(static_cast<NativeSocket>(listen_fd_), 4) != 0) {
        return false;
    }
#else
    listen_fd_ = static_cast<SocketHandle>(socket(AF_UNIX, SOCK_STREAM, 0));
    if (listen_fd_ == kInvalidNativeSocket) {
        return false;
    }
    const std::string path = ControlEndpointPath(config_);
    std::filesystem::remove(path, ec);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
        return false;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1U);
    if (bind(static_cast<NativeSocket>(listen_fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return false;
    }
    if (listen(static_cast<NativeSocket>(listen_fd_), 8) != 0) {
        return false;
    }
#endif

    stop_ = false;
    running_ = true;
    thread_ = std::thread([this]() { ServeLoop(); });
    return true;
}

void ControlServer::Stop() {
    stop_ = true;
#if !defined(_WIN32)
    if (listen_fd_ != static_cast<SocketHandle>(kInvalidNativeSocket)) {
        ControlClient(config_).SendCommand("PING");
    }
#endif
    if (thread_.joinable()) {
        thread_.join();
    }
    if (listen_fd_ != static_cast<SocketHandle>(kInvalidNativeSocket)) {
#if defined(_WIN32)
        closesocket(static_cast<NativeSocket>(listen_fd_));
        WSACleanup();
#else
        close(static_cast<NativeSocket>(listen_fd_));
        std::filesystem::remove(ControlEndpointPath(config_));
#endif
        listen_fd_ = static_cast<SocketHandle>(kInvalidNativeSocket);
    }
    running_ = false;
}

void ControlServer::ServeLoop() {
    while (!stop_.load()) {
#if defined(_WIN32)
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(static_cast<NativeSocket>(listen_fd_), &fds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
#else
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(static_cast<NativeSocket>(listen_fd_), &fds);
        timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        if (select(static_cast<NativeSocket>(listen_fd_) + 1, &fds, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
#endif
        const NativeSocket client = accept(static_cast<NativeSocket>(listen_fd_), nullptr, nullptr);
        if (client == kInvalidNativeSocket) {
            continue;
        }
        const std::string request = ReadRequest(static_cast<int>(client));
        const std::string response = handler_ ? handler_(request) : "ERR no handler\n";
        WriteResponse(static_cast<int>(client), response);
#if defined(_WIN32)
        closesocket(client);
#else
        close(client);
#endif
    }
}

std::string ControlServer::ReadRequest(int client_fd) const {
    char buffer[512] = {};
    const int n = recv(client_fd, buffer, sizeof(buffer) - 1U, 0);
    if (n <= 0) {
        return {};
    }
    std::string line(buffer, static_cast<std::size_t>(n));
    const auto pos = line.find('\n');
    if (pos != std::string::npos) {
        line.resize(pos);
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.pop_back();
    }
    return line;
}

void ControlServer::WriteResponse(int client_fd, const std::string& body) const {
    send(client_fd, body.data(), static_cast<int>(body.size()), 0);
}

ControlClient::ControlClient(RuntimeConfig config) : config_(std::move(config)) {}

std::string ControlClient::SendCommand(const std::string& command_line) const {
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    NativeSocket fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kInvalidNativeSocket) {
        return {};
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(config_.control_port);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(fd);
        return {};
    }
#else
    NativeSocket fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == kInvalidNativeSocket) {
        return {};
    }
    const std::string path = ControlEndpointPath(config_);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1U);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return {};
    }
#endif
    std::string line = command_line;
    line.push_back('\n');
    send(fd, line.data(), static_cast<int>(line.size()), 0);

    std::string response;
    char buffer[4096] = {};
    for (;;) {
        const int n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0) {
            break;
        }
        response.append(buffer, static_cast<std::size_t>(n));
        if (response.find("\n\n") != std::string::npos) {
            break;
        }
    }
#if defined(_WIN32)
    closesocket(fd);
#else
    close(fd);
#endif
    return response;
}

bool ControlClient::DaemonReachable() const {
    const std::string response = SendCommand("PING");
    return response.find("OK") != std::string::npos;
}

}  // namespace ego_runtime
