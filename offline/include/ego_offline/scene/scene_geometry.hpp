#pragma once
// SC-01..06: расчёт параметров сцены ego ↔ источник

#include "ego_offline/types.hpp"
#include "ego_offline/align/time_aligner.hpp"

#include <vector>

namespace ego_offline::scene {

// ── ENU helper ────────────────────────────────────────────────────────────────
struct EnuVector {
    double east_m  = 0.0;
    double north_m = 0.0;
    double up_m    = 0.0;
    double range_m = 0.0;
};

// SC-01: convert lat/lon/alt to ENU relative to reference
EnuVector geodetic_to_enu(
    double ref_lat, double ref_lon, double ref_alt,
    double tgt_lat, double tgt_lon, double tgt_alt);

// SC-02: range in metres
double compute_range(const EnuVector& enu);

// SC-03: azimuth, 0° = ego nose, CW (yaw from trajectory or heading)
double compute_azimuth(const EnuVector& enu, double ego_heading_deg);

// SC-04: closing speed (positive = approaching)
double compute_closing_speed(
    const SceneGeometry& prev, const SceneGeometry& cur, double dt_s);

// ── SC-06: step metrics on grid ───────────────────────────────────────────────
struct StepMetrics {
    ns_t   t_ns              = 0;
    double range_m           = 0.0;
    double azimuth_deg       = 0.0;
    double closing_speed_mps = 0.0;
    double ego_speed_mps     = 0.0;
    double src_speed_mps     = 0.0;
    bool   valid             = false;
};

// ── SceneCalculator ───────────────────────────────────────────────────────────
class SceneCalculator {
public:
    // SC-01..04: compute per aligned-sample geometry
    std::vector<SceneGeometry> compute(
        const std::vector<align::AlignedSample>& samples) const;

    // SC-06: step metrics on the aligned grid
    std::vector<StepMetrics> step_metrics(
        const std::vector<SceneGeometry>& geom, double dt_s) const;
};

} // namespace ego_offline::scene
