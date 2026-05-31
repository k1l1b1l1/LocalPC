#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "ego_runtime/config.hpp"

namespace ego_runtime {

struct Datagram {
    std::vector<std::uint8_t> data;
    std::string source_ip;
};

class NetworkReceiver {
public:
    using Handler = std::function<void(Datagram)>;

    NetworkReceiver(RuntimeConfig config, Handler handler);
    ~NetworkReceiver();

    NetworkReceiver(const NetworkReceiver&) = delete;
    NetworkReceiver& operator=(const NetworkReceiver&) = delete;

    bool Start();
    void Stop();
    bool Running() const { return running_.load(); }
    const std::string& LastError() const { return last_error_; }
    std::uint64_t DatagramsReceived() const { return datagrams_received_.load(); }
    std::uint64_t DatagramsRejected() const { return datagrams_rejected_.load(); }

private:
    void ReceiveLoop();
    bool IsSourceAllowed(const std::string& ip) const;

    RuntimeConfig config_;
    Handler handler_;
    std::thread thread_{};
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<std::uint64_t> datagrams_received_{0};
    std::atomic<std::uint64_t> datagrams_rejected_{0};
    std::string last_error_;
#if defined(_WIN32)
    using SocketHandle = std::uintptr_t;
#else
    using SocketHandle = int;
#endif
    SocketHandle socket_fd_ = static_cast<SocketHandle>(-1);
};

}  // namespace ego_runtime
