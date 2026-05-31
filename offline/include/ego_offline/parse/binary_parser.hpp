#pragma once
// PR-01: binary_parser — parse + CRC check per packet

#include "ego_offline/protocol.hpp"
#include "ego_offline/load/ego_log_reader.hpp"

#include <vector>
#include <string>

namespace ego_offline::parse {

enum class ParseStatus {
    ok,
    bad_magic,
    bad_version,
    truncated,
    crc_fail,
    size_exceeded,
};

struct ParseResult {
    ParseStatus              status = ParseStatus::ok;
    PacketHeader             header{};
    std::vector<uint8_t>     payload;
    std::string              error;

    bool ok() const { return status == ParseStatus::ok; }
};

class BinaryParser {
public:
    static constexpr uint32_t kMaxPayloadSize = 8192u;

    // Parse and CRC-validate a raw packet already read from disk.
    static ParseResult validate(const load::RawPacket& raw);

    // Parse from a memory buffer (for unit tests).
    // Returns how many bytes consumed (0 on failure).
    static size_t parse_from_buffer(const uint8_t* data, size_t len, ParseResult& out);
};

} // namespace ego_offline::parse
