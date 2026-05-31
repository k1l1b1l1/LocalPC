#pragma once
// ego_offline -- shared protocol types
// Mirrors prod/common/types.hpp (PacketHeader, PacketType)
// All multi-byte fields are little-endian.
//
// IMPORTANT: type numbers must match prod/common/types.hpp exactly.
// Last sync: audit 30.05.2026 (P0-1 fix)

#include <cstdint>
#include <cstring>

namespace ego_offline {

// ── PacketHeader (34 bytes, LE) ──────────────────────────────────────────────
static constexpr uint32_t kMagic           = 0x45574F48u; // 'EWOH'
static constexpr uint16_t kProtocolVersion = 1u;
static constexpr size_t   kHeaderSize      = 34u;

#pragma pack(push, 1)
struct PacketHeader {
    uint32_t magic;            //  0  must be kMagic
    uint16_t protocol_version; //  4  must be 1
    uint16_t payload_version;  //  6  payload format version
    uint16_t type;             //  8  EgoPacketType
    uint32_t payload_size;     // 10  bytes
    uint64_t ts_ns;            // 14  wall-clock nanoseconds
    uint32_t seq;              // 22  monotonic per-stream counter
    uint32_t status_flags;     // 26  bitmask
    uint32_t checksum;         // 30  CRC32 of payload
};
#pragma pack(pop)
static_assert(sizeof(PacketHeader) == kHeaderSize, "PacketHeader size mismatch");

// ── Ego packet types (1-8) -- MUST match prod/common/types.hpp ───────────────
enum class EgoPacketType : uint16_t {
    kAudioBlock   = 1,  // PCM audio blocks
    kCanFrame     = 2,  // raw CAN frame
    kImuSample    = 3,  // IMU accelerometer + gyro
    kMotionState  = 4,  // vehicle speed, steering, odometry
    kGpsFix       = 5,  // GNSS fix
    kSessionFrame = 6,  // primary aggregated SC589 frame (GPS+IMU+status)
    kDiagnostics  = 7,  // system diagnostics / health
    kSessionMeta  = 8,  // session metadata / markers
};

// ── Source packet types (101-105) ─────────────────────────────────────────────
enum class SourcePacketType : uint16_t {
    kSourceEvent        = 101,
    kSourceGnssFix      = 102,
    kSourceRtkStatus    = 103,
    kSourceScenarioMeta = 104,
    kSourceDiagnostics  = 105,
};

// ── Ego payload structs ───────────────────────────────────────────────────────
#pragma pack(push, 1)

// type 1 -- kAudioBlock
// variable-length: header + int16_t samples[channels][frames]
struct AudioBlockHeader {
    uint8_t  channels;
    uint8_t  bits_per_sample;
    uint32_t sample_rate_hz;
    uint32_t frame_count;
};

// type 2 -- kCanFrame
struct CanFramePayload {
    uint32_t can_id;
    uint8_t  dlc;
    uint8_t  data[8];
    uint8_t  pad[3];
};

// type 3 -- kImuSample
struct ImuPayload {
    float acc_x_mps2;
    float acc_y_mps2;
    float acc_z_mps2;
    float gyro_x_rads;
    float gyro_y_rads;
    float gyro_z_rads;
    float temp_c;
};

// type 4 -- kMotionState
struct MotionStatePayload {
    float    speed_mps;
    float    steering_angle_deg;
    float    yaw_rate_rads;
    float    acceleration_mps2;
    uint8_t  gear;
    uint8_t  brake_active;
    uint16_t reserved;
};

// type 5 -- kGpsFix
struct GpsFixPayload {
    double   latitude_deg;
    double   longitude_deg;
    double   altitude_m;
    float    speed_mps;
    float    heading_deg;
    uint8_t  fix_quality;
    uint8_t  satellites;
    float    hdop;
    uint16_t reserved;
};

// type 6 -- kSessionFrame (primary aggregated SC589 frame)
struct SessionFramePayload {
    // GPS sub-record
    double   gps_lat_deg;
    double   gps_lon_deg;
    double   gps_alt_m;
    float    gps_speed_mps;
    float    gps_heading_deg;
    uint8_t  gps_fix_quality;
    uint8_t  gps_satellites;
    float    gps_hdop;
    // IMU sub-record
    float    imu_acc_x_mps2;
    float    imu_acc_y_mps2;
    float    imu_acc_z_mps2;
    float    imu_gyro_x_rads;
    float    imu_gyro_y_rads;
    float    imu_gyro_z_rads;
    // Status
    uint8_t  record_active;
    uint8_t  sync_locked;
    uint16_t status_word;
    uint32_t frame_index;
};

// type 7 -- kDiagnostics
struct DiagnosticsPayload {
    uint8_t  health_state;
    uint8_t  cpu_load_pct;
    uint8_t  disk_free_pct;
    uint8_t  temperature_c;
    uint32_t uptime_s;
    uint32_t error_flags;
    uint8_t  reserved[4];
};

// type 8 -- kSessionMeta
struct SessionMetaHeader {
    uint8_t  meta_type;
    uint8_t  reserved[3];
    uint32_t payload_offset;
    // followed by uint16_t text_len + char[text_len]
};

// Legacy aliases (kept for tools using old names)
using RtkStatusPayload = DiagnosticsPayload;
struct TrajectoryPayload {
    double  latitude_deg;
    double  longitude_deg;
    double  altitude_m;
    float   velocity_n_mps;
    float   velocity_e_mps;
    float   velocity_d_mps;
    float   roll_deg;
    float   pitch_deg;
    float   yaw_deg;
    uint8_t solution_type;
    uint8_t reserved[3];
};

// ── Source payload structs ────────────────────────────────────────────────────

// type 101 -- kSourceEvent
struct SourceEventPayload {
    uint64_t event_id;
    uint16_t event_type;
    uint16_t flags;
    uint64_t duration_ns;
    uint32_t reserved;
};
static_assert(sizeof(SourceEventPayload) == 24, "SourceEventPayload size mismatch");

// type 102 -- kSourceGnssFix
struct SourceGnssFixPayload {
    double   latitude_deg;
    double   longitude_deg;
    double   altitude_m;
    float    speed_mps;
    float    heading_deg;
    uint8_t  fix_quality;
    uint8_t  satellites;
    float    hdop;
    uint16_t reserved;
};
static_assert(sizeof(SourceGnssFixPayload) == 40, "SourceGnssFixPayload size mismatch");

// type 103 -- kSourceRtkStatus
struct SourceRtkStatusPayload {
    uint8_t  fix_type;
    uint8_t  correction_age_s;
    uint32_t baseline_mm;
    uint16_t solution_status;
    uint64_t reserved;
};
static_assert(sizeof(SourceRtkStatusPayload) == 16, "SourceRtkStatusPayload size mismatch");

// type 105 -- kSourceDiagnostics (fixed header + variable message)
struct SourceDiagnosticsHeader {
    uint8_t  health_state;
    uint8_t  siren_state;
    uint8_t  battery_pct;
    uint8_t  disk_free_pct;
    int16_t  temperature_c;
    uint16_t message_len;
};

#pragma pack(pop)

} // namespace ego_offline
