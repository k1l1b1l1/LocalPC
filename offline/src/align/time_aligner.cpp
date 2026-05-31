#include "ego_offline/align/time_aligner.hpp"

#include <algorithm>
#include <cmath>

namespace ego_offline::align {

// ── TimeGrid ──────────────────────────────────────────────────────────────────

TimeGrid TimeGrid::build(ns_t t0, ns_t t1, int dt_ms) {
    TimeGrid g;
    g.t0    = t0;
    g.t1    = t1;
    g.dt_ms = dt_ms;
    ns_t dt = static_cast<ns_t>(dt_ms) * kNsPerMs;
    for (ns_t t = t0; t <= t1; t += dt) g.ticks.push_back(t);
    return g;
}

// ── Interpolators ─────────────────────────────────────────────────────────────

GnssPoint interpolate_gnss(const std::vector<GnssPoint>& pts, ns_t t) {
    if (pts.empty()) return {};
    if (pts.size() == 1) return pts[0];

    // Find bracketing pair
    auto it = std::lower_bound(pts.begin(), pts.end(), t,
        [](const GnssPoint& p, ns_t ts) { return p.ts_ns < ts; });

    if (it == pts.end())   return pts.back();
    if (it == pts.begin()) return pts.front();

    const auto& b = *it;
    const auto& a = *std::prev(it);

    if (b.ts_ns == a.ts_ns) return a;
    double alpha = static_cast<double>(t - a.ts_ns) / (b.ts_ns - a.ts_ns);

    GnssPoint r;
    r.ts_ns         = t;
    r.latitude_deg  = a.latitude_deg  + alpha * (b.latitude_deg  - a.latitude_deg);
    r.longitude_deg = a.longitude_deg + alpha * (b.longitude_deg - a.longitude_deg);
    r.altitude_m    = a.altitude_m    + alpha * (b.altitude_m    - a.altitude_m);
    r.speed_mps     = a.speed_mps     + static_cast<float>(alpha) * (b.speed_mps - a.speed_mps);
    r.heading_deg   = a.heading_deg   + static_cast<float>(alpha) * (b.heading_deg - a.heading_deg);
    r.fix_quality   = a.fix_quality; // step-hold
    r.satellites    = a.satellites;
    r.hdop          = a.hdop;
    return r;
}

TrajectoryPoint interpolate_trajectory(const std::vector<TrajectoryPoint>& pts, ns_t t) {
    if (pts.empty()) return {};
    if (pts.size() == 1) return pts[0];

    auto it = std::lower_bound(pts.begin(), pts.end(), t,
        [](const TrajectoryPoint& p, ns_t ts) { return p.ts_ns < ts; });

    if (it == pts.end())   return pts.back();
    if (it == pts.begin()) return pts.front();

    const auto& b = *it;
    const auto& a = *std::prev(it);
    if (b.ts_ns == a.ts_ns) return a;
    double alpha = static_cast<double>(t - a.ts_ns) / (b.ts_ns - a.ts_ns);
    float  af    = static_cast<float>(alpha);

    TrajectoryPoint r;
    r.ts_ns         = t;
    r.latitude_deg  = a.latitude_deg  + alpha * (b.latitude_deg  - a.latitude_deg);
    r.longitude_deg = a.longitude_deg + alpha * (b.longitude_deg - a.longitude_deg);
    r.altitude_m    = a.altitude_m    + alpha * (b.altitude_m    - a.altitude_m);
    r.vel_n_mps     = a.vel_n_mps  + af * (b.vel_n_mps - a.vel_n_mps);
    r.vel_e_mps     = a.vel_e_mps  + af * (b.vel_e_mps - a.vel_e_mps);
    r.vel_d_mps     = a.vel_d_mps  + af * (b.vel_d_mps - a.vel_d_mps);
    r.roll_deg      = a.roll_deg   + af * (b.roll_deg  - a.roll_deg);
    r.pitch_deg     = a.pitch_deg  + af * (b.pitch_deg - a.pitch_deg);
    r.yaw_deg       = a.yaw_deg    + af * (b.yaw_deg   - a.yaw_deg);
    r.solution_type = a.solution_type;
    return r;
}

SourceEvent step_hold_source(const std::vector<SourceEvent>& evs, ns_t t) {
    if (evs.empty()) return {};
    // Return last event at or before t
    SourceEvent last = evs.front();
    for (const auto& e : evs) {
        if (e.ts_ego_ns <= t) last = e;
        else break;
    }
    return last;
}

// ── TimeAligner ───────────────────────────────────────────────────────────────

TimeAligner::TimeAligner(const Config& cfg) : cfg_(cfg) {}

std::vector<AlignedSample> TimeAligner::align(
    const TimeGrid&                     grid,
    const std::vector<GnssPoint>&       ego_gps,
    const std::vector<GnssPoint>&       src_gps,
    const std::vector<ImuSample>&       imu,
    const std::vector<TrajectoryPoint>& traj) const
{
    std::vector<AlignedSample> result;
    result.reserve(grid.ticks.size());

    ns_t join_ns = static_cast<ns_t>(cfg_.join_tolerance_ms) * kNsPerMs;

    // Sort all inputs by time (safety)
    auto gps_sorted = ego_gps;
    std::sort(gps_sorted.begin(), gps_sorted.end(),
              [](const GnssPoint& a, const GnssPoint& b) { return a.ts_ns < b.ts_ns; });

    auto src_sorted = src_gps;
    std::sort(src_sorted.begin(), src_sorted.end(),
              [](const GnssPoint& a, const GnssPoint& b) { return a.ts_ns < b.ts_ns; });

    auto traj_sorted = traj;
    std::sort(traj_sorted.begin(), traj_sorted.end(),
              [](const TrajectoryPoint& a, const TrajectoryPoint& b) { return a.ts_ns < b.ts_ns; });

    // IMU: find nearest within join_ns
    auto find_nearest_imu = [&](ns_t t) -> const ImuSample* {
        if (imu.empty()) return nullptr;
        auto it = std::min_element(imu.begin(), imu.end(),
            [t](const ImuSample& a, const ImuSample& b) {
                return std::abs(a.ts_ns - t) < std::abs(b.ts_ns - t);
            });
        if (std::abs(it->ts_ns - t) <= join_ns) return &(*it);
        return nullptr;
    };

    for (ns_t tick : grid.ticks) {
        AlignedSample s;
        s.t_ns = tick;

        // Ego GPS
        if (!gps_sorted.empty()) {
            auto g = interpolate_gnss(gps_sorted, tick);
            s.ego_lat = g.latitude_deg; s.ego_lon = g.longitude_deg;
            s.ego_alt = g.altitude_m;   s.ego_speed = g.speed_mps;
            s.ego_heading = g.heading_deg; s.ego_fix = g.fix_quality;
        }

        // Source GPS
        if (!src_sorted.empty()) {
            auto g = interpolate_gnss(src_sorted, tick);
            // Only valid if tick is within source range ±5s
            if (!src_sorted.empty()
                && tick >= src_sorted.front().ts_ns - 5*kNsPerSec
                && tick <= src_sorted.back().ts_ns  + 5*kNsPerSec) {
                s.src_lat = g.latitude_deg; s.src_lon = g.longitude_deg;
                s.src_alt = g.altitude_m;   s.src_speed = g.speed_mps;
                s.src_valid = (g.fix_quality > 0);
            }
        }

        // IMU (nearest)
        const ImuSample* im = find_nearest_imu(tick);
        if (im) {
            s.acc_x = im->acc_x_mps2; s.acc_y = im->acc_y_mps2; s.acc_z = im->acc_z_mps2;
            s.gyr_x = im->gyro_x_rads; s.gyr_y = im->gyro_y_rads; s.gyr_z = im->gyro_z_rads;
            s.imu_valid = true;
        }

        // Trajectory
        if (!traj_sorted.empty()) {
            auto tp = interpolate_trajectory(traj_sorted, tick);
            s.traj_lat = tp.latitude_deg; s.traj_lon = tp.longitude_deg;
            s.roll = tp.roll_deg; s.pitch = tp.pitch_deg; s.yaw = tp.yaw_deg;
            s.traj_valid = (tp.solution_type > 0);
        }

        result.push_back(s);
    }
    return result;
}

} // namespace ego_offline::align
