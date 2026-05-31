#include "ego_offline/mdf4/mdf4_channel_builder.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace ego_offline::mdf4 {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Find a ChannelGroup by name; throws if not found.
ChannelGroup& find_group(std::vector<ChannelGroup>& groups, const std::string& name) {
    for (auto& g : groups)
        if (g.name == name) return g;
    throw std::runtime_error("fill_channel_data: group not found: " + name);
}

// Find a ChannelDesc by name within a group; throws if not found.
ChannelDesc& find_channel(ChannelGroup& g, const std::string& name) {
    for (auto& ch : g.channels)
        if (ch.name == name) return ch;
    throw std::runtime_error("fill_channel_data: channel not found: " + name);
}

void reserve_all(ChannelGroup& g, size_t n) {
    g.time_axis_s.reserve(n);
    for (auto& ch : g.channels)
        ch.data.reserve(n);
}

} // namespace

void fill_channel_data(
    std::vector<ChannelGroup>&               groups,
    const std::vector<align::AlignedSample>& samples,
    const std::vector<SceneGeometry>&        scene_geom,
    const std::vector<scene::StepMetrics>&   step_metrics,
    ns_t                                     t0_ns)
{
    const size_t N = samples.size();

    // ── Ego.GPS ───────────────────────────────────────────────────────────────
    {
        auto& g = find_group(groups, "Ego.GPS");
        g.time_axis_s.clear();
        for (auto& ch : g.channels) ch.data.clear();
        reserve_all(g, N);

        auto& lat  = find_channel(g, "Ego.GPS.latitude_deg");
        auto& lon  = find_channel(g, "Ego.GPS.longitude_deg");
        auto& alt  = find_channel(g, "Ego.GPS.altitude_m");
        auto& spd  = find_channel(g, "Ego.GPS.speed_mps");
        auto& hdg  = find_channel(g, "Ego.GPS.heading_deg");
        auto& fix  = find_channel(g, "Ego.GPS.fix_quality");

        for (const auto& s : samples) {
            g.time_axis_s.push_back(static_cast<double>(s.t_ns - t0_ns) / 1e9);
            lat.data.push_back(s.ego_lat);
            lon.data.push_back(s.ego_lon);
            alt.data.push_back(s.ego_alt);
            spd.data.push_back(static_cast<double>(s.ego_speed));
            hdg.data.push_back(static_cast<double>(s.ego_heading));
            fix.data.push_back(static_cast<double>(s.ego_fix));
        }
    }

    // ── Ego.IMU ───────────────────────────────────────────────────────────────
    {
        auto& g = find_group(groups, "Ego.IMU");
        g.time_axis_s.clear();
        for (auto& ch : g.channels) ch.data.clear();
        reserve_all(g, N);

        auto& ax = find_channel(g, "Ego.IMU.acc_x_mps2");
        auto& ay = find_channel(g, "Ego.IMU.acc_y_mps2");
        auto& az = find_channel(g, "Ego.IMU.acc_z_mps2");
        auto& gx = find_channel(g, "Ego.IMU.gyro_x_rads");
        auto& gy = find_channel(g, "Ego.IMU.gyro_y_rads");
        auto& gz = find_channel(g, "Ego.IMU.gyro_z_rads");

        for (const auto& s : samples) {
            g.time_axis_s.push_back(static_cast<double>(s.t_ns - t0_ns) / 1e9);
            double v = s.imu_valid ? 1.0 : 0.0; (void)v;
            ax.data.push_back(s.imu_valid ? static_cast<double>(s.acc_x) : kNaN);
            ay.data.push_back(s.imu_valid ? static_cast<double>(s.acc_y) : kNaN);
            az.data.push_back(s.imu_valid ? static_cast<double>(s.acc_z) : kNaN);
            gx.data.push_back(s.imu_valid ? static_cast<double>(s.gyr_x) : kNaN);
            gy.data.push_back(s.imu_valid ? static_cast<double>(s.gyr_y) : kNaN);
            gz.data.push_back(s.imu_valid ? static_cast<double>(s.gyr_z) : kNaN);
        }
    }

    // ── Ego.Trajectory ────────────────────────────────────────────────────────
    {
        auto& g = find_group(groups, "Ego.Trajectory");
        g.time_axis_s.clear();
        for (auto& ch : g.channels) ch.data.clear();
        reserve_all(g, N);

        auto& lat   = find_channel(g, "Ego.Trajectory.latitude_deg");
        auto& lon   = find_channel(g, "Ego.Trajectory.longitude_deg");
        auto& roll  = find_channel(g, "Ego.Trajectory.roll_deg");
        auto& pitch = find_channel(g, "Ego.Trajectory.pitch_deg");
        auto& yaw   = find_channel(g, "Ego.Trajectory.yaw_deg");

        for (const auto& s : samples) {
            g.time_axis_s.push_back(static_cast<double>(s.t_ns - t0_ns) / 1e9);
            lat.data.push_back(s.traj_valid ? s.traj_lat : kNaN);
            lon.data.push_back(s.traj_valid ? s.traj_lon : kNaN);
            roll.data.push_back(s.traj_valid  ? static_cast<double>(s.roll)  : kNaN);
            pitch.data.push_back(s.traj_valid ? static_cast<double>(s.pitch) : kNaN);
            yaw.data.push_back(s.traj_valid   ? static_cast<double>(s.yaw)   : kNaN);
        }
    }

    // ── Source.GPS ────────────────────────────────────────────────────────────
    {
        auto& g = find_group(groups, "Source.GPS");
        g.time_axis_s.clear();
        for (auto& ch : g.channels) ch.data.clear();
        reserve_all(g, N);

        auto& lat   = find_channel(g, "Source.GPS.latitude_deg");
        auto& lon   = find_channel(g, "Source.GPS.longitude_deg");
        auto& spd   = find_channel(g, "Source.GPS.speed_mps");
        auto& valid = find_channel(g, "Source.GPS.valid");

        for (const auto& s : samples) {
            g.time_axis_s.push_back(static_cast<double>(s.t_ns - t0_ns) / 1e9);
            lat.data.push_back(s.src_valid ? s.src_lat : kNaN);
            lon.data.push_back(s.src_valid ? s.src_lon : kNaN);
            spd.data.push_back(s.src_valid ? static_cast<double>(s.src_speed) : kNaN);
            valid.data.push_back(s.src_valid ? 1.0 : 0.0);
        }
    }

    // ── Scene ─────────────────────────────────────────────────────────────────
    if (!scene_geom.empty()) {
        auto& g = find_group(groups, "Scene");
        g.time_axis_s.clear();
        for (auto& ch : g.channels) ch.data.clear();
        reserve_all(g, scene_geom.size());

        auto& rng  = find_channel(g, "Scene.range_m");
        auto& az   = find_channel(g, "Scene.azimuth_deg");
        auto& cspd = find_channel(g, "Scene.closing_speed_mps");

        for (const auto& sg : scene_geom) {
            g.time_axis_s.push_back(static_cast<double>(sg.ts_ns - t0_ns) / 1e9);
            rng.data.push_back(sg.valid ? sg.range_m           : kNaN);
            az.data.push_back(sg.valid  ? sg.azimuth_deg       : kNaN);
            cspd.data.push_back(sg.valid ? sg.closing_speed_mps : kNaN);
        }
    }

    // ── Scene.Metrics ─────────────────────────────────────────────────────────
    if (!step_metrics.empty()) {
        auto& g = find_group(groups, "Scene.Metrics");
        g.time_axis_s.clear();
        for (auto& ch : g.channels) ch.data.clear();
        reserve_all(g, step_metrics.size());

        auto& espd = find_channel(g, "Scene.Metrics.ego_speed_mps");
        auto& sspd = find_channel(g, "Scene.Metrics.src_speed_mps");

        for (const auto& sm : step_metrics) {
            g.time_axis_s.push_back(static_cast<double>(sm.t_ns - t0_ns) / 1e9);
            espd.data.push_back(sm.valid ? sm.ego_speed_mps : kNaN);
            sspd.data.push_back(sm.valid ? sm.src_speed_mps : kNaN);
        }
    }

    // Update sample_count for all channels
    for (auto& g : groups)
        for (auto& ch : g.channels)
            ch.sample_count = ch.data.size();
}

} // namespace ego_offline::mdf4
