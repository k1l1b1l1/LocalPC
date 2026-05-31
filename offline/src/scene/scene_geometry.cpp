#include "ego_offline/scene/scene_geometry.hpp"

#include <cmath>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ego_offline::scene {

namespace {
static constexpr double kPI  = M_PI;
static constexpr double kDeg = kPI / 180.0;
static constexpr double kRad = 180.0 / kPI;
static constexpr double kA   = 6378137.0;        // WGS-84 semi-major axis
static constexpr double kE2  = 6.69437999014e-3; // first eccentricity squared
} // namespace

// ── SC-01: geodetic to ENU ────────────────────────────────────────────────────

EnuVector geodetic_to_enu(double ref_lat, double ref_lon, double ref_alt,
                          double tgt_lat, double tgt_lon, double tgt_alt) {
    auto to_ecef = [](double lat, double lon, double alt,
                      double& x, double& y, double& z) {
        double rl  = lat * kDeg;
        double rl2 = lon * kDeg;
        double N   = kA / std::sqrt(1.0 - kE2 * std::sin(rl) * std::sin(rl));
        x = (N + alt) * std::cos(rl) * std::cos(rl2);
        y = (N + alt) * std::cos(rl) * std::sin(rl2);
        z = (N * (1.0 - kE2) + alt) * std::sin(rl);
    };

    double rx, ry, rz, tx, ty, tz;
    to_ecef(ref_lat, ref_lon, ref_alt, rx, ry, rz);
    to_ecef(tgt_lat, tgt_lon, tgt_alt, tx, ty, tz);

    double dx = tx - rx, dy = ty - ry, dz = tz - rz;

    double rl  = ref_lat * kDeg;
    double rl2 = ref_lon * kDeg;
    double sl = std::sin(rl),  cl = std::cos(rl);
    double sl2 = std::sin(rl2), cl2 = std::cos(rl2);

    EnuVector enu;
    enu.east_m  = -sl2 * dx + cl2 * dy;
    enu.north_m =  -sl*cl2*dx - sl*sl2*dy + cl*dz;
    enu.up_m    =   cl*cl2*dx + cl*sl2*dy + sl*dz;
    enu.range_m = std::sqrt(enu.east_m*enu.east_m + enu.north_m*enu.north_m);
    return enu;
}

// ── SC-02 ──────────────────────────────────────────────────────────────────────

double compute_range(const EnuVector& enu) {
    return enu.range_m;
}

// ── SC-03: azimuth (ego nose = 0, CW) ────────────────────────────────────────

double compute_azimuth(const EnuVector& enu, double ego_heading_deg) {
    // Bearing from ego to source in geographic frame (0=North, CW)
    double bearing = std::atan2(enu.east_m, enu.north_m) * kRad;
    // Convert to ego-nose frame
    double az = bearing - ego_heading_deg;
    // Normalize to [0, 360)
    az = std::fmod(az, 360.0);
    if (az < 0) az += 360.0;
    return az;
}

// ── SC-04: closing speed ──────────────────────────────────────────────────────

double compute_closing_speed(const SceneGeometry& prev, const SceneGeometry& cur, double dt_s) {
    if (dt_s < 1e-9) return 0.0;
    return (prev.range_m - cur.range_m) / dt_s; // positive = approaching
}

// ── SceneCalculator ───────────────────────────────────────────────────────────

std::vector<SceneGeometry> SceneCalculator::compute(
    const std::vector<align::AlignedSample>& samples) const
{
    std::vector<SceneGeometry> out;
    out.reserve(samples.size());

    SceneGeometry prev;
    bool has_prev = false;

    for (const auto& s : samples) {
        SceneGeometry g;
        g.ts_ns     = s.t_ns;
        g.ego_lat_deg = s.ego_lat;
        g.ego_lon_deg = s.ego_lon;
        g.src_lat_deg = s.src_lat;
        g.src_lon_deg = s.src_lon;

        if (s.src_valid && s.ego_fix > 0) {
            EnuVector enu = geodetic_to_enu(
                s.ego_lat, s.ego_lon, s.ego_alt,
                s.src_lat, s.src_lon, s.src_alt);

            g.range_m    = compute_range(enu);
            g.azimuth_deg = compute_azimuth(enu, s.ego_heading);
            g.valid      = true;

            if (has_prev && prev.valid) {
                double dt = static_cast<double>(g.ts_ns - prev.ts_ns) / 1e9;
                g.closing_speed_mps = compute_closing_speed(prev, g, dt);
            }
        }
        out.push_back(g);
        if (g.valid) { prev = g; has_prev = true; }
    }
    return out;
}

std::vector<StepMetrics> SceneCalculator::step_metrics(
    const std::vector<SceneGeometry>& geom, double dt_s) const
{
    std::vector<StepMetrics> out;
    out.reserve(geom.size());
    for (const auto& g : geom) {
        StepMetrics m;
        m.t_ns              = g.ts_ns;
        m.range_m           = g.range_m;
        m.azimuth_deg       = g.azimuth_deg;
        m.closing_speed_mps = g.closing_speed_mps;
        m.valid             = g.valid;
        out.push_back(m);
    }
    return out;
}

} // namespace ego_offline::scene
