#include "ego_runtime/chunk_writer.hpp"

#include <cstring>
#include <filesystem>
#include <sstream>

#include "ego_contract/crc32.hpp"
#include "ego_protocol_packets.hpp"
#include "ego_runtime/util.hpp"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace ego_runtime {
namespace {

#if !defined(_WIN32)
void FsyncPath(const std::string& path) {
    if (path.empty()) {
        return;
    }
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        ::close(fd);
    }
}
#endif

}  // namespace

ChunkWriter::ChunkWriter(std::string session_dir, RuntimeConfig config)
    : session_dir_(std::move(session_dir)),
      config_(std::move(config)),
      last_flush_time_(std::chrono::steady_clock::now()) {}

bool ChunkWriter::Open() {
    std::lock_guard<std::mutex> lock(mu_);
    std::error_code ec;
    std::filesystem::create_directories(session_dir_, ec);
    return OpenChunk();
}

bool ChunkWriter::OpenChunk() {
    const std::string filename =
        "ego_" + std::to_string(current_chunk_id_) + ".bin";
    const std::string path = (std::filesystem::path(session_dir_) / filename).string();
    chunk_path_ = path;
    chunk_stream_.open(path, std::ios::binary | std::ios::trunc);
    if (!chunk_stream_.good()) {
        return false;
    }
    if (!index_stream_.is_open()) {
        const std::string index_path = (std::filesystem::path(session_dir_) / "ego.index").string();
        index_path_ = index_path;
        index_stream_.open(index_path, std::ios::app);
    }
    current_chunk_offset_ = 0U;
    current_chunk_bytes_ = 0U;
    chunk_opened_at_ = std::chrono::steady_clock::now();
    ChunkInfo info{};
    info.filename = filename;
    chunks_.push_back(info);
    return true;
}

bool ChunkWriter::RotateIfNeeded() {
    if (!chunk_stream_.is_open()) {
        return OpenChunk();
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - chunk_opened_at_);
    const bool size_limit = current_chunk_bytes_ >= config_.chunk_max_bytes;
    const bool time_limit = elapsed.count() >= static_cast<long long>(config_.chunk_max_sec);
    if (!size_limit && !time_limit) {
        return true;
    }
    Flush(true);
    chunk_stream_.close();
    ++current_chunk_id_;
    return OpenChunk();
}

void ChunkWriter::AppendContractIndex(const std::uint32_t frame_type,
                                      const std::uint64_t seq,
                                      const std::uint64_t t0_ns,
                                      const std::uint64_t t1_ns,
                                      const std::uint64_t offset,
                                      const std::uint32_t payload_size) {
    (void)t1_ns;
    if (!index_stream_.good()) {
        return;
    }
    index_stream_ << offset << ',' << t0_ns << ',' << frame_type << ',' << seq << ','
                  << current_chunk_id_ << ',' << payload_size << '\n';
}

bool ChunkWriter::WriteContractFrame(const std::vector<std::uint8_t>& frame_bytes) {
    std::lock_guard<std::mutex> lock(mu_);
    if (frame_bytes.size() < sizeof(ego::protocol::v1::EgoFrameHeader)) {
        return false;
    }
    if (!RotateIfNeeded()) {
        return false;
    }
    ego::protocol::v1::EgoFrameHeader header{};
    std::memcpy(&header, frame_bytes.data(), sizeof(header));
    if (header.magic != ego::protocol::v1::EGO_FRAME_MAGIC) {
        return false;
    }
    chunk_stream_.write(reinterpret_cast<const char*>(frame_bytes.data()),
                        static_cast<std::streamsize>(frame_bytes.size()));
    if (!chunk_stream_.good()) {
        return false;
    }
    AppendContractIndex(header.frame_type, header.seq, header.t0_ns, header.t1_ns, current_chunk_offset_,
                        header.payload_size);
    current_chunk_offset_ += frame_bytes.size();
    current_chunk_bytes_ += frame_bytes.size();
    total_bytes_ += frame_bytes.size();
    ++total_packets_;
    if (!chunks_.empty()) {
        auto& chunk = chunks_.back();
        chunk.bytes += frame_bytes.size();
        chunk.packet_count += 1U;
        if (chunk.packet_count == 1U) {
            chunk.first_ts_ns = header.t0_ns;
        }
        chunk.last_ts_ns = header.t1_ns;
    }
    ++packets_since_flush_;
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - last_flush_time_);
    if (packets_since_flush_ >= config_.flush_packets ||
        elapsed.count() >= static_cast<long long>(config_.flush_interval_sec)) {
#if !defined(_WIN32)
        Flush(true);
#else
        Flush(false);
#endif
    }
    return true;
}

void ChunkWriter::Flush(bool fsync_now) {
    if (chunk_stream_.is_open()) {
        chunk_stream_.flush();
#if !defined(_WIN32)
        if (fsync_now) {
            FsyncPath(chunk_path_);
        }
#endif
    }
    if (index_stream_.is_open()) {
        index_stream_.flush();
#if !defined(_WIN32)
        if (fsync_now) {
            FsyncPath(index_path_);
        }
#endif
    }
    packets_since_flush_ = 0U;
    last_flush_time_ = std::chrono::steady_clock::now();
    UpdateManifest();
}

void ChunkWriter::UpdateManifest() {
    std::ostringstream json;
    json << "{\n  \"chunks\": [\n";
    for (std::size_t i = 0U; i < chunks_.size(); ++i) {
        const auto& c = chunks_[i];
        json << "    {\n";
        json << "      \"chunk_id\": " << i << ",\n";
        json << "      \"file\": \"" << JsonEscape(c.filename) << "\",\n";
        json << "      \"bytes\": " << c.bytes << ",\n";
        json << "      \"first_ts_ns\": " << c.first_ts_ns << ",\n";
        json << "      \"last_ts_ns\": " << c.last_ts_ns << ",\n";
        json << "      \"packet_count\": " << c.packet_count << "\n";
        json << "    }" << (i + 1U < chunks_.size() ? "," : "") << "\n";
    }
    json << "  ]\n}\n";
    WriteTextAtomic((std::filesystem::path(session_dir_) / "ego_manifest.json").string(), json.str());
}

void ChunkWriter::Close() {
    std::lock_guard<std::mutex> lock(mu_);
    Flush(true);
    if (chunk_stream_.is_open()) {
        chunk_stream_.close();
    }
    if (index_stream_.is_open()) {
        index_stream_.close();
    }
}

std::uint64_t ChunkWriter::TotalBytesWritten() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_bytes_;
}

std::uint32_t ChunkWriter::TotalPacketsWritten() const {
    std::lock_guard<std::mutex> lock(mu_);
    return total_packets_;
}

}  // namespace ego_runtime
