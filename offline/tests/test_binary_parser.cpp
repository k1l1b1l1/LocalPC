// IT-01: binary_parser unit tests
// Tests: good packet, bad magic, bad CRC, truncated payload

#include "ego_offline/parse/binary_parser.hpp"
#include "ego_offline/checksum.hpp"
#include "ego_offline/protocol.hpp"

#include <cassert>
#include <iostream>
#include <vector>
#include <cstring>

using namespace ego_offline;
using namespace ego_offline::parse;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::vector<uint8_t> make_packet(uint16_t type, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> buf(kHeaderSize + payload.size());
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf.data());
    hdr->magic            = kMagic;
    hdr->protocol_version = kProtocolVersion;
    hdr->payload_version  = 1;
    hdr->type             = type;
    hdr->payload_size     = static_cast<uint32_t>(payload.size());
    hdr->ts_ns            = 1'000'000'000ULL;
    hdr->seq              = 42;
    hdr->status_flags     = 0;
    hdr->checksum = payload.empty() ? 0 : crc32(payload.data(), payload.size());
    std::memcpy(buf.data() + kHeaderSize, payload.data(), payload.size());
    return buf;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

static void test_good_packet() {
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03, 0x04};
    auto buf = make_packet(static_cast<uint16_t>(EgoPacketType::kGpsFix), payload);

    ParseResult res;
    size_t consumed = BinaryParser::parse_from_buffer(buf.data(), buf.size(), res);

    assert(res.ok() && "Expected ok");
    assert(consumed == buf.size() && "Consumed all bytes");
    assert(res.header.magic == kMagic);
    assert(res.header.type  == static_cast<uint16_t>(EgoPacketType::kGpsFix));
    assert(res.payload == payload);
    std::cout << "[PASS] test_good_packet\n";
}

static void test_bad_magic() {
    auto buf = make_packet(1, {0xAA, 0xBB});
    buf[0] ^= 0xFF; // corrupt magic

    ParseResult res;
    size_t consumed = BinaryParser::parse_from_buffer(buf.data(), buf.size(), res);
    assert(!res.ok());
    assert(res.status == ParseStatus::bad_magic);
    std::cout << "[PASS] test_bad_magic\n";
}

static void test_crc_fail() {
    std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
    auto buf = make_packet(1, payload);
    // Corrupt payload byte
    buf[kHeaderSize] ^= 0xFF;

    ParseResult res;
    BinaryParser::parse_from_buffer(buf.data(), buf.size(), res);
    assert(res.status == ParseStatus::crc_fail);
    std::cout << "[PASS] test_crc_fail\n";
}

static void test_truncated() {
    auto buf = make_packet(1, {0x01, 0x02, 0x03, 0x04});
    buf.resize(buf.size() - 2); // truncate

    ParseResult res;
    size_t consumed = BinaryParser::parse_from_buffer(buf.data(), buf.size(), res);
    assert(!res.ok());
    assert(res.status == ParseStatus::truncated);
    std::cout << "[PASS] test_truncated\n";
}

static void test_empty_payload() {
    auto buf = make_packet(static_cast<uint16_t>(EgoPacketType::kSessionMeta), {});

    ParseResult res;
    size_t consumed = BinaryParser::parse_from_buffer(buf.data(), buf.size(), res);
    assert(res.ok());
    assert(res.payload.empty());
    std::cout << "[PASS] test_empty_payload\n";
}

static void test_size_exceeded() {
    // Craft packet claiming massive payload
    std::vector<uint8_t> buf(kHeaderSize);
    PacketHeader* hdr = reinterpret_cast<PacketHeader*>(buf.data());
    hdr->magic            = kMagic;
    hdr->protocol_version = kProtocolVersion;
    hdr->payload_version  = 1;
    hdr->type             = 1;
    hdr->payload_size     = 0xFFFF; // exceeds limit
    hdr->checksum         = 0;

    ParseResult res;
    BinaryParser::parse_from_buffer(buf.data(), buf.size(), res);
    assert(res.status == ParseStatus::size_exceeded);
    std::cout << "[PASS] test_size_exceeded\n";
}

int main() {
    test_good_packet();
    test_bad_magic();
    test_crc_fail();
    test_truncated();
    test_empty_payload();
    test_size_exceeded();
    std::cout << "\nAll binary_parser tests passed.\n";
    return 0;
}
