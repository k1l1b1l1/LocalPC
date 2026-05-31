#pragma once
// FN-01..07: scene_events.json + scene_segments.json

#include "ego_offline/types.hpp"
#include "ego_offline/config.hpp"
#include "ego_offline/scene/scene_geometry.hpp"
#include "ego_offline/parse/source_demuxer.hpp"

#include <filesystem>
#include <string>
#include <vector>
#include <optional>

namespace ego_offline::finalize {

// ── Scene event (matches schema) ──────────────────────────────────────────────
struct SceneEvent {
    std::string event_id;
    std::string type;   // siren_on | siren_off | mode_change | etc.
    ns_t        t_start_ns  = 0;
    ns_t        t_end_ns    = 0;
    ns_t        source_ts_ns = 0;
    uint64_t    source_event_id = 0;
    double      confidence   = 1.0;
    std::vector<std::string> flags;

    struct Geometry {
        double range_m    = 0.0;
        double azimuth_deg = 0.0;
        double ego_lat_deg = 0.0, ego_lon_deg = 0.0;
        double src_lat_deg = 0.0, src_lon_deg = 0.0;
    };
    std::optional<Geometry> geometry_at_event;
};

// ── Scene segment (matches schema) ────────────────────────────────────────────
struct SceneSegment {
    std::string              segment_id;
    std::optional<std::string> event_id;
    ns_t                     t_start_ns = 0;
    ns_t                     t_end_ns   = 0;
    SegmentClass             seg_class  = SegmentClass::unknown;
    double                   confidence = 1.0;

    struct Metrics {
        double range_min_m          = 0.0;
        double range_max_m          = 0.0;
        double range_at_closest_m   = 0.0;
        double azimuth_at_closest_deg = 0.0;
        double max_closing_speed_mps = 0.0;
        double duration_s           = 0.0;
    } metrics;
};

// ── FN-05: segment classifier ─────────────────────────────────────────────────
SegmentClass classify_segment(
    const std::vector<SceneGeometry>& geom_in_segment,
    double min_duration_s,
    bool classify_unknown_when_degraded);

// ── Scene builder ─────────────────────────────────────────────────────────────
class SceneBuilder {
public:
    explicit SceneBuilder(const Config& cfg);

    // FN-02: extract siren events from source + optional audio
    std::vector<SceneEvent> extract_events(
        const std::vector<SourceEvent>& src_events,
        const std::vector<SceneGeometry>& geom) const;

    // FN-03: build continuous segments during siren_on periods
    std::vector<SceneSegment> build_segments(
        const std::vector<SceneEvent>&           events,
        const std::vector<SceneGeometry>& geom) const;

    // FN-04: link segments to events
    void link_segments_to_events(
        std::vector<SceneSegment>& segs,
        const std::vector<SceneEvent>& events) const;

    // FN-06: write scene_events.json
    void write_scene_events(
        const std::vector<SceneEvent>& events,
        const std::string& session_id,
        const std::filesystem::path& out_path) const;

    // FN-07: write scene_segments.json
    void write_scene_segments(
        const std::vector<SceneSegment>& segs,
        const std::string& session_id,
        const std::filesystem::path& out_path) const;

private:
    Config cfg_;
    static std::string new_uuid();
};

} // namespace ego_offline::finalize
