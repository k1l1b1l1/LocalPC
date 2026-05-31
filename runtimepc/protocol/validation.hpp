#pragma once

#include <string>
#include <vector>

#include "checksum.hpp"
#include "types.hpp"

namespace ego::protocol {

struct ValidationResult {
    bool accepted = false;
    std::string reason = "unknown";
};

inline ValidationResult ValidatePacketVerbose(const PacketHeader& header,
                                              const std::vector<std::uint8_t>& payload,
                                              std::uint32_t max_payload_bytes = 8192U) {
    if (header.magic != kPacketMagic) {
        return {false, "invalid magic"};
    }
    if (header.protocol_version != kProtocolVersion) {
        return {false, "unsupported protocol_version"};
    }
    if (header.payload_version != kPayloadVersion) {
        return {false, "unsupported payload_version"};
    }
    if (!IsKnownPacketType(header.type)) {
        return {false, "unknown packet type"};
    }
    if (header.payload_size > max_payload_bytes) {
        return {false, "payload_size exceeds limit"};
    }
    if (header.payload_size != payload.size()) {
        return {false, "payload_size mismatch"};
    }
    if (header.checksum != Crc32(payload)) {
        return {false, "checksum mismatch"};
    }
    return {true, "ok"};
}

inline bool ValidatePacket(const PacketHeader& header,
                           const std::vector<std::uint8_t>& payload,
                           std::uint32_t max_payload_bytes = 8192U) {
    return ValidatePacketVerbose(header, payload, max_payload_bytes).accepted;
}

}  // namespace ego::protocol
