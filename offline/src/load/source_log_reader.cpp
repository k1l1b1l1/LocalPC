#include "ego_offline/load/source_log_reader.hpp"
#include "ego_offline/json_reader.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

namespace ego_offline::load {

SourceManifest SourceManifest::from_json(const std::filesystem::path& path) {
    auto j = json_load(path.string());
    SourceManifest m;
    m.session_id   = j.value<std::string>("session_id",  "");
    m.first_ts_ns  = j.value<int64_t>("first_ts_ns",  0);
    m.last_ts_ns   = j.value<int64_t>("last_ts_ns",   0);
    m.packet_count = j.value<uint64_t>("packet_count", 0ull);

    auto base_dir = path.parent_path();
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
                if (fname.empty()) fname = cj.value<std::string>("filename", "source.bin");
                c.path = base_dir / fname;
            }
            m.chunks.push_back(c);
        }
    } else {
        ChunkInfo c; c.chunk_id = 0;
        c.path = base_dir / "source.bin";
        m.chunks.push_back(c);
    }
    return m;
}

SourceManifest SourceManifest::single_file(const std::filesystem::path& bin_path) {
    SourceManifest m;
    ChunkInfo c; c.chunk_id = 0; c.path = bin_path;
    m.chunks.push_back(c);
    return m;
}

SourceLogReader::SourceLogReader(const SourceManifest& manifest)
    : manifest_(manifest) {}

void SourceLogReader::scan(std::function<bool(const RawPacket&)> cb) const {
    for (const auto& chunk : manifest_.chunks) {
        std::ifstream f(chunk.path, std::ios::binary);
        if (!f) continue;
        uint64_t file_offset = 0;
        while (f) {
            PacketHeader hdr{};
            if (!f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr))) break;
            file_offset += sizeof(hdr);
            if (hdr.magic != kMagic) break;
            if (hdr.type < 101 || hdr.type > 199) {
                if (hdr.payload_size > 0)
                    f.seekg(static_cast<std::streamoff>(hdr.payload_size), std::ios::cur);
                file_offset += hdr.payload_size;
                continue;
            }
            RawPacket pkt;
            pkt.header      = hdr;
            pkt.chunk_id    = chunk.chunk_id;
            pkt.file_offset = file_offset - sizeof(hdr);
            if (hdr.payload_size > 0) {
                pkt.payload.resize(hdr.payload_size);
                if (!f.read(reinterpret_cast<char*>(pkt.payload.data()),
                            static_cast<std::streamsize>(hdr.payload_size))) break;
                file_offset += hdr.payload_size;
            }
            if (!cb(pkt)) return;
        }
    }
}

void SourceLogReader::build_index(const std::filesystem::path& index_path) {
    index_.clear();
    if (!index_path.empty() && std::filesystem::exists(index_path)) {
        std::ifstream f(index_path);
        std::string line;
        std::getline(f, line);
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string tok; IndexEntry e; int col = 0;
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
              [](const IndexEntry& a, const IndexEntry& b){ return a.ts_ns < b.ts_ns; });
    index_built_ = true;
}

bool SourceLogReader::verify_packet_count(uint64_t& actual_count) const {
    actual_count = 0;
    scan([&actual_count](const RawPacket&) { ++actual_count; return true; });
    return (manifest_.packet_count == 0) || (actual_count == manifest_.packet_count);
}

} // namespace ego_offline::load
