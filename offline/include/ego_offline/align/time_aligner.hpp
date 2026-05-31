#pragma once
// AL-01..05: выровненные временные ряды на общей сетке

#include "ego_offline/types.hpp"
#include "ego_offline/config.hpp"
#include "ego_offline/parse/packet_demuxer.hpp"
#include "ego_offline/parse/source_demuxer.hpp"

#include <vector>

namespace ego_offline::align {

// ── Common time grid (AL-02) ──────────────────────────────────────────────────
struct TimeGrid {
    ns_t              t0        = 0;
    ns_t              t1        = 0;
    int               dt_ms     = 10;
    std::vector<ns_t> ticks;   // monotonic ns timestamps

    static TimeGrid build(ns_t t0, ns_t t1, int dt_ms);
};

// ── Aligned sample on grid ────────────────────────────────────────────────────
struct AlignedSample {
    ns_t   t_ns     = 0;  // grid tick

    // Ego GPS (interpolated)
    double ego_lat  = 0.0, ego_lon = 0.0, ego_alt = 0.0;
    float  ego_speed = 0.f, ego_heading = 0.f;
    uint8_t ego_fix = 0;

    // Source GPS (interpolated, already in ego clock after sync)
    double src_lat  = 0.0, src_lon = 0.0, src_alt = 0.0;
    float  src_speed = 0.f;
    bool   src_valid = false;

    // IMU nearest (step interpolation)
    float acc_x = 0.f, acc_y = 0.f, acc_z = 0.f;
    float gyr_x = 0.f, gyr_y = 0.f, gyr_z = 0.f;
    bool  imu_valid = false;

    // Trajectory (interpolated)
    double traj_lat = 0.0, traj_lon = 0.0;
    float  roll = 0.f, pitch = 0.f, yaw = 0.f;
    bool   traj_valid = false;
};

// ── Interpolators ─────────────────────────────────────────────────────────────

// AL-03: GPS/trajectory linear interpolation
GnssPoint      interpolate_gnss(const std::vector<GnssPoint>& pts, ns_t t);
TrajectoryPoint interpolate_trajectory(const std::vector<TrajectoryPoint>& pts, ns_t t);

// AL-04: Source events — step/hold
SourceEvent step_hold_source(const std::vector<SourceEvent>& evs, ns_t t);

// ── Aligner ───────────────────────────────────────────────────────────────────
class TimeAligner {
public:
    explicit TimeAligner(const Config& cfg);

    // AL-01..05: produce aligned samples on the common grid
    std::vector<AlignedSample> align(
        const TimeGrid&                         grid,
        const std::vector<GnssPoint>&           ego_gps,
        const std::vector<GnssPoint>&           src_gps,   // already in ego clock
        const std::vector<ImuSample>&           imu,
        const std::vector<TrajectoryPoint>&     traj) const;

private:
    Config cfg_;
};

} // namespace ego_offline::align
