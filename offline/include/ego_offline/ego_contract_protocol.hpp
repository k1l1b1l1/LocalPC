#pragma once
// ego-contract v1.3 frame format (must match ego-contract-main headers).

#include <cstdint>

namespace ego_offline::contract {

static constexpr uint32_t kFrameMagic = 0x314F4745u;  // EGO1 LE
static constexpr uint16_t kProtocolVersion = 2u;
static constexpr size_t kFrameHeaderSize = 72u;

#pragma pack(push, 1)
struct EgoFrameHeader {
    uint32_t magic;
    uint16_t protocol_ver;
    uint16_t header_size;
    uint32_t frame_type;
    uint32_t flags;
    uint64_t session_id_hi;
    uint64_t session_id_lo;
    uint64_t seq;
    uint64_t t0_ns;
    uint64_t t1_ns;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t header_crc32;
    uint32_t reserved0;
};
#pragma pack(pop)

enum class FrameType : uint32_t {
    kSessionStarted = 1,
    kConfigSnapshot = 2,
    kAudioBlock = 100,
    kImuWindow = 101,
    kCanDecoded = 102,
    kCanRaw = 103,
    kTrajectory = 104,
    kGpsFix = 105,
    kSystemStatus = 201,
    kSessionEnded = 900,
};

}  // namespace ego_offline::contract
