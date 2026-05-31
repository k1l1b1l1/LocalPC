#include "ego_runtime/contract_tcp_client.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "ego_contract/crc32.hpp"
#include "ego_protocol_packets.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ego_runtime {
namespace {

using ego::protocol::v1::EgoFrameHeader;
using ego::protocol::v1::EGO_FRAME_MAGIC;

}  // namespace

ContractTcpClient::ContractTcpClient(RuntimeConfig config, Handler handler)
    : config_(std::move(config)), handler_(std::move(handler)) {}

ContractTcpClient::~ContractTcpClient() {
    Stop();
}

bool ContractTcpClient::ReadExact(std::uint8_t* dst, std::size_t size) {
#if defined(_WIN32)
    const SocketHandle fd = socket_fd_;
    std::size_t got = 0U;
    while (got < size) {
        const int r = recv(static_cast<SOCKET>(fd), reinterpret_cast<char*>(dst + got),
                           static_cast<int>(size - got), 0);
        if (r <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(r);
    }
    return true;
#else
    std::size_t got = 0U;
    while (got < size) {
        const ssize_t r = recv(static_cast<int>(socket_fd_), dst + got, size - got, 0);
        if (r <= 0) {
            return false;
        }
        got += static_cast<std::size_t>(r);
    }
    return true;
#endif
}

bool ContractTcpClient::Start() {
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
    const SocketHandle fd = static_cast<SocketHandle>(socket(
#if defined(_WIN32)
        AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd == INVALID_SOCKET) {
#else
        AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
#endif
        last_error_ = "socket create failed";
        return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.data_port);
    if (inet_pton(AF_INET, config_.ego_host.c_str(), &addr.sin_addr) != 1) {
        last_error_ = "invalid ego_host";
#if defined(_WIN32)
        closesocket(static_cast<SOCKET>(fd));
#else
        close(static_cast<int>(fd));
#endif
        return false;
    }
    bool connected = false;
    for (int attempt = 0; attempt < 200 && !stop_requested_.load(); ++attempt) {
        if (connect(
#if defined(_WIN32)
                static_cast<SOCKET>(fd),
#else
                static_cast<int>(fd),
#endif
                reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!connected) {
        last_error_ = "connect failed to " + config_.ego_host + ":" + std::to_string(config_.data_port);
#if defined(_WIN32)
        closesocket(static_cast<SOCKET>(fd));
#else
        close(static_cast<int>(fd));
#endif
        return false;
    }
    socket_fd_ = fd;
    stop_requested_ = false;
    running_ = true;
    thread_ = std::thread([this]() { ReceiveLoop(); });
    last_error_.clear();
    return true;
}

void ContractTcpClient::Stop() {
    stop_requested_ = true;
    if (thread_.joinable()) {
        thread_.join();
    }
    if (socket_fd_ != static_cast<SocketHandle>(-1)) {
#if defined(_WIN32)
        closesocket(static_cast<SOCKET>(socket_fd_));
        WSACleanup();
#else
        close(static_cast<int>(socket_fd_));
#endif
        socket_fd_ = static_cast<SocketHandle>(-1);
    }
    running_ = false;
}

void ContractTcpClient::ReceiveLoop() {
    while (!stop_requested_.load()) {
        EgoFrameHeader header{};
        if (!ReadExact(reinterpret_cast<std::uint8_t*>(&header), sizeof(header))) {
            if (!stop_requested_.load()) {
                last_error_ = "read header failed";
            }
            break;
        }
        if (header.magic != EGO_FRAME_MAGIC || header.header_size != sizeof(EgoFrameHeader)) {
            last_error_ = "bad frame header";
            break;
        }
        if (header.payload_size > config_.max_payload_bytes) {
            last_error_ = "payload too large";
            break;
        }
        std::vector<std::uint8_t> payload(header.payload_size);
        if (header.payload_size > 0U) {
            if (!ReadExact(payload.data(), payload.size())) {
                last_error_ = "read payload failed";
                break;
            }
        }
        if (ego_contract::Crc32(payload) != header.payload_crc32) {
            last_error_ = "payload crc mismatch";
            break;
        }
        EgoFrameHeader zero_hdr = header;
        zero_hdr.header_crc32 = 0U;
        if (ego_contract::Crc32(reinterpret_cast<const std::uint8_t*>(&zero_hdr), sizeof(zero_hdr)) !=
            header.header_crc32) {
            last_error_ = "header crc mismatch";
            break;
        }
        ContractFrame frame{};
        frame.bytes.resize(sizeof(header) + payload.size());
        std::memcpy(frame.bytes.data(), &header, sizeof(header));
        if (!payload.empty()) {
            std::memcpy(frame.bytes.data() + sizeof(header), payload.data(), payload.size());
        }
        handler_(std::move(frame));
        ++frames_received_;
    }
    running_ = false;
}

}  // namespace ego_runtime
