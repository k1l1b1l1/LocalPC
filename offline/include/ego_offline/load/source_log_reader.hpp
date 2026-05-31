#pragma once
// LD-02: source_log_reader — читает source.bin (контракт source_bin_v1)

#include "ego_offline/load/ego_log_reader.hpp"   // RawPacket, IndexEntry
#include "ego_offline/protocol.hpp"

#include <filesystem>
#include <functional>
#include <vector>
#include <string>

namespace ego_offline::load {

struct SourceManifest {
    std::string            session_id;
    ns_t                   first_ts_ns  = 0;
    ns_t                   last_ts_ns   = 0;
    uint64_t               packet_count = 0;
    std::vector<ChunkInfo> chunks;     // usually just one file

    static SourceManifest from_json(const std::filesystem::path& path);
    static SourceManifest single_file(const std::filesystem::path& bin_path);
};

class SourceLogReader {
public:
    explicit SourceLogReader(const SourceManifest& manifest);

    // Sequential scan — same contract as EgoLogReader::scan
    void scan(std::function<bool(const RawPacket&)> cb) const;

    void build_index(const std::filesystem::path& index_path = {});

    bool verify_packet_count(uint64_t& actual_count) const;

    const SourceManifest& manifest() const { return manifest_; }

private:
    SourceManifest          manifest_;
    std::vector<IndexEntry> index_;
    bool                    index_built_ = false;
};

} // namespace ego_offline::load
