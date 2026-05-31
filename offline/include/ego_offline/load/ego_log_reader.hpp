#pragma once
// LD-01: ego_log_reader — читает чанки ego_*.bin через ego_manifest.json
// Поддерживает sequential и indexed-доступ.

#include "ego_offline/ego_contract_protocol.hpp"
#include "ego_offline/protocol.hpp"
#include "ego_offline/types.hpp"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <cstdint>

namespace ego_offline::load {

// ── Manifest ─────────────────────────────────────────────────────────────────
struct ChunkInfo {
    std::filesystem::path path;
    ns_t                  first_ts_ns = 0;
    ns_t                  last_ts_ns  = 0;
    uint64_t              packet_count = 0;
    int                   chunk_id     = 0;
};

struct EgoManifest {
    std::string            session_id;
    ns_t                   first_ts_ns   = 0;
    ns_t                   last_ts_ns    = 0;
    uint64_t               packet_count  = 0;  // expected total
    std::vector<ChunkInfo> chunks;

    static EgoManifest from_json(const std::filesystem::path& manifest_path);
};

// ── Index entry ───────────────────────────────────────────────────────────────
struct IndexEntry {
    uint64_t offset     = 0;   // byte offset in chunk file
    ns_t     ts_ns      = 0;
    uint16_t type       = 0;
    seq_t    seq        = 0;
    int      chunk_id   = 0;
};

// ── Parsed packet (header + payload bytes) ───────────────────────────────────
struct RawPacket {
    PacketHeader         header{};
    contract::EgoFrameHeader contract_header{};
    bool                 is_contract = false;
    std::vector<uint8_t> payload;
    int                  chunk_id = 0;   // which chunk this came from
    uint64_t             file_offset = 0;
};

// ── Reader ────────────────────────────────────────────────────────────────────
class EgoLogReader {
public:
    explicit EgoLogReader(const EgoManifest& manifest);

    // Sequential scan — calls cb for each valid raw packet.
    // cb returns false → stop iteration early.
    void scan(std::function<bool(const RawPacket&)> cb) const;

    // Build an in-memory index (or load from .index CSV).
    // Needed for random-access by ts_ns.
    void build_index(const std::filesystem::path& index_path = {});

    // Random access: returns all packets in [t_start, t_end] ns.
    std::vector<RawPacket> read_range(ns_t t_start, ns_t t_end) const;

    // Verify packet count matches manifest (LD-01 criterion).
    bool verify_packet_count(uint64_t& actual_count) const;

    const EgoManifest& manifest() const { return manifest_; }
    bool is_contract_format() const { return format_ == EgoBinFormat::kContract; }

private:
    enum class EgoBinFormat { kUnknown, kLegacy, kContract };
    static EgoBinFormat detect_format(const std::filesystem::path& path);
    void scan_contract_chunk(std::ifstream& f, int chunk_id, std::function<bool(const RawPacket&)> cb) const;
    void scan_legacy_chunk(std::ifstream& f, int chunk_id, std::function<bool(const RawPacket&)> cb) const;

    EgoBinFormat format_ = EgoBinFormat::kUnknown;
    EgoManifest             manifest_;
    std::vector<IndexEntry> index_;
    bool                    index_built_ = false;
};

} // namespace ego_offline::load
