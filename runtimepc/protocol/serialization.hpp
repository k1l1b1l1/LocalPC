#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "checksum.hpp"
#include "types.hpp"

namespace ego::protocol {

constexpr std::size_t kPacketHeaderSize = 34U;

struct Packet {
    PacketHeader header;
    std::vector<std::uint8_t> payload;
};

inline void AppendBytes(std::vector<std::uint8_t>& out, const void* src, std::size_t size) {
    const auto* ptr = static_cast<const std::uint8_t*>(src);
    out.insert(out.end(), ptr, ptr + size);
}

template <typename T>
inline T ReadPod(const std::vector<std::uint8_t>& data, std::size_t offset) {
    if (offset + sizeof(T) > data.size()) {
        throw std::runtime_error("decode overflow");
    }
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

inline std::vector<std::uint8_t> SerializeHeader(const PacketHeader& header) {
    std::vector<std::uint8_t> out;
    out.reserve(kPacketHeaderSize);
    AppendBytes(out, &header.magic, sizeof(header.magic));
    AppendBytes(out, &header.protocol_version, sizeof(header.protocol_version));
    AppendBytes(out, &header.payload_version, sizeof(header.payload_version));
    AppendBytes(out, &header.type, sizeof(header.type));
    AppendBytes(out, &header.payload_size, sizeof(header.payload_size));
    AppendBytes(out, &header.ts_ns, sizeof(header.ts_ns));
    AppendBytes(out, &header.seq, sizeof(header.seq));
    AppendBytes(out, &header.status_flags, sizeof(header.status_flags));
    AppendBytes(out, &header.checksum, sizeof(header.checksum));
    return out;
}

inline PacketHeader DeserializeHeader(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() < kPacketHeaderSize) {
        throw std::runtime_error("header too small");
    }

    PacketHeader header{};
    std::size_t offset = 0U;
    header.magic = ReadPod<std::uint32_t>(bytes, offset);
    offset += sizeof(header.magic);
    header.protocol_version = ReadPod<std::uint16_t>(bytes, offset);
    offset += sizeof(header.protocol_version);
    header.payload_version = ReadPod<std::uint16_t>(bytes, offset);
    offset += sizeof(header.payload_version);
    header.type = ReadPod<std::uint16_t>(bytes, offset);
    offset += sizeof(header.type);
    header.payload_size = ReadPod<std::uint32_t>(bytes, offset);
    offset += sizeof(header.payload_size);
    header.ts_ns = ReadPod<std::uint64_t>(bytes, offset);
    offset += sizeof(header.ts_ns);
    header.seq = ReadPod<std::uint32_t>(bytes, offset);
    offset += sizeof(header.seq);
    header.status_flags = ReadPod<std::uint32_t>(bytes, offset);
    offset += sizeof(header.status_flags);
    header.checksum = ReadPod<std::uint32_t>(bytes, offset);
    return header;
}

inline std::vector<std::uint8_t> SerializePacket(PacketHeader header,
                                                 const std::vector<std::uint8_t>& payload) {
    header.payload_size = static_cast<std::uint32_t>(payload.size());
    header.checksum = Crc32(payload);
    std::vector<std::uint8_t> out = SerializeHeader(header);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

inline Packet DeserializePacket(const std::vector<std::uint8_t>& bytes) {
    Packet packet{};
    packet.header = DeserializeHeader(bytes);
    const std::size_t expected_size = kPacketHeaderSize + packet.header.payload_size;
    if (expected_size != bytes.size()) {
        throw std::runtime_error("packet size mismatch");
    }
    packet.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(kPacketHeaderSize), bytes.end());
    return packet;
}

}  // namespace ego::protocol
