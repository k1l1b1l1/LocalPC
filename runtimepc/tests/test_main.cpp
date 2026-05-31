#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "ego_contract/crc32.hpp"
#include "ego_protocol_packets.hpp"
#include "ego_runtime/chunk_writer.hpp"
#include "ego_runtime/config.hpp"
#include "ego_runtime/contract_frame_io.hpp"
#include "ego_runtime/session_integrity.hpp"

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

}  // namespace

int main() {
    try {
        TestContractFrameValidation();
        TestSeqGapDetectionInWriter();
        TestFileIngestContract();
        std::cout << "All ego_runtime tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "Test failure: " << ex.what() << "\n";
        return 1;
    }
}
