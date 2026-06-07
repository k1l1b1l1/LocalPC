#include "ego_runtime/contract_control_client.hpp"

#include <chrono>
#include <cstring>
#include <thread>

#include "ego/v1/ego_common.pb.h"
#include "ego/v1/ego_control.pb.h"
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

using ego::protocol::v1::EgoControlMessageHeader;
using ego::protocol::v1::EGO_CONTROL_MAGIC;

std::vector<std::uint8_t> PackControlMessage(const std::uint64_t seq, const std::vector<std::uint8_t>& payload) {
    EgoControlMessageHeader hdr{};
    hdr.magic = EGO_CONTROL_MAGIC;
    hdr.protocol_ver = ego::protocol::v1::EGO_PROTOCOL_VERSION;
    hdr.header_size = static_cast<std::uint16_t>(sizeof(EgoControlMessageHeader));
    hdr.seq = seq;
    hdr.payload_size = static_cast<std::uint32_t>(payload.size());
    hdr.payload_crc32 = ego_contract::Crc32(payload);
    hdr.header_crc32 = 0U;
    hdr.reserved0 = 0U;
    const std::uint32_t header_crc =
        ego_contract::Crc32(reinterpret_cast<const std::uint8_t*>(&hdr), sizeof(hdr));
    hdr.header_crc32 = header_crc;

    std::vector<std::uint8_t> out(sizeof(hdr) + payload.size());
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    if (!payload.empty()) {
        std::memcpy(out.data() + sizeof(hdr), payload.data(), payload.size());
    }
    return out;
}

bool UnpackControlMessage(const std::uint8_t* header_bytes,
                          const std::uint8_t* payload,
                          std::string* error) {
    EgoControlMessageHeader hdr{};
    std::memcpy(&hdr, header_bytes, sizeof(hdr));
    if (hdr.magic != EGO_CONTROL_MAGIC) {
        if (error) {
            *error = "bad control magic";
        }
        return false;
    }
    if (hdr.header_size != sizeof(EgoControlMessageHeader)) {
        if (error) {
            *error = "bad control header size";
        }
        return false;
    }
    if (ego_contract::Crc32(payload, hdr.payload_size) != hdr.payload_crc32) {
        if (error) {
            *error = "control payload crc mismatch";
        }
        return false;
    }
    EgoControlMessageHeader zero_hdr = hdr;
    zero_hdr.header_crc32 = 0U;
    if (ego_contract::Crc32(reinterpret_cast<const std::uint8_t*>(&zero_hdr), sizeof(zero_hdr)) !=
        hdr.header_crc32) {
        if (error) {
            *error = "control header crc mismatch";
        }
        return false;
    }
    return true;
}

void FillSessionMetadata(const ScenarioMetadata& scenario,
                         const std::string& session_id,
                         const RuntimeConfig& config,
                         ego::v1::SessionMetadata* meta) {
    meta->set_session_id(session_id);
    meta->set_scenario_id(scenario.scenario_id);
    meta->set_scenario_name(scenario.scenario_name);
    meta->set_operator_name(scenario.operator_name);
    meta->set_project("localpc");
    meta->set_vehicle_id(config.vehicle_id);
    meta->set_source_id("ego-runtime");
    if (!scenario.notes.empty()) {
        auto* tag = meta->add_tags();
        tag->set_key("notes");
        tag->set_value(scenario.notes);
    }
}

}  // namespace

ContractControlClient::ContractControlClient(RuntimeConfig config) : config_(std::move(config)) {}

ContractControlClient::~ContractControlClient() {
    Disconnect();
}

bool ContractControlClient::ReadExact(std::uint8_t* dst, const std::size_t size) {
#if defined(_WIN32)
    std::size_t got = 0U;
    while (got < size) {
        const int r = recv(static_cast<SOCKET>(socket_fd_), reinterpret_cast<char*>(dst + got),
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

bool ContractControlClient::WriteAll(const std::uint8_t* src, const std::size_t size) {
#if defined(_WIN32)
    std::size_t sent = 0U;
    while (sent < size) {
        const int r = send(static_cast<SOCKET>(socket_fd_), reinterpret_cast<const char*>(src + sent),
                           static_cast<int>(size - sent), 0);
        if (r <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(r);
    }
    return true;
#else
    std::size_t sent = 0U;
    while (sent < size) {
        const ssize_t r = send(static_cast<int>(socket_fd_), src + sent, size - sent, 0);
        if (r <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(r);
    }
    return true;
#endif
}

bool ContractControlClient::Connect() {
    if (connected_) {
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
    addr.sin_port = htons(config_.board_control_port);
    if (inet_pton(AF_INET, config_.ego_host.c_str(), &addr.sin_addr) != 1) {
        last_error_ = "invalid ego_host";
#if defined(_WIN32)
        closesocket(static_cast<SOCKET>(fd));
#else
        close(static_cast<int>(fd));
#endif
        return false;
    }
    bool ok = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (connect(
#if defined(_WIN32)
                static_cast<SOCKET>(fd),
#else
                static_cast<int>(fd),
#endif
                reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            ok = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ok) {
        last_error_ = "connect failed to " + config_.ego_host + ":" +
                      std::to_string(config_.board_control_port);
#if defined(_WIN32)
        closesocket(static_cast<SOCKET>(fd));
#else
        close(static_cast<int>(fd));
#endif
        return false;
    }
    socket_fd_ = fd;
    connected_ = true;
    seq_ = 0U;
    last_error_.clear();
    return true;
}

void ContractControlClient::Disconnect() {
    if (!connected_) {
        return;
    }
#if defined(_WIN32)
    closesocket(static_cast<SOCKET>(socket_fd_));
    WSACleanup();
#else
    close(static_cast<int>(socket_fd_));
#endif
    socket_fd_ = static_cast<SocketHandle>(-1);
    connected_ = false;
}

bool ContractControlClient::SendControlPayload(const std::vector<std::uint8_t>& payload) {
    ++seq_;
    const auto frame = PackControlMessage(seq_, payload);
    if (!WriteAll(frame.data(), frame.size())) {
        last_error_ = "control send failed";
        connected_ = false;
        return false;
    }
    return true;
}

bool ContractControlClient::RecvControlPayload(std::vector<std::uint8_t>& payload) {
    EgoControlMessageHeader hdr{};
    if (!ReadExact(reinterpret_cast<std::uint8_t*>(&hdr), sizeof(hdr))) {
        last_error_ = "control recv header failed";
        connected_ = false;
        return false;
    }
    payload.resize(hdr.payload_size);
    if (hdr.payload_size > 0U) {
        if (!ReadExact(payload.data(), payload.size())) {
            last_error_ = "control recv payload failed";
            connected_ = false;
            return false;
        }
    }
    std::string unpack_error;
    if (!UnpackControlMessage(reinterpret_cast<const std::uint8_t*>(&hdr), payload.data(), &unpack_error)) {
        last_error_ = unpack_error;
        connected_ = false;
        return false;
    }
    return true;
}

ControlHelloResult ContractControlClient::Hello(const std::string& client_name,
                                                const std::uint32_t protocol_version) {
    ControlHelloResult result{};
    if (!connected_ && !Connect()) {
        result.error = last_error_;
        return result;
    }

    ego::v1::ControlRequest req;
    req.set_request_id(request_id_++);
    req.mutable_hello()->set_client_name(client_name);
    req.mutable_hello()->set_client_version("ego-runtime");
    req.mutable_hello()->set_protocol_version(protocol_version);

    const std::string req_bytes = req.SerializeAsString();
    if (!SendControlPayload(std::vector<std::uint8_t>(req_bytes.begin(), req_bytes.end()))) {
        result.error = last_error_;
        return result;
    }

    std::vector<std::uint8_t> resp_bytes;
    if (!RecvControlPayload(resp_bytes)) {
        result.error = last_error_;
        return result;
    }

    ego::v1::ControlResponse resp;
    if (!resp.ParseFromArray(resp_bytes.data(), static_cast<int>(resp_bytes.size()))) {
        result.error = "control response parse failed";
        return result;
    }
    if (!resp.has_hello()) {
        result.error = "unexpected control response type";
        return result;
    }
    const auto& hello = resp.hello();
    if (hello.result() != ego::v1::RESULT_CODE_OK) {
        result.error = hello.error().message().empty() ? "hello rejected" : hello.error().message();
        return result;
    }
    result.ok = true;
    return result;
}

ControlStartSessionResult ContractControlClient::StartSession(const ScenarioMetadata& scenario,
                                                              const std::string& session_id) {
    ControlStartSessionResult result{};
    if (!connected_ && !Connect()) {
        result.error = last_error_;
        return result;
    }

    ego::v1::ControlRequest req;
    req.set_request_id(request_id_++);
    auto* start = req.mutable_start_session();
    FillSessionMetadata(scenario, session_id, config_, start->mutable_session());
    start->set_start_data_stream(true);
    start->set_emit_config_snapshot_to_data_stream(true);
    start->set_require_valid_saved_configs(true);

    const std::string req_bytes = req.SerializeAsString();
    if (!SendControlPayload(std::vector<std::uint8_t>(req_bytes.begin(), req_bytes.end()))) {
        result.error = last_error_;
        return result;
    }

    std::vector<std::uint8_t> resp_bytes;
    if (!RecvControlPayload(resp_bytes)) {
        result.error = last_error_;
        return result;
    }

    ego::v1::ControlResponse resp;
    if (!resp.ParseFromArray(resp_bytes.data(), static_cast<int>(resp_bytes.size()))) {
        result.error = "control response parse failed";
        return result;
    }
    if (!resp.has_start_session()) {
        result.error = "unexpected control response type";
        return result;
    }
    const auto& start_resp = resp.start_session();
    result.session_id = start_resp.session_id();
    if (start_resp.result() != ego::v1::RESULT_CODE_OK) {
        result.error =
            start_resp.error().message().empty() ? "start_session rejected" : start_resp.error().message();
        for (const int missing : start_resp.error().missing_configs()) {
            result.missing_configs.push_back(std::to_string(missing));
        }
        for (const int invalid : start_resp.error().invalid_configs()) {
            result.invalid_configs.push_back(std::to_string(invalid));
        }
        return result;
    }
    result.ok = true;
    if (result.session_id.empty()) {
        result.session_id = session_id;
    }
    return result;
}

ControlStopSessionResult ContractControlClient::StopSession(const std::string& session_id,
                                                            const std::string& reason) {
    ControlStopSessionResult result{};
    if (!connected_ && !Connect()) {
        result.error = last_error_;
        return result;
    }

    ego::v1::ControlRequest req;
    req.set_request_id(request_id_++);
    req.mutable_stop_session()->set_session_id(session_id);
    req.mutable_stop_session()->set_reason(reason);

    const std::string req_bytes = req.SerializeAsString();
    if (!SendControlPayload(std::vector<std::uint8_t>(req_bytes.begin(), req_bytes.end()))) {
        result.error = last_error_;
        return result;
    }

    std::vector<std::uint8_t> resp_bytes;
    if (!RecvControlPayload(resp_bytes)) {
        result.error = last_error_;
        return result;
    }

    ego::v1::ControlResponse resp;
    if (!resp.ParseFromArray(resp_bytes.data(), static_cast<int>(resp_bytes.size()))) {
        result.error = "control response parse failed";
        return result;
    }
    if (!resp.has_stop_session()) {
        result.error = "unexpected control response type";
        return result;
    }
    const auto& stop_resp = resp.stop_session();
    result.session_id = stop_resp.session_id();
    if (stop_resp.result() != ego::v1::RESULT_CODE_OK) {
        result.error =
            stop_resp.error().message().empty() ? "stop_session rejected" : stop_resp.error().message();
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace ego_runtime
