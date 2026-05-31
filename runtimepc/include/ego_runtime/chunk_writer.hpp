#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "ego_runtime/config.hpp"

namespace ego_runtime {

struct ChunkInfo {
    std::string filename;
    std::uint64_t bytes = 0U;
    std::uint64_t first_ts_ns = 0U;
    std::uint64_t last_ts_ns = 0U;
    std::uint32_t packet_count = 0U;
};

class ChunkWriter {
public:
    ChunkWriter(std::string session_dir, RuntimeConfig config);

    bool Open();
    bool WriteContractFrame(const std::vector<std::uint8_t>& frame_bytes);
    bool WritePacket(const std::vector<std::uint8_t>& frame_bytes) { return WriteContractFrame(frame_bytes); }
    void Flush(bool fsync_now);
    void Close();

    std::uint64_t TotalBytesWritten() const;
    std::uint32_t TotalPacketsWritten() const;
    const std::vector<ChunkInfo>& Chunks() const { return chunks_; }

private:
    bool RotateIfNeeded();
    bool OpenChunk();
    void UpdateManifest();
    void AppendContractIndex(std::uint32_t frame_type,
                             std::uint64_t seq,
                             std::uint64_t t0_ns,
                             std::uint64_t t1_ns,
                             std::uint64_t offset,
                             std::uint32_t payload_size);

    std::string session_dir_;
    RuntimeConfig config_;
    std::ofstream chunk_stream_{};
    std::ofstream index_stream_{};
    std::uint32_t current_chunk_id_ = 0U;
    std::uint64_t current_chunk_offset_ = 0U;
    std::uint64_t current_chunk_bytes_ = 0U;
    std::uint32_t packets_since_flush_ = 0U;
    std::chrono::steady_clock::time_point last_flush_time_{};
    std::chrono::steady_clock::time_point chunk_opened_at_{};
    std::uint64_t total_bytes_ = 0U;
    std::uint32_t total_packets_ = 0U;
    std::vector<ChunkInfo> chunks_{};
    mutable std::mutex mu_{};
};

}  // namespace ego_runtime
