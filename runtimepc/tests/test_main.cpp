#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

#include "ego_contract/crc32.hpp"
#include "ego_protocol_packets.hpp"
#include "ego_runtime/chunk_writer.hpp"
#include "ego_runtime/config.hpp"
#include "ego_runtime/contract_frame_io.hpp"
#include "ego_runtime/contract_tcp_client.hpp"
#include "ego_runtime/nav_history.hpp"
#include "ego_runtime/nav_provider.hpp"
#include "ego_runtime/nav_sidecar_writer.hpp"
#include "ego_runtime/runtime_service.hpp"
#include "ego_runtime/session_integrity.hpp"
#include "ego_runtime/session_checkpoint.hpp"

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<std::uint8_t> BuildContractFrame(std::uint64_t seq,
                                             ego::protocol::v1::FramePayloadType type,
                                             const std::vector<std::uint8_t>& payload) {
    using namespace ego::protocol::v1;
    const std::uint32_t payload_crc = payload.empty() ? 0U : ego_contract::Crc32(payload);
    EgoFrameHeader header = make_frame_header(type, to_u32(FrameFlags::PAYLOAD_BINARY), 0U, 0U, seq, 1000U + seq,
                                              1001U + seq, static_cast<std::uint32_t>(payload.size()), payload_crc);
    EgoFrameHeader zero_hdr = header;
    zero_hdr.header_crc32 = 0U;
    header.header_crc32 = ego_contract::Crc32(reinterpret_cast<const std::uint8_t*>(&zero_hdr), sizeof(zero_hdr));

    std::vector<std::uint8_t> frame(sizeof(header) + payload.size());
    std::memcpy(frame.data(), &header, sizeof(header));
    if (!payload.empty()) {
        std::memcpy(frame.data() + sizeof(header), payload.data(), payload.size());
    }
    return frame;
}

std::string BuildNmeaSentence(const std::string& body) {
    std::uint8_t checksum = 0U;
    for (const char ch : body) {
        checksum ^= static_cast<std::uint8_t>(ch);
    }
    std::ostringstream out;
    out << '$' << body << '*'
        << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(checksum);
    return out.str();
}

void TestContractFrameValidation() {
    const auto frame = BuildContractFrame(1U, ego::protocol::v1::FramePayloadType::AUDIO_BLOCK, {1, 2, 3, 4});
    Require(ego_runtime::ValidateContractFrame(frame, 65536U), "valid contract frame must pass");
    auto bad = frame;
    bad[sizeof(ego::protocol::v1::EgoFrameHeader)] ^= 0xFFU;
    Require(!ego_runtime::ValidateContractFrame(bad, 65536U), "corrupted payload must fail");
}

void TestSeqGapDetectionInWriter() {
    const std::string session_dir =
        (std::filesystem::temp_directory_path() / "ego_runtime_contract_test").string();
    std::error_code ec;
    std::filesystem::remove_all(session_dir, ec);
    std::filesystem::create_directories(session_dir, ec);

    ego_runtime::RuntimeConfig cfg{};
    cfg.transport = ego_runtime::TransportMode::kEgoContractTcp;
    cfg.chunk_max_bytes = 1'000'000U;
    cfg.flush_packets = 1U;
    cfg.flush_interval_sec = 0U;

    ego_runtime::ChunkWriter writer(session_dir, cfg);
    Require(writer.Open(), "chunk writer open failed");
    for (std::uint64_t seq = 1U; seq <= 5U; ++seq) {
        const auto frame = BuildContractFrame(seq, ego::protocol::v1::FramePayloadType::AUDIO_BLOCK,
                                              {static_cast<std::uint8_t>(seq)});
        Require(writer.WriteContractFrame(frame), "write failed");
    }
    writer.Close();

    const auto integrity = ego_runtime::CheckSessionIntegrity(session_dir, 65536U);
    Require(integrity.result == ego_runtime::IntegrityResult::kOk,
            std::string("integrity failed: ") + integrity.detail);
    std::filesystem::remove_all(session_dir, ec);
}

void TestCheckpointAndIndexTail() {
    const std::string data_root =
        (std::filesystem::temp_directory_path() / "ego_runtime_checkpoint_root").string();
    const std::string session_dir = data_root + "/sessions/session-checkpoint-test";
    std::error_code ec;
    std::filesystem::remove_all(data_root, ec);
    std::filesystem::create_directories(session_dir + "/logs", ec);

    ego_runtime::RuntimeConfig cfg{};
    cfg.chunk_max_bytes = 1'000'000U;
    cfg.flush_packets = 1U;
    ego_runtime::ChunkWriter writer(session_dir, cfg);
    Require(writer.Open(), "open failed");
    for (std::uint64_t seq = 1U; seq <= 4U; ++seq) {
        const auto frame = BuildContractFrame(seq, ego::protocol::v1::FramePayloadType::AUDIO_BLOCK,
                                              {static_cast<std::uint8_t>(seq)});
        Require(writer.WriteContractFrame(frame), "write failed");
    }
    writer.Close();

    ego_runtime::IndexTail tail{};
    Require(ego_runtime::ReadIndexTail(session_dir, tail), "index tail read failed");
    Require(tail.last_seq == 4U, "expected last_seq=4");
    Require(tail.line_count == 4U, "expected 4 index lines");

    ego_runtime::SessionCheckpoint cp{};
    cp.session_id = "session-test";
    cp.board_session_id = "board-1";
    cp.last_seq = tail.last_seq;
    cp.last_ts_ns = tail.last_ts_ns;
    cp.chunk_id = tail.chunk_id;
    cp.data_link = "up";
    cp.updated_at_utc = "2026-01-01T00:00:00Z";
    Require(ego_runtime::WriteCheckpoint(session_dir, cp), "checkpoint write failed");

    ego_runtime::SessionCheckpoint loaded{};
    Require(ego_runtime::LoadCheckpoint(session_dir, loaded), "checkpoint load failed");
    Require(loaded.last_seq == 4U, "checkpoint last_seq mismatch");
    Require(loaded.board_session_id == "board-1", "checkpoint board id mismatch");

    ego_runtime::ChunkWriter writer2(session_dir, cfg);
    Require(writer2.OpenForResume(tail), "resume open failed");
    const auto frame5 = BuildContractFrame(5U, ego::protocol::v1::FramePayloadType::AUDIO_BLOCK, {5U});
    Require(writer2.WriteContractFrame(frame5), "append after resume failed");
    writer2.Close();

    ego_runtime::IndexTail tail2{};
    Require(ego_runtime::ReadIndexTail(session_dir, tail2), "index tail2 read failed");
    Require(tail2.last_seq == 5U, "expected last_seq=5 after resume append");
    Require(!ego_runtime::SessionIsFinalized(session_dir), "session must not be finalized");
    Require(ego_runtime::FindResumableSessionDir(data_root) == session_dir, "resumable dir lookup");

    const int abandoned = ego_runtime::AbandonResumableSessions(data_root, "test_abandon");
    Require(abandoned == 1, "expected one abandoned session");
    Require(ego_runtime::SessionIsFinalized(session_dir), "session must be finalized after abandon");
    Require(ego_runtime::FindResumableSessionDir(data_root).empty(), "no resumable after abandon");

    std::filesystem::remove_all(data_root, ec);
}

struct MockReplayServer {
#if defined(_WIN32)
    SOCKET listen_fd = INVALID_SOCKET;
#else
    int listen_fd = -1;
#endif
    std::uint16_t port = 0U;
    std::atomic<int> connection_count{0};
    std::atomic<bool> stop{false};
    std::thread accept_thread{};

    ~MockReplayServer() { Stop(); }

    bool Start() {
#if defined(_WIN32)
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd == INVALID_SOCKET) {
            return false;
        }
#else
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            return false;
        }
#endif
        int yes = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return false;
        }
        port = ntohs(addr.sin_port);
        if (listen(listen_fd, 4) != 0) {
            return false;
        }
        accept_thread = std::thread([this]() { AcceptLoop(); });
        return true;
    }

    void Stop() {
        stop = true;
#if defined(_WIN32)
        if (listen_fd != INVALID_SOCKET) {
            closesocket(listen_fd);
            listen_fd = INVALID_SOCKET;
        }
#else
        if (listen_fd >= 0) {
            close(listen_fd);
            listen_fd = -1;
        }
#endif
        if (accept_thread.joinable()) {
            accept_thread.join();
        }
    }

    void AcceptLoop() {
        while (!stop.load()) {
#if defined(_WIN32)
            const SOCKET client = accept(listen_fd, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                break;
            }
#else
            const int client = accept(listen_fd, nullptr, nullptr);
            if (client < 0) {
                break;
            }
#endif
            const int conn = ++connection_count;
            std::thread([this, client, conn]() {
                const std::uint64_t replay_end = (conn == 1) ? 5U : 10U;
                for (std::uint64_t seq = 1U; seq <= replay_end; ++seq) {
                    const auto frame = BuildContractFrame(seq, ego::protocol::v1::FramePayloadType::AUDIO_BLOCK,
                                                          {static_cast<std::uint8_t>(seq)});
                    const char* data = reinterpret_cast<const char*>(frame.data());
                    std::size_t sent = 0U;
                    while (sent < frame.size()) {
#if defined(_WIN32)
                        const int r = send(client, data + sent, static_cast<int>(frame.size() - sent), 0);
#else
                        const ssize_t r = send(client, data + sent, frame.size() - sent, 0);
#endif
                        if (r <= 0) {
                            break;
                        }
                        sent += static_cast<std::size_t>(r);
                    }
                }
#if defined(_WIN32)
                shutdown(client, SD_BOTH);
                closesocket(client);
#else
                shutdown(client, SHUT_RDWR);
                close(client);
#endif
            }).detach();
        }
    }
};

struct MockNmeaServer {
#if defined(_WIN32)
    SOCKET listen_fd = INVALID_SOCKET;
#else
    int listen_fd = -1;
#endif
    std::uint16_t port = 0U;
    std::atomic<bool> stop{false};
    std::thread accept_thread{};

    ~MockNmeaServer() { Stop(); }

    bool Start() {
#if defined(_WIN32)
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd == INVALID_SOCKET) {
            return false;
        }
#else
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            return false;
        }
#endif
        int yes = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&yes), sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            return false;
        }
        socklen_t len = sizeof(addr);
        if (getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
            return false;
        }
        port = ntohs(addr.sin_port);
        if (listen(listen_fd, 1) != 0) {
            return false;
        }
        accept_thread = std::thread([this]() { AcceptLoop(); });
        return true;
    }

    void Stop() {
        stop = true;
#if defined(_WIN32)
        if (listen_fd != INVALID_SOCKET) {
            closesocket(listen_fd);
            listen_fd = INVALID_SOCKET;
        }
#else
        if (listen_fd >= 0) {
            close(listen_fd);
            listen_fd = -1;
        }
#endif
        if (accept_thread.joinable()) {
            accept_thread.join();
        }
    }

    void AcceptLoop() {
        while (!stop.load()) {
#if defined(_WIN32)
            const SOCKET client = accept(listen_fd, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                break;
            }
#else
            const int client = accept(listen_fd, nullptr, nullptr);
            if (client < 0) {
                break;
            }
#endif
            std::thread([client]() {
                const std::string payload =
                    BuildNmeaSentence("GPRMC,120000.00,A,5545.0720,N,03737.1040,E,0.50,161.3,140624,,,A") + "\r\n" +
                    BuildNmeaSentence("GPGGA,120000.00,5545.0720,N,03737.1040,E,1,07,2.1,170.0,M,0.0,M,,") + "\r\n";
                std::size_t sent = 0U;
                while (sent < payload.size()) {
#if defined(_WIN32)
                    const int rc = send(client, payload.data() + sent, static_cast<int>(payload.size() - sent), 0);
#else
                    const ssize_t rc = send(client, payload.data() + sent, payload.size() - sent, 0);
#endif
                    if (rc <= 0) {
                        break;
                    }
                    sent += static_cast<std::size_t>(rc);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
#if defined(_WIN32)
                shutdown(client, SD_BOTH);
                closesocket(client);
#else
                shutdown(client, SHUT_RDWR);
                close(client);
#endif
            }).detach();
        }
    }
};

void TestIT04ReplayReconnectDedup() {
    MockReplayServer server;
    Require(server.Start(), "mock replay server start failed");

    const std::string session_dir =
        (std::filesystem::temp_directory_path() / "ego_runtime_it04").string();
    std::error_code ec;
    std::filesystem::remove_all(session_dir, ec);
    std::filesystem::create_directories(session_dir, ec);

    ego_runtime::RuntimeConfig cfg{};
    cfg.ego_host = "127.0.0.1";
    cfg.data_port = server.port;
    cfg.max_payload_bytes = 65536U;

    ego_runtime::ChunkWriter writer(session_dir, cfg);
    Require(writer.Open(), "writer open failed");

    std::uint64_t last_seq = 0U;
    bool seq_initialized = false;
    std::uint64_t packets_replayed = 0U;
    std::uint64_t packets_written = 0U;

    ego_runtime::ContractTcpClient client(
        cfg, [&](ego_runtime::ContractFrame frame) {
            ego::protocol::v1::EgoFrameHeader header{};
            std::memcpy(&header, frame.bytes.data(), sizeof(header));
            bool is_replay = false;
            if (seq_initialized && header.seq <= last_seq) {
                is_replay = true;
                ++packets_replayed;
            } else {
                if (!seq_initialized) {
                    seq_initialized = true;
                }
                last_seq = header.seq;
            }
            if (!is_replay) {
                if (writer.WriteContractFrame(frame.bytes)) {
                    ++packets_written;
                }
            }
        });

    Require(client.Start(), "client first connect failed");
    for (int i = 0; i < 200 && client.Running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Require(!client.Running(), "expected disconnect after first batch");
    Require(last_seq == 5U, "expected last_seq=5 after first connect");
    Require(packets_written == 5U, "expected 5 packets written on first connect");

    const std::uint64_t replayed_after_first = packets_replayed;
    Require(client.Reconnect(), "client reconnect failed");
    for (int i = 0; i < 300 && client.Running(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Require(!client.Running(), "expected disconnect after replay batch");
    Require(last_seq == 10U, "expected last_seq=10 after replay");
    Require(packets_written == 10U, "expected 10 total packets written (dedup replay)");
    Require(packets_replayed >= 5U, "expected replayed packets on reconnect");
    Require(packets_replayed > replayed_after_first, "replay count must increase on reconnect");

    writer.Close();
    ego_runtime::IndexTail tail{};
    Require(ego_runtime::ReadIndexTail(session_dir, tail), "index tail read failed");
    Require(tail.last_seq == 10U, "index last_seq must be 10");
    Require(tail.line_count == 10U, "index must have 10 lines without duplicate seq");

    client.Stop();
    server.Stop();
    std::filesystem::remove_all(session_dir, ec);
}

void TestFileIngestContract() {
    const std::string session_dir =
        (std::filesystem::temp_directory_path() / "ego_runtime_file_ingest").string();
    std::error_code ec;
    std::filesystem::remove_all(session_dir, ec);

    const auto bin_path = session_dir + "/input.bin";
    std::filesystem::create_directories(session_dir, ec);
    {
        std::ofstream out(bin_path, std::ios::binary);
        for (std::uint64_t seq = 1U; seq <= 3U; ++seq) {
            const auto frame = BuildContractFrame(seq, ego::protocol::v1::FramePayloadType::IMU_WINDOW, {9U});
            out.write(reinterpret_cast<const char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
        }
    }

    std::size_t count = 0U;
    Require(ego_runtime::ReadContractFramesFromFile(
                bin_path, 65536U,
                [&](const std::vector<std::uint8_t>& frame) {
                    ++count;
                    return ego_runtime::ValidateContractFrame(frame, 65536U);
                }),
            "file ingest failed");
    Require(count == 3U, "expected 3 frames");
    std::filesystem::remove_all(session_dir, ec);
}

void TestFileIngestKeepsSessionEnded() {
    const std::string data_root =
        (std::filesystem::temp_directory_path() / "ego_runtime_file_session_ended").string();
    std::error_code ec;
    std::filesystem::remove_all(data_root, ec);
    std::filesystem::create_directories(data_root, ec);

    const std::string input_path = data_root + "/input.bin";
    {
        std::ofstream out(input_path, std::ios::binary);
        const auto started = BuildContractFrame(1U, ego::protocol::v1::FramePayloadType::SESSION_STARTED, {1U});
        const auto config = BuildContractFrame(2U, ego::protocol::v1::FramePayloadType::CONFIG_SNAPSHOT, {2U});
        const auto realtime = BuildContractFrame(3U, ego::protocol::v1::FramePayloadType::IMU_WINDOW, {3U});
        const auto ended = BuildContractFrame(4U, ego::protocol::v1::FramePayloadType::SESSION_ENDED, {4U});
        out.write(reinterpret_cast<const char*>(started.data()), static_cast<std::streamsize>(started.size()));
        out.write(reinterpret_cast<const char*>(config.data()), static_cast<std::streamsize>(config.size()));
        out.write(reinterpret_cast<const char*>(realtime.data()), static_cast<std::streamsize>(realtime.size()));
        out.write(reinterpret_cast<const char*>(ended.data()), static_cast<std::streamsize>(ended.size()));
    }

    ego_runtime::RuntimeConfig cfg{};
    cfg.data_root = data_root;
    cfg.input_file = input_path;
    cfg.offline.enabled = false;
    cfg.flush_packets = 1U;
    cfg.flush_interval_sec = 0U;
    cfg.max_payload_bytes = 65536U;

    ego_runtime::RuntimeService service(cfg);
    Require(service.StartDaemon(), "start daemon failed");
    const ego_runtime::ScenarioMetadata scenario{"test-session-ended", "test", "operator", ""};
    Require(service.StartRecording(scenario) == ego_runtime::RuntimeErrorCode::kOk, "start local recording failed");
    service.ProcessFileInput(input_path);
    Require(service.StopRecording("file_eof") == ego_runtime::RuntimeErrorCode::kOk, "stop recording failed");

    const std::string session_dir = service.Status().session_dir;
    Require(!session_dir.empty(), "session_dir must exist after stop");

    std::size_t frame_count = 0U;
    std::uint32_t last_type = 0U;
    std::uint64_t last_seq = 0U;
    Require(ego_runtime::ReadContractFramesFromFile(
                session_dir + "/ego_0.bin", 65536U,
                [&](const std::vector<std::uint8_t>& frame) {
                    ego::protocol::v1::EgoFrameHeader header{};
                    std::memcpy(&header, frame.data(), sizeof(header));
                    last_type = header.frame_type;
                    last_seq = header.seq;
                    ++frame_count;
                    return true;
                }),
            "read output ego_0.bin failed");
    Require(frame_count == 4U, "expected 4 frames including SessionEnded");
    Require(last_type == static_cast<std::uint32_t>(ego::protocol::v1::FramePayloadType::SESSION_ENDED),
            "last frame must be SessionEnded");
    Require(last_seq == 4U, "last seq must be 4");

    service.StopDaemon();
    std::filesystem::remove_all(data_root, ec);
}

void TestNavSidecarWriter() {
    const std::string session_dir =
        (std::filesystem::temp_directory_path() / "ego_runtime_nav_sidecar").string();
    std::error_code ec;
    std::filesystem::remove_all(session_dir, ec);
    std::filesystem::create_directories(session_dir, ec);

    ego_runtime::RuntimeConfig cfg{};
    cfg.nav_sidecar_filename = "ego_nav.jsonl";
    ego_runtime::NavSidecarWriter writer(cfg);
    Require(writer.Open(session_dir), "nav sidecar writer open failed");

    ego_runtime::NavSnapshot snapshot{};
    snapshot.sample_id = 1U;
    snapshot.latitude_deg = 55.7512;
    snapshot.longitude_deg = 37.6184;
    snapshot.altitude_m = 170.0;
    snapshot.speed_mps = 0.5f;
    snapshot.heading_deg = 161.3f;
    snapshot.fix_quality = 1U;
    snapshot.satellites = 7U;
    snapshot.hdop = 2.1f;
    snapshot.source = "local_m2_tcp";
    Require(writer.Write(snapshot, 123456789U), "nav sidecar write failed");
    writer.Close();

    const std::filesystem::path sidecar_path = std::filesystem::path(session_dir) / "ego_nav.jsonl";
    std::ifstream input(sidecar_path);
    Require(input.good(), "nav sidecar file missing");
    std::string line;
    std::getline(input, line);
    Require(line.find("\"ts_ns\":123456789") != std::string::npos, "sidecar ts missing");
    Require(line.find("\"source\":\"local_m2_tcp\"") != std::string::npos, "sidecar source missing");

    std::filesystem::remove_all(session_dir, ec);
}

void TestNavProviderTcpNmea() {
    MockNmeaServer server;
    Require(server.Start(), "mock NMEA server start failed");

    ego_runtime::RuntimeConfig cfg{};
    cfg.nav_mode = "tcp_nmea";
    cfg.nav_host = "127.0.0.1";
    cfg.nav_port = server.port;
    cfg.nav_stale_timeout_ms = 5000U;
    cfg.nav_fallback_enabled = true;

    ego_runtime::NavProvider provider(cfg);
    Require(provider.Start(), "nav provider start failed");

    ego_runtime::NavSnapshot snapshot{};
    bool received = false;
    for (int i = 0; i < 40; ++i) {
        if (provider.GetSnapshot(&snapshot)) {
            received = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Require(received, "nav provider did not receive TCP NMEA snapshot");
    Require(snapshot.fix_quality == 1U, "unexpected GNSS fix quality");
    Require(snapshot.satellites == 7U, "unexpected satellites count");
    Require(std::abs(snapshot.latitude_deg - 55.7512) < 0.001, "unexpected latitude");
    Require(std::abs(snapshot.longitude_deg - 37.6184) < 0.001, "unexpected longitude");
    Require(provider.Status().find("live:127.0.0.1:") == 0, "unexpected nav status");

    provider.Stop();
    server.Stop();
}

void TestNavHistoryMaterializeWindow() {
    const std::filesystem::path runtime_root =
        std::filesystem::temp_directory_path() / "ego_runtime_nav_history_root";
    const std::filesystem::path session_dir =
        std::filesystem::temp_directory_path() / "ego_runtime_nav_history_session";
    std::error_code ec;
    std::filesystem::remove_all(runtime_root, ec);
    std::filesystem::remove_all(session_dir, ec);
    std::filesystem::create_directories(runtime_root / "var" / "nav", ec);
    std::filesystem::create_directories(session_dir, ec);

    const std::filesystem::path history_path =
        runtime_root / "var" / "nav" / "ego_nav_history.jsonl";
    {
        std::ofstream out(history_path);
        out << "{\"ts_ns\":1000,\"lat_deg\":55.0,\"lon_deg\":37.0,\"fix_quality\":1}\n";
        out << "{\"ts_ns\":2000,\"lat_deg\":55.1,\"lon_deg\":37.1,\"fix_quality\":1}\n";
        out << "{\"ts_ns\":3000,\"lat_deg\":55.2,\"lon_deg\":37.2,\"fix_quality\":1}\n";
        out << "{\"broken\":true}\n";
    }

    const auto result = ego_runtime::MaterializeNavHistoryWindow(
        runtime_root.string(), session_dir.string(), "ego_nav.jsonl", 1500U, 2500U);
    Require(result.history_available, "nav history must be available");
    Require(result.copied_samples == 1U, "expected one copied nav sample");
    Require(result.skipped_lines == 1U, "expected one skipped invalid history line");
    Require(!result.sidecar_path.empty(), "sidecar path must be set");

    std::ifstream input(result.sidecar_path);
    Require(input.good(), "materialized nav sidecar missing");
    std::string line;
    std::getline(input, line);
    Require(line.find("\"ts_ns\":2000") != std::string::npos,
            "materialized nav sidecar must contain selected sample");

    std::filesystem::remove_all(runtime_root, ec);
    std::filesystem::remove_all(session_dir, ec);
}

}  // namespace

int main() {
    try {
        TestContractFrameValidation();
        TestSeqGapDetectionInWriter();
        TestCheckpointAndIndexTail();
        TestIT04ReplayReconnectDedup();
        TestFileIngestContract();
        TestFileIngestKeepsSessionEnded();
        TestNavSidecarWriter();
        TestNavProviderTcpNmea();
        TestNavHistoryMaterializeWindow();
        std::cout << "All ego_runtime tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failure: " << ex.what() << "\n";
        return 1;
    }
}
