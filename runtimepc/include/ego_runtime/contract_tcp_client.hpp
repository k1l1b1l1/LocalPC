#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ego_runtime/config.hpp"

namespace ego_runtime {

struct ContractFrame {
    std::vector<std::uint8_t> bytes;
};

class ContractTcpClient {
public:
    using Handler = std::function<void(ContractFrame)>;

    ContractTcpClient(RuntimeConfig config, Handler handler);
    ~ContractTcpClient();

    ContractTcpClient(const ContractTcpClient&) = delete;
    ContractTcpClient& operator=(const ContractTcpClient&) = delete;

    bool Start();
    void Stop();
    bool Running() const { return running_.load(); }
    const std::string& LastError() const { return last_error_; }
    std::uint64_t FramesReceived() const { return frames_received_.load(); }

private:
    void ReceiveLoop();
    bool ReadExact(std::uint8_t* dst, std::size_t size);

    RuntimeConfig config_;
    Handler handler_;
    std::thread thread_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> frames_received_{0};
    std::string last_error_;
#if defined(_WIN32)
    using SocketHandle = std::uintptr_t;
#else
    using SocketHandle = int;
#endif
    SocketHandle socket_fd_ = static_cast<SocketHandle>(-1);
};

}  // namespace ego_runtime
