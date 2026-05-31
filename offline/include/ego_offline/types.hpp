#pragma once
// Common domain types used across all pipeline modules

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace ego_offline {

// ── Time ─────────────────────────────────────────────────────────────────────
using ns_t = int64_t;  // nanoseconds on the master clock (ego)
using seq_t = uint32_t;

static constexpr ns_t kNsPerSec  = 1'000'000'000LL;
static constexpr ns_t kNsPerMs   = 1'000'000LL;
static constexpr ns_t kNsPerUs   = 1'000LL;

// ── Streams ──────────────────────────────────────────────────────────────────
enum class Stream { ego, source, both };

struct TimeRange {
    ns_t   t_start_ns = 0;
    ns_t   t_end_ns   = 0;
    Stream stream     = Stream::both;
    std::string reason;

    [[nodiscard]] double duration_s() const {
        return static_cast<double>(t_end_ns - t_start_ns) / 1e9;
    }
    [[nodiscard]] bool valid() const {
        return t_end_ns > t_start_ns;
    }
};

// ── GPS fix (ego or source) ───────────────────────────────────────────────────
struct GnssPoint {
    ns_t   ts_ns          = 0;
    double latitude_deg   = 0.0;
    double longitude_deg  = 0.0;
    double altitude_m     = 0.0;
    float  speed_mps      = 0.0f;
    float  heading_deg    = 0.0f;
    uint8_t fix_quality   = 0;
    uint8_t satellites    = 0;
    float  hdop           = 99.9f;
};

// ── IMU sample ────────────────────────────────────────────────────────────────
struct ImuSample {
    ns_t  ts_ns = 0;
    float acc_x_mps2  = 0.f;
    float acc_y_mps2  = 0.f;
    float acc_z_mps2  = 0.f;
    float gyro_x_rads = 0.f;
    float gyro_y_rads = 0.f;
    float gyro_z_rads = 0.f;
};

// ── Trajectory point ──────────────────────────────────────────────────────────
struct TrajectoryPoint {
    ns_t   ts_ns         = 0;
    double latitude_deg  = 0.0;
    double longitude_deg = 0.0;
    double altitude_m    = 0.0;
    float  vel_n_mps     = 0.f;
    float  vel_e_mps     = 0.f;
    float  vel_d_mps     = 0.f;
    float  roll_deg      = 0.f;
    float  pitch_deg     = 0.f;
    float  yaw_deg       = 0.f;
    uint8_t solution_type = 0;
};

// ── Source event ──────────────────────────────────────────────────────────────
enum class SourceEventType : uint16_t {
    kUnknown       = 0,
    kSirenOn       = 1,
    kSirenOff      = 2,
    kModeChange    = 3,
    kUserMarker    = 4,
    kScenarioStart = 5,
    kScenarioStop  = 6,
};

struct SourceEvent {
    ns_t            ts_ns      = 0;  // in source clock (before sync)
    ns_t            ts_ego_ns  = 0;  // after sync to ego clock
    uint64_t        event_id   = 0;
    SourceEventType event_type = SourceEventType::kUnknown;
    uint16_t        flags      = 0;
    ns_t            duration_ns = 0;
};

// ── Scene geometry ────────────────────────────────────────────────────────────
struct SceneGeometry {
    ns_t   ts_ns              = 0;
    double range_m            = 0.0;
    double azimuth_deg        = 0.0;  // 0° = ego nose, CW
    double closing_speed_mps  = 0.0;
    double ego_lat_deg        = 0.0;
    double ego_lon_deg        = 0.0;
    double src_lat_deg        = 0.0;
    double src_lon_deg        = 0.0;
    bool   valid              = false;
};

// ── Segment class ─────────────────────────────────────────────────────────────
enum class SegmentClass {
    approach,
    pass_by,
    recede,
    stationary,
    unknown,
};

inline const char* to_string(SegmentClass c) {
    switch (c) {
        case SegmentClass::approach:    return "approach";
        case SegmentClass::pass_by:     return "pass_by";
        case SegmentClass::recede:      return "recede";
        case SegmentClass::stationary:  return "stationary";
        default:                        return "unknown";
    }
}

// ── Pipeline error codes ──────────────────────────────────────────────────────
enum class ExitCode : int {
    success           = 0,
    validation_error  = 1,
    sync_error        = 2,
    internal_error    = 3,
    upload_failed     = 4,
    upload_blocked    = 5,
};

} // namespace ego_offline
