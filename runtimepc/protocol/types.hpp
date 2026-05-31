#pragma once

#include <cstdint>
#include <string>

namespace ego::protocol {

using Timestamp = std::uint64_t;
using Sequence = std::uint32_t;
using SessionId = std::string;

constexpr std::uint32_t kPacketMagic = 0x45574F48U;
constexpr std::uint16_t kProtocolVersion = 1U;
constexpr std::uint16_t kPayloadVersion = 1U;

enum class PacketType : std::uint16_t {
    kAudioBlock = 1,
    kCanFrame = 2,
    kImuSample = 3,
    kMotionState = 4,
    kGpsFix = 5,
    kSessionFrame = 6,
    kDiagnostics = 7,
    kSessionMeta = 8
};

enum class StatusFlags : std::uint32_t {
    kNone = 0,
    kTimeValid = 1U << 0,
    kChecksumValid = 1U << 1,
    kOrderValid = 1U << 2,
    kSyncDegraded = 1U << 3,
    kSensorDropped = 1U << 4
};

inline StatusFlags operator|(StatusFlags lhs, StatusFlags rhs) {
    return static_cast<StatusFlags>(
        static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

inline bool HasFlag(StatusFlags value, StatusFlags flag) {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0U;
}

struct PacketHeader {
    std::uint32_t magic = kPacketMagic;
    std::uint16_t protocol_version = kProtocolVersion;
    std::uint16_t payload_version = kPayloadVersion;
    std::uint16_t type = static_cast<std::uint16_t>(PacketType::kDiagnostics);
    std::uint32_t payload_size = 0U;
    std::uint64_t ts_ns = 0U;
    std::uint32_t seq = 0U;
    std::uint32_t status_flags = static_cast<std::uint32_t>(StatusFlags::kNone);
    std::uint32_t checksum = 0U;
};

enum class HealthState {
    kOk,
    kDegraded,
    kFailed
};

inline bool IsKnownPacketType(std::uint16_t type) {
    return type >= static_cast<std::uint16_t>(PacketType::kAudioBlock) &&
           type <= static_cast<std::uint16_t>(PacketType::kSessionMeta);
}

}  // namespace ego::protocol
