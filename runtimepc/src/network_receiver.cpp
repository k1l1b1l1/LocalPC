#include "ego_runtime/network_receiver.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <thread>

namespace ego_runtime {
namespace {

#if defined(_WIN32)
constexpr SOCKET kInvalidSocket = INVALID_SOCKET;
#else
constexpr int kInvalidSocket = -1;
#endif

}  // namespace

NetworkReceiver::NetworkReceiver(RuntimeConfig config, Handler handler)
    : config_(std::move(config)), handler_(std::move(handler)) {}

NetworkReceiver::~NetworkReceiver() { Stop(); }

bool NetworkReceiver::IsSourceAllowed(const std::string& ip) const {
    if (config_.allow_all_sources) {
        return true;
    }
    if (config_.allowed_sources.empty()) {
        return false;
    }
    return std::find(config_.allowed_sources.begin(), config_.allowed_sources.end(), ip) !=
           config_.allowed_sources.end();
}

bool NetworkReceiver::Start() {
    if (running_.load()) {
        return true;
    }
#if defined(_WIN32)
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        last_error_ = "WSAStartup failed";
        return false;
    }
#endif
    socket_fd_ = static_cast<SocketHandle>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
#if defined(_WIN32)
    if (socket_fd_ == INVALID_SOCKET) {
#else
    if (socket_fd_ == kInvalidSocket) {
#endif
        last_error_ = "socket() failed";
        return false;
    }

    int reuse = 1;
    setsockopt(static_cast<int>(socket_fd_), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    const int rcvbuf = static_cast<int>(config_.udp_rcvbuf_bytes);
    setsockopt(static_cast<int>(socket_fd_), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf),
               sizeof(rcvbuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.bind_port);
    if (inet_pton(AF_INET, config_.bind_host.c_str(), &addr.sin_addr) != 1) {
        last_error_ = "invalid bind_host";
        return false;
    }
    if (bind(static_cast<int>(socket_fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        last_error_ = "bind() failed";
        return false;
    }

#if !defined(_WIN32)
    const int flags = fcntl(static_cast<int>(socket_fd_), F_GETFL, 0);
    fcntl(static_cast<int>(socket_fd_), F_SETFL, flags | O_NONBLOCK);
#else
    u_long mode = 1;
    ioctlsocket(static_cast<SOCKET>(socket_fd_), FIONBIO, &mode);
#endif

    stop_requested_ = false;
    running_ = true;
    thread_ = std::thread([this]() { ReceiveLoop(); });
    return true;
}

void NetworkReceiver::Stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (socket_fd_ != static_cast<SocketHandle>(kInvalidSocket)) {
#if defined(_WIN32)
        closesocket(static_cast<SOCKET>(socket_fd_));
        WSACleanup();
#else
        close(static_cast<int>(socket_fd_));
#endif
        socket_fd_ = static_cast<SocketHandle>(kInvalidSocket);
    }
    running_ = false;
}

void NetworkReceiver::ReceiveLoop() {
    std::vector<std::uint8_t> buffer(65536U);

#if !defined(_WIN32)
    const int epfd = epoll_create1(0);
    if (epfd < 0) {
        last_error_ = "epoll_create1 failed";
        return;
    }
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = static_cast<int>(socket_fd_);
    epoll_ctl(epfd, EPOLL_CTL_ADD, static_cast<int>(socket_fd_), &ev);
    epoll_event events[16];

    while (!stop_requested_.load()) {
        const int n = epoll_wait(epfd, events, 16, 200);
        if (n <= 0) {
            continue;
        }
        for (int i = 0; i < n; ++i) {
            for (;;) {
                sockaddr_in peer{};
                socklen_t peer_len = sizeof(peer);
                const int received = recvfrom(static_cast<int>(socket_fd_),
                                              reinterpret_cast<char*>(buffer.data()),
                                              static_cast<int>(buffer.size()),
                                              0,
                                              reinterpret_cast<sockaddr*>(&peer),
                                              &peer_len);
                if (received <= 0) {
                    break;
                }
                char ip_str[INET_ADDRSTRLEN] = {};
                inet_ntop(AF_INET, &peer.sin_addr, ip_str, sizeof(ip_str));
                if (!IsSourceAllowed(ip_str)) {
                    ++datagrams_rejected_;
                    continue;
                }
                Datagram datagram{};
                datagram.data.assign(buffer.begin(), buffer.begin() + received);
                datagram.source_ip = ip_str;
                ++datagrams_received_;
                handler_(std::move(datagram));
            }
        }
    }
    close(epfd);
    return;
#endif

    while (!stop_requested_.load()) {
        sockaddr_in peer{};
        socklen_t peer_len = sizeof(peer);
        const int received = recvfrom(
            static_cast<int>(socket_fd_),
            reinterpret_cast<char*>(buffer.data()),
            static_cast<int>(buffer.size()),
            0,
            reinterpret_cast<sockaddr*>(&peer),
            &peer_len);
        if (received <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        char ip_str[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip_str, sizeof(ip_str));
        if (!IsSourceAllowed(ip_str)) {
            ++datagrams_rejected_;
            continue;
        }
        Datagram datagram{};
        datagram.data.assign(buffer.begin(), buffer.begin() + received);
        datagram.source_ip = ip_str;
        ++datagrams_received_;
        handler_(std::move(datagram));
    }
}

}  // namespace ego_runtime
