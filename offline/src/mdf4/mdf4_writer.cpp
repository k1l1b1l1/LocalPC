#include "ego_offline/mdf4/mdf4_writer.hpp"
#include "ego_offline/mdf4/mdf4_channel_builder.hpp"
#include "ego_offline/mdf4/mdf4_lib_adapter.hpp"

namespace ego_offline::mdf4 {

Mdf4Writer::Mdf4Writer(const Config& cfg) : cfg_(cfg) {}

void Mdf4Writer::prepare(
    const std::vector<align::AlignedSample>&   samples,
    const std::vector<SceneGeometry>&          scene_geom,
    const std::vector<scene::StepMetrics>&     step_metrics,
    const load::SessionBundle&                 meta,
    ns_t                                       t0_ns)
{
    groups_.clear();
    t0_ns_ = t0_ns;
    meta_  = meta;

    if(samples.empty()) return;

    auto time_ax = [&]{
        std::vector<double> t;
        t.reserve(samples.size());
        for(const auto& s : samples)
            t.push_back(static_cast<double>(s.t_ns - t0_ns) / 1e9);
        return t;
    }();

    auto mk_group = [&](std::string name,
                        std::vector<ChannelDesc> chs) -> ChannelGroup {
        ChannelGroup g;
        g.name       = std::move(name);
        g.time_axis_s = time_ax;
        g.channels   = std::move(chs);
        return g;
    };

    // MF-02: build group structures per §5.1
    groups_.push_back(mk_group("Ego.GPS", {
        {"Ego.GPS","Ego.GPS.latitude_deg",  "deg",  "float64", samples.size()},
        {"Ego.GPS","Ego.GPS.longitude_deg", "deg",  "float64", samples.size()},
        {"Ego.GPS","Ego.GPS.altitude_m",    "m",    "float64", samples.size()},
        {"Ego.GPS","Ego.GPS.speed_mps",     "m/s",  "float32", samples.size()},
        {"Ego.GPS","Ego.GPS.heading_deg",   "deg",  "float32", samples.size()},
        {"Ego.GPS","Ego.GPS.fix_quality",   "",     "uint8",   samples.size()},
    }));

    groups_.push_back(mk_group("Ego.IMU", {
        {"Ego.IMU","Ego.IMU.acc_x_mps2",  "m/s²","float32",samples.size()},
        {"Ego.IMU","Ego.IMU.acc_y_mps2",  "m/s²","float32",samples.size()},
        {"Ego.IMU","Ego.IMU.acc_z_mps2",  "m/s²","float32",samples.size()},
        {"Ego.IMU","Ego.IMU.gyro_x_rads", "rad/s","float32",samples.size()},
        {"Ego.IMU","Ego.IMU.gyro_y_rads", "rad/s","float32",samples.size()},
        {"Ego.IMU","Ego.IMU.gyro_z_rads", "rad/s","float32",samples.size()},
    }));

    groups_.push_back(mk_group("Ego.Trajectory", {
        {"Ego.Trajectory","Ego.Trajectory.latitude_deg",  "deg",  "float64",samples.size()},
        {"Ego.Trajectory","Ego.Trajectory.longitude_deg", "deg",  "float64",samples.size()},
        {"Ego.Trajectory","Ego.Trajectory.roll_deg",      "deg",  "float32",samples.size()},
        {"Ego.Trajectory","Ego.Trajectory.pitch_deg",     "deg",  "float32",samples.size()},
        {"Ego.Trajectory","Ego.Trajectory.yaw_deg",       "deg",  "float32",samples.size()},
    }));

    groups_.push_back(mk_group("Source.GPS", {
        {"Source.GPS","Source.GPS.latitude_deg",  "deg","float64",samples.size()},
        {"Source.GPS","Source.GPS.longitude_deg", "deg","float64",samples.size()},
        {"Source.GPS","Source.GPS.speed_mps",     "m/s","float32",samples.size()},
        {"Source.GPS","Source.GPS.valid",          "",  "uint8",  samples.size()},
    }));

    if(!scene_geom.empty()){
        std::vector<double> sc_time;
        sc_time.reserve(scene_geom.size());
        for(const auto& sg : scene_geom)
            sc_time.push_back(static_cast<double>(sg.ts_ns - t0_ns) / 1e9);

        ChannelGroup sg;
        sg.name        = "Scene";
        sg.time_axis_s = std::move(sc_time);
        sg.channels    = {
            {"Scene","Scene.range_m",          "m",  "float64",scene_geom.size()},
            {"Scene","Scene.azimuth_deg",       "deg","float64",scene_geom.size()},
            {"Scene","Scene.closing_speed_mps", "m/s","float64",scene_geom.size()},
        };
        groups_.push_back(std::move(sg));
    }

    if(!step_metrics.empty()){
        std::vector<double> sm_time;
        sm_time.reserve(step_metrics.size());
        for(const auto& sm : step_metrics)
            sm_time.push_back(static_cast<double>(sm.t_ns - t0_ns) / 1e9);

        ChannelGroup sm;
        sm.name        = "Scene.Metrics";
        sm.time_axis_s = std::move(sm_time);
        sm.channels    = {
            {"Scene.Metrics","Scene.Metrics.ego_speed_mps","m/s","float64",step_metrics.size()},
            {"Scene.Metrics","Scene.Metrics.src_speed_mps","m/s","float64",step_metrics.size()},
        };
        groups_.push_back(std::move(sm));
    }

    // MF-01: fill actual data arrays
    fill_channel_data(groups_, samples, scene_geom, step_metrics, t0_ns);
}

bool Mdf4Writer::write(const std::filesystem::path& out_path) {
    return write_mdf4(out_path, groups_, meta_, t0_ns_);
}

bool Mdf4Writer::validate(const std::filesystem::path& mdf_path) const {
    if(!validate_mdf4(mdf_path, groups_)) return false;
    write_sha256_sidecar(mdf_path);
    return true;
}

} // namespace ego_offline::mdf4
