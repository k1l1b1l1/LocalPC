#ifndef EGO_PROTOCOL_PACKETS_HPP
#define EGO_PROTOCOL_PACKETS_HPP

#include <cstdint>
#include <cstddef>

namespace ego::protocol::v1 {

static constexpr uint32_t EGO_FRAME_MAGIC = 0x314F4745u; // 'EGO1' little-endian
static constexpr uint16_t EGO_PROTOCOL_VERSION = 2u;
static constexpr uint16_t EGO_FRAME_HEADER_SIZE = 72u;

// Data TCP payload type. Values match proto/ego/v1/ego_common.proto.
enum class FramePayloadType : uint32_t {
    UNSPECIFIED = 0,
    SESSION_STARTED = 1,
    CONFIG_SNAPSHOT = 2,

    AUDIO_BLOCK = 100,
    IMU_WINDOW = 101,
    CAN_DECODED_VALUE = 102,
    CAN_RAW_FRAME = 103,
    TRAJECTORY_POINT = 104,
    GPS_FIX = 105,

    TIME_STATUS = 200,
    SYSTEM_STATUS = 201,
    IMU_CALIBRATION_EVENT = 202,
    MARKER_EVENT = 203,

    SESSION_ENDED = 900,
};

enum class FrameFlags : uint32_t {
    NONE = 0,
    PAYLOAD_PROTOBUF = 1u << 0,
    PAYLOAD_BINARY = 1u << 1,
    PAYLOAD_COMPRESSED = 1u << 2,
    PAYLOAD_KEYFRAME = 1u << 3,
};

inline constexpr uint32_t to_u32(FramePayloadType v) {
    return static_cast<uint32_t>(v);
}

inline constexpr uint32_t to_u32(FrameFlags v) {
    return static_cast<uint32_t>(v);
}

#pragma pack(push, 1)

// Fixed header for every Data TCP frame and for every record in ego.bin.
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

// Optional fixed control envelope for Control TCP when protobuf request/response
// messages are length-prefixed. Payload is serialized ControlRequest or ControlResponse.
struct EgoControlMessageHeader {
    uint32_t magic;          // 'EGOC' little-endian = 0x434F4745
    uint16_t protocol_ver;
    uint16_t header_size;
    uint64_t seq;
    uint32_t payload_size;
    uint32_t payload_crc32;
    uint32_t header_crc32;
    uint32_t reserved0;
};

static constexpr uint32_t EGO_CONTROL_MAGIC = 0x434F4745u; // 'EGOC' little-endian
static constexpr uint16_t EGO_CONTROL_HEADER_SIZE = sizeof(EgoControlMessageHeader);

// Production binary audio payload. It is used inside EgoFrameHeader payload when
// frame_type == AUDIO_BLOCK and FrameFlags::PAYLOAD_BINARY is set.
// The PCM bytes start immediately after AudioBlockBinaryHeader.
struct AudioBlockBinaryHeader {
    uint64_t audio_block_id;
    uint64_t t0_ns;
    uint64_t t1_ns;

    uint32_t sample_rate_hz;
    uint16_t channels_count;
    uint16_t bytes_per_sample;

    uint32_t frames_count;
    uint32_t layout;          // 0 = interleaved, 1 = planar

    uint32_t data_size;
    uint32_t reserved0;
};

struct ImuWindowPacket {
    uint64_t window_id;
    uint64_t t0_ns;
    uint64_t t1_ns;

    uint16_t sample_count;
    uint16_t flags;

    float accel_mean_x_mps2;
    float accel_mean_y_mps2;
    float accel_mean_z_mps2;

    float gyro_mean_x_rad_s;
    float gyro_mean_y_rad_s;
    float gyro_mean_z_rad_s;

    float delta_velocity_x_mps;
    float delta_velocity_y_mps;
    float delta_velocity_z_mps;

    float delta_angle_x_rad;
    float delta_angle_y_rad;
    float delta_angle_z_rad;
};

struct CanDecodedValuePacket {
    uint64_t t_ns;

    uint32_t value_id;
    uint32_t can_id;

    float value;
    uint32_t raw_value;
    uint32_t flags;

    uint8_t dlc;
    uint8_t reserved0;
    uint8_t reserved1;
    uint8_t reserved2;
};

struct CanRawFramePacket {
    uint64_t t_ns;

    uint32_t can_id;
    uint8_t dlc;
    uint8_t is_extended;
    uint8_t bus_id;
    uint8_t flags;

    uint8_t data[8];
};

struct TrajectoryPointPacket {
    uint64_t t_ns;

    float x_m;
    float y_m;
    float z_m;

    float vx_mps;
    float vy_mps;
    float vz_mps;

    float yaw_rad;
    float pitch_rad;
    float roll_rad;

    float yaw_rate_rad_s;
    float path_s_m;
    float vehicle_speed_mps;

    uint32_t flags;
    uint32_t reserved0;
};

struct GpsFixPacket {
    uint64_t t_ns;

    double lat_deg;
    double lon_deg;
    double alt_m;

    float speed_mps;
    float heading_rad;
    float h_acc_m;
    float v_acc_m;

    uint8_t fix_type;
    uint8_t rtk_status;
    uint8_t satellites;
    uint8_t reserved0;

    uint32_t flags;
};

struct TimeStatusPacket {
    uint64_t t_ns;
    uint64_t monotonic_ns;
    int64_t utc_offset_ns;

    uint32_t time_source;
    uint32_t sync_status;

    float estimated_drift_ppm;
    float sync_error_us;
};

struct SystemStatusPacket {
    uint64_t t_ns;

    uint32_t audio_status;
    uint32_t can_status;
    uint32_t imu_status;
    uint32_t gps_status;
    uint32_t network_status;

    uint32_t audio_overruns;
    uint32_t imu_fifo_overruns;
    uint32_t can_rx_errors;
    uint32_t dropped_frames;

    float cpu_load_arm;
    float cpu_load_sharc0;
    float cpu_load_sharc1;
};

struct ImuCalibrationEventPacket {
    uint64_t t_ns;

    float gyro_bias_x_rad_s;
    float gyro_bias_y_rad_s;
    float gyro_bias_z_rad_s;

    float accel_ref_x_mps2;
    float accel_ref_y_mps2;
    float accel_ref_z_mps2;

    float collect_time_s;
    uint32_t sample_count;
    uint32_t quality_flags;
};

#pragma pack(pop)

static_assert(sizeof(EgoFrameHeader) == 72, "EgoFrameHeader size must be 72 bytes");
static_assert(sizeof(EgoControlMessageHeader) == 32, "EgoControlMessageHeader size must be 32 bytes");
static_assert(sizeof(AudioBlockBinaryHeader) == 48, "AudioBlockBinaryHeader size must be 48 bytes");
static_assert(sizeof(ImuWindowPacket) == 76, "ImuWindowPacket size must be 76 bytes");
static_assert(sizeof(CanDecodedValuePacket) == 32, "CanDecodedValuePacket size must be 32 bytes");
static_assert(sizeof(CanRawFramePacket) == 24, "CanRawFramePacket size must be 24 bytes");
static_assert(sizeof(TrajectoryPointPacket) == 64, "TrajectoryPointPacket size must be 64 bytes");
static_assert(sizeof(GpsFixPacket) == 56, "GpsFixPacket size must be 56 bytes");
static_assert(sizeof(TimeStatusPacket) == 40, "TimeStatusPacket size must be 40 bytes");
static_assert(sizeof(SystemStatusPacket) == 56, "SystemStatusPacket size must be 56 bytes");
static_assert(sizeof(ImuCalibrationEventPacket) == 44, "ImuCalibrationEventPacket size must be 44 bytes");

inline EgoFrameHeader make_frame_header(
    FramePayloadType type,
    uint32_t flags,
    uint64_t session_id_hi,
    uint64_t session_id_lo,
    uint64_t seq,
    uint64_t t0_ns,
    uint64_t t1_ns,
    uint32_t payload_size,
    uint32_t payload_crc32
) {
    EgoFrameHeader h{};
    h.magic = EGO_FRAME_MAGIC;
    h.protocol_ver = EGO_PROTOCOL_VERSION;
    h.header_size = EGO_FRAME_HEADER_SIZE;
    h.frame_type = to_u32(type);
    h.flags = flags;
    h.session_id_hi = session_id_hi;
    h.session_id_lo = session_id_lo;
    h.seq = seq;
    h.t0_ns = t0_ns;
    h.t1_ns = t1_ns;
    h.payload_size = payload_size;
    h.payload_crc32 = payload_crc32;
    h.header_crc32 = 0u;
    h.reserved0 = 0u;
    return h;
}

} // namespace ego::protocol::v1

#endif // EGO_PROTOCOL_PACKETS_HPP
