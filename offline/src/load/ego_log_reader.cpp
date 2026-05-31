#include "ego_offline/load/ego_log_reader.hpp"
#include "ego_offline/json_reader.hpp"
#include "ego_offline/checksum.hpp"
#include "ego_offline/ego_contract_protocol.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cstring>

namespace ego_offline::load {

EgoManifest EgoManifest::from_json(const std::filesystem::path& manifest_path) {
    auto j = json_load(manifest_path.string());

    EgoManifest m;
    m.session_id   = j.value<std::string>("session_id",  "");
    m.first_ts_ns  = j.value<int64_t>("first_ts_ns",  0);
    m.last_ts_ns   = j.value<int64_t>("last_ts_ns",   0);
    m.packet_count = j.value<uint64_t>("packet_count", 0ull);

    auto base_dir = manifest_path.parent_path();

    if (j.contains("chunks")) {
        for (size_t ci = 0; ci < j["chunks"].arr.size(); ++ci) {
            const auto& cj = j["chunks"][ci];
            ChunkInfo c;
            c.chunk_id     = cj.value<int>("chunk_id", 0);
            c.first_ts_ns  = cj.value<int64_t>("first_ts_ns", 0);
            c.last_ts_ns   = cj.value<int64_t>("last_ts_ns",  0);
            c.packet_count = cj.value<uint64_t>("packet_count", 0ull);
            {
                std::string fname = cj.value<std::string>("file", "");
                if (fname.empty()) fname = cj.value<std::string>("filename", "");
                c.path = base_dir / fname;
            }
            m.chunks.push_back(std::move(c));
        }
    } else {
        for (int i = 0; ; ++i) {
            auto p = base_dir / ("ego_" + std::to_string(i) + ".bin");
            if (!std::filesystem::exists(p)) {
                if (i == 0) {
                    p = base_dir / "ego.bin";
                    if (std::filesystem::exists(p)) {
                        ChunkInfo c; c.chunk_id = 0; c.path = p;
                        m.chunks.push_back(c);
                    }
                }
                break;
            }
            ChunkInfo c; c.chunk_id = i; c.path = p;
            m.chunks.push_back(c);
        }
    }
    return m;
}

EgoLogReader::EgoLogReader(const EgoManifest& manifest) : manifest_(manifest) {
    if (!manifest_.chunks.empty()) {
        format_ = detect_format(manifest_.chunks.front().path);
    }
}

EgoLogReader::EgoBinFormat EgoLogReader::detect_format(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return EgoBinFormat::kUnknown;
    }
    uint32_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic == contract::kFrameMagic) {
        return EgoBinFormat::kContract;
    }
    if (magic == kMagic) {
        return EgoBinFormat::kLegacy;
    }
    return EgoBinFormat::kUnknown;
}

void EgoLogReader::scan_legacy_chunk(std::ifstream& f, const int chunk_id,
                                     std::function<bool(const RawPacket&)> cb) const {
    uint64_t file_offset = 0;
    while (f) {
        PacketHeader hdr{};
        if (!f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
            break;
        }
        file_offset += sizeof(hdr);
        if (hdr.magic != kMagic) {
            break;
        }

        RawPacket pkt;
        pkt.header = hdr;
        pkt.chunk_id = chunk_id;
        pkt.file_offset = file_offset - sizeof(hdr);
        pkt.is_contract = false;

        if (hdr.payload_size > 0) {
            pkt.payload.resize(hdr.payload_size);
            if (!f.read(reinterpret_cast<char*>(pkt.payload.data()),
                        static_cast<std::streamsize>(hdr.payload_size))) {
                break;
            }
            file_offset += hdr.payload_size;
        }

        if (!cb(pkt)) {
            return;
        }
    }
}

void EgoLogReader::scan_contract_chunk(std::ifstream& f, const int chunk_id,
                                       std::function<bool(const RawPacket&)> cb) const {
    uint64_t file_offset = 0;
    while (f) {
        contract::EgoFrameHeader hdr{};
        if (!f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) {
            break;
        }
        file_offset += sizeof(hdr);
        if (hdr.magic != contract::kFrameMagic) {
            break;
        }

        RawPacket pkt;
        pkt.contract_header = hdr;
        pkt.is_contract = true;
        pkt.chunk_id = chunk_id;
        pkt.file_offset = file_offset - sizeof(hdr);
        pkt.header.ts_ns = hdr.t0_ns;
        pkt.header.seq = static_cast<seq_t>(hdr.seq);
        pkt.header.type = static_cast<uint16_t>(hdr.frame_type);
        pkt.header.payload_size = hdr.payload_size;

        if (hdr.payload_size > 0) {
            pkt.payload.resize(hdr.payload_size);
            if (!f.read(reinterpret_cast<char*>(pkt.payload.data()),
                        static_cast<std::streamsize>(hdr.payload_size))) {
                break;
            }
            file_offset += hdr.payload_size;
        }

        if (!cb(pkt)) {
            return;
        }
    }
}

void EgoLogReader::scan(std::function<bool(const RawPacket&)> cb) const {
    for (const auto& chunk : manifest_.chunks) {
        std::ifstream f(chunk.path, std::ios::binary);
        if (!f) {
            continue;
        }
        const auto fmt = (format_ == EgoBinFormat::kUnknown) ? detect_format(chunk.path) : format_;
        if (fmt == EgoBinFormat::kContract) {
            scan_contract_chunk(f, chunk.chunk_id, cb);
        } else {
            scan_legacy_chunk(f, chunk.chunk_id, cb);
        }
    }
}

void EgoLogReader::build_index(const std::filesystem::path& index_path) {
    index_.clear();

    if (!index_path.empty() && std::filesystem::exists(index_path)) {
        std::ifstream f(index_path);
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string tok;
            IndexEntry e;
            int col = 0;
            while (std::getline(ss, tok, ',')) {
                switch (col++) {
                    case 0: e.offset   = std::stoull(tok); break;
                    case 1: e.ts_ns    = static_cast<ns_t>(std::stoll(tok)); break;
                    case 2: e.type     = static_cast<uint16_t>(std::stoul(tok)); break;
                    case 3: e.seq      = static_cast<seq_t>(std::stoul(tok)); break;
                    case 4: e.chunk_id = std::stoi(tok); break;
                }
            }
            index_.push_back(e);
        }
        index_built_ = !index_.empty();
        return;
    }

    scan([this](const RawPacket& pkt) {
        IndexEntry e;
        e.offset   = pkt.file_offset;
        e.ts_ns    = static_cast<ns_t>(pkt.header.ts_ns);
        e.type     = pkt.header.type;
        e.seq      = pkt.header.seq;
        e.chunk_id = pkt.chunk_id;
        index_.push_back(e);
        return true;
    });
    std::sort(index_.begin(), index_.end(),
              [](const IndexEntry& a, const IndexEntry& b) { return a.ts_ns < b.ts_ns; });
    index_built_ = true;
}

std::vector<RawPacket> EgoLogReader::read_range(ns_t t_start, ns_t t_end) const {
    std::vector<RawPacket> result;
    if (!index_built_) {
        scan([&](const RawPacket& pkt) {
            ns_t ts = static_cast<ns_t>(pkt.header.ts_ns);
            if (ts >= t_start && ts <= t_end) result.push_back(pkt);
            return true;
        });
        return result;
    }

    auto it_lo = std::lower_bound(index_.begin(), index_.end(), t_start,
        [](const IndexEntry& e, ns_t t) { return e.ts_ns < t; });
    auto it_hi = std::upper_bound(index_.begin(), index_.end(), t_end,
        [](ns_t t, const IndexEntry& e) { return t < e.ts_ns; });

    for (auto it = it_lo; it != it_hi; ++it) {
        const ChunkInfo* chunk_ptr = nullptr;
        for (const auto& c : manifest_.chunks)
            if (c.chunk_id == it->chunk_id) { chunk_ptr = &c; break; }
        if (!chunk_ptr) continue;

        std::ifstream f(chunk_ptr->path, std::ios::binary);
        if (!f) continue;
        f.seekg(static_cast<std::streamoff>(it->offset));

        PacketHeader hdr{};
        if (!f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) continue;

        RawPacket pkt;
        pkt.header      = hdr;
        pkt.chunk_id    = it->chunk_id;
        pkt.file_offset = it->offset;

        if (hdr.payload_size > 0) {
            pkt.payload.resize(hdr.payload_size);
            if (!f.read(reinterpret_cast<char*>(pkt.payload.data()),
                        static_cast<std::streamsize>(hdr.payload_size)))
                continue;
        }
        result.push_back(std::move(pkt));
    }
    return result;
}

bool EgoLogReader::verify_packet_count(uint64_t& actual_count) const {
    actual_count = 0;
    scan([&actual_count](const RawPacket&) { ++actual_count; return true; });
    return (manifest_.packet_count == 0) || (actual_count == manifest_.packet_count);
}

} // namespace ego_offline::load
