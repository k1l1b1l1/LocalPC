#include "ego_offline/parse/binary_parser.hpp"
#include "ego_offline/checksum.hpp"

#include <cstring>

namespace ego_offline::parse {

ParseResult BinaryParser::validate(const load::RawPacket& raw) {
    ParseResult res;
    res.header  = raw.header;
    res.payload = raw.payload;

    if (raw.header.magic != kMagic) {
        res.status = ParseStatus::bad_magic;
        res.error  = "bad magic";
        return res;
    }
    if (raw.header.protocol_version != kProtocolVersion) {
        res.status = ParseStatus::bad_version;
        res.error  = "protocol_version != 1";
        return res;
    }
    if (raw.header.payload_size > kMaxPayloadSize) {
        res.status = ParseStatus::size_exceeded;
        res.error  = "payload_size exceeds limit";
        return res;
    }
    if (raw.payload.size() < raw.header.payload_size) {
        res.status = ParseStatus::truncated;
        res.error  = "truncated payload";
        return res;
    }

    // CRC32 of payload
    if (raw.header.payload_size > 0) {
        uint32_t computed = crc32(raw.payload.data(), raw.header.payload_size);
        if (computed != raw.header.checksum) {
            res.status = ParseStatus::crc_fail;
            res.error  = "CRC32 mismatch";
            return res;
        }
    }

    res.status = ParseStatus::ok;
    return res;
}

size_t BinaryParser::parse_from_buffer(const uint8_t* data, size_t len, ParseResult& out) {
    out = {};
    if (len < kHeaderSize) {
        out.status = ParseStatus::truncated;
        out.error  = "buffer too small for header";
        return 0;
    }

    std::memcpy(&out.header, data, kHeaderSize);

    if (out.header.magic != kMagic) {
        out.status = ParseStatus::bad_magic;
        out.error  = "bad magic";
        return 0;
    }
    if (out.header.protocol_version != kProtocolVersion) {
        out.status = ParseStatus::bad_version;
        out.error  = "bad protocol_version";
        return 0;
    }
    if (out.header.payload_size > kMaxPayloadSize) {
        out.status = ParseStatus::size_exceeded;
        out.error  = "payload_size exceeds limit";
        return 0;
    }

    size_t total = kHeaderSize + out.header.payload_size;
    if (len < total) {
        out.status = ParseStatus::truncated;
        out.error  = "buffer too small for payload";
        return 0;
    }

    out.payload.assign(data + kHeaderSize, data + total);

    if (out.header.payload_size > 0) {
        uint32_t computed = crc32(out.payload.data(), out.header.payload_size);
        if (computed != out.header.checksum) {
            out.status = ParseStatus::crc_fail;
            out.error  = "CRC32 mismatch";
            return 0;
        }
    }

    out.status = ParseStatus::ok;
    return total;
}

} // namespace ego_offline::parse
