#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ego_runtime/config.hpp"
#include "ego_runtime/session_manager.hpp"

namespace ego_runtime {

struct ControlHelloResult {
    bool ok = false;
    std::string error;
};

struct ControlStartSessionResult {
    bool ok = false;
    std::string session_id;
    std::string error;
    std::vector<std::string> missing_configs;
    std::vector<std::string> invalid_configs;
};

struct ControlStopSessionResult {
    bool ok = false;
    std::string session_id;
    std::string error;
};

class ContractControlClient {
public:
    explicit ContractControlClient(RuntimeConfig config);
    ~ContractControlClient();

    ContractControlClient(const ContractControlClient&) = delete;
    ContractControlClient& operator=(const ContractControlClient&) = delete;

    bool Connect();
    void Disconnect();
    bool IsConnected() const { return connected_; }

    ControlHelloResult Hello(const std::string& client_name, std::uint32_t protocol_version = 2U);
    ControlStartSessionResult StartSession(const ScenarioMetadata& scenario, const std::string& session_id);
    ControlStopSessionResult StopSession(const std::string& session_id, const std::string& reason);

    const std::string& LastError() const { return last_error_; }

private:
    bool ReadExact(std::uint8_t* dst, std::size_t size);
    bool WriteAll(const std::uint8_t* src, std::size_t size);
    bool SendControlPayload(const std::vector<std::uint8_t>& payload);
    bool RecvControlPayload(std::vector<std::uint8_t>& payload);

    RuntimeConfig config_;
    bool connected_ = false;
    std::uint64_t seq_ = 0U;
    std::uint64_t request_id_ = 1U;
    std::string last_error_;
#if defined(_WIN32)
    using SocketHandle = std::uintptr_t;
#else
    using SocketHandle = int;
#endif
    SocketHandle socket_fd_ = static_cast<SocketHandle>(-1);
};

}  // namespace ego_runtime
