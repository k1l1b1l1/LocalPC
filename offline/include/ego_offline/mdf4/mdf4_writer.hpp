#pragma once
// MF-01..04 / EX-01..04: MDF4 channel prep + export (ADR-001: native writer)

#include "ego_offline/types.hpp"
#include "ego_offline/config.hpp"
#include "ego_offline/align/time_aligner.hpp"
#include "ego_offline/scene/scene_geometry.hpp"
#include "ego_offline/load/metadata_loader.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ego_offline::mdf4 {

// ── Channel descriptor (MF-01, MF-04) ────────────────────────────────────────
struct ChannelDesc {
    std::string group;    // e.g. "Ego.GPS"
    std::string name;     // e.g. "Ego.GPS.latitude_deg"
    std::string unit;
    std::string data_type; // "float64", "float32", "uint8", "int16"
    size_t      sample_count = 0;

    std::vector<double> data; // MF-01: flat sample array (cast to data_type when writing)
};

// ── Channel group (MF-02) ─────────────────────────────────────────────────────
struct ChannelGroup {
    std::string              name;
    std::vector<ChannelDesc> channels;
    std::vector<double>      time_axis_s; // MF-03: master time
};

// ── MDF4 writer ───────────────────────────────────────────────────────────────
class Mdf4Writer {
public:
    explicit Mdf4Writer(const Config& cfg);

    // MF-01..03: build channel groups from aligned data
    void prepare(
        const std::vector<align::AlignedSample>&   samples,
        const std::vector<SceneGeometry>&   scene_geom,
        const std::vector<scene::StepMetrics>&     step_metrics,
        const load::SessionBundle&                 meta,
        ns_t                                       t0_ns);

    // EX-01..04: write MDF4 v4.1 binary file
    bool write(const std::filesystem::path& out_path);

    // EX-04: re-open and validate
    bool validate(const std::filesystem::path& mdf_path) const;

    const std::vector<ChannelGroup>& groups() const { return groups_; }

private:
    Config                     cfg_;
    std::vector<ChannelGroup>  groups_;
    ns_t                       t0_ns_ = 0;
    load::SessionBundle        meta_;

};

} // namespace ego_offline::mdf4
