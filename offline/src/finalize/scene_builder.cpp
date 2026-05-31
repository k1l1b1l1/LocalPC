#include "ego_offline/finalize/scene_builder.hpp"
#include "ego_offline/json_writer.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <random>
#include <stdexcept>

namespace ego_offline::finalize {

namespace {

std::string utc_now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

const char* source_event_type_str(SourceEventType t) {
    switch (t) {
        case SourceEventType::kSirenOn:       return "siren_on";
        case SourceEventType::kSirenOff:      return "siren_off";
        case SourceEventType::kModeChange:    return "mode_change";
        case SourceEventType::kUserMarker:    return "user_marker";
        case SourceEventType::kScenarioStart: return "scenario_start";
        case SourceEventType::kScenarioStop:  return "scenario_stop";
        default:                              return "unknown";
    }
}

// Find the SceneGeometry nearest to ts_ns
const SceneGeometry* nearest_geom(
    const std::vector<SceneGeometry>& geom, ns_t ts_ns) {
    if (geom.empty()) return nullptr;
    auto it = std::min_element(geom.begin(), geom.end(),
        [ts_ns](const SceneGeometry& a, const SceneGeometry& b) {
            return std::abs(a.ts_ns - ts_ns) < std::abs(b.ts_ns - ts_ns);
        });
    return &(*it);
}

} // namespace

// ── FN-05: segment classifier ─────────────────────────────────────────────────

SegmentClass classify_segment(
    const std::vector<SceneGeometry>& g,
    double min_duration_s,
    bool classify_unknown_when_degraded)
{
    if (g.empty()) return SegmentClass::unknown;

    // Collect valid range values
    std::vector<double> ranges;
    for (const auto& sg : g) if (sg.valid) ranges.push_back(sg.range_m);
    if (ranges.empty()) {
        return classify_unknown_when_degraded ? SegmentClass::unknown : SegmentClass::unknown;
    }

    // Duration check
    double dur = static_cast<double>(g.back().ts_ns - g.front().ts_ns) / 1e9;
    if (dur < min_duration_s) return SegmentClass::unknown;

    double r_min = *std::min_element(ranges.begin(), ranges.end());
    double r_max = *std::max_element(ranges.begin(), ranges.end());

    // Closing speed trend (mean over valid samples)
    double total_closing = 0.0;
    for (const auto& sg : g) if (sg.valid) total_closing += sg.closing_speed_mps;
    double mean_closing = total_closing / static_cast<double>(ranges.size());

    // pass_by: clear range minimum in middle AND significant range variation
    size_t min_idx = static_cast<size_t>(
        std::min_element(ranges.begin(), ranges.end()) - ranges.begin());
    double range_variation = (r_max > 0) ? (r_max - r_min) / r_max : 0.0;
    if (range_variation > 0.15
        && min_idx > ranges.size() / 5
        && min_idx < 4 * ranges.size() / 5)
        return SegmentClass::pass_by;

    // Stationary: small range variation AND near-zero mean closing speed
    if (range_variation < 0.10 && std::abs(mean_closing) < 1.0)
        return SegmentClass::stationary;

    if (mean_closing > 0.5)  return SegmentClass::approach;
    if (mean_closing < -0.5) return SegmentClass::recede;

    return SegmentClass::unknown;
}

// ── SceneBuilder ──────────────────────────────────────────────────────────────

SceneBuilder::SceneBuilder(const Config& cfg) : cfg_(cfg) {}

std::string SceneBuilder::new_uuid() {
    // Simple pseudo-UUID v4
    static std::mt19937_64 rng(std::chrono::steady_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t a = dist(rng), b = dist(rng);
    // Set version (4) and variant bits
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(8)  << ((a >> 32) & 0xFFFFFFFF) << "-"
        << std::setw(4)  << ((a >> 16) & 0xFFFF)     << "-"
        << std::setw(4)  << (a & 0xFFFF)              << "-"
        << std::setw(4)  << ((b >> 48) & 0xFFFF)      << "-"
        << std::setw(12) << (b & 0xFFFFFFFFFFFFull);
    return oss.str();
}

std::vector<SceneEvent> SceneBuilder::extract_events(
    const std::vector<SourceEvent>& src_events,
    const std::vector<SceneGeometry>& geom) const
{
    std::vector<SceneEvent> out;
    for (const auto& se : src_events) {
        SceneEvent ev;
        ev.event_id       = new_uuid();
        ev.type           = source_event_type_str(se.event_type);
        ev.t_start_ns     = se.ts_ego_ns;  // already in ego clock
        ev.t_end_ns       = se.ts_ego_ns + (se.duration_ns > 0 ? se.duration_ns : 0);
        ev.source_ts_ns   = se.ts_ns;
        ev.source_event_id = se.event_id;
        ev.confidence     = 1.0;

        if (se.flags & 0x01) ev.flags.push_back("manual_trigger");
        if (se.flags & 0x02) ev.flags.push_back("auto_trigger");
        if (se.flags & 0x04) ev.flags.push_back("gps_time_valid");

        // Attach geometry at event
        const auto* g = nearest_geom(geom, ev.t_start_ns);
        if (g && g->valid) {
            SceneEvent::Geometry gev;
            gev.range_m      = g->range_m;
            gev.azimuth_deg  = g->azimuth_deg;
            gev.ego_lat_deg  = g->ego_lat_deg;
            gev.ego_lon_deg  = g->ego_lon_deg;
            gev.src_lat_deg  = g->src_lat_deg;
            gev.src_lon_deg  = g->src_lon_deg;
            ev.geometry_at_event = gev;
        }
        out.push_back(ev);
    }
    return out;
}

std::vector<SceneSegment> SceneBuilder::build_segments(
    const std::vector<SceneEvent>&           events,
    const std::vector<SceneGeometry>& geom) const
{
    std::vector<SceneSegment> out;

    // Find siren_on / siren_off pairs
    std::vector<std::pair<ns_t, ns_t>> intervals; // [on, off)
    ns_t pending_on = -1;
    for (const auto& ev : events) {
        if (ev.type == "siren_on")  { pending_on = ev.t_start_ns; }
        if (ev.type == "siren_off" && pending_on >= 0) {
            intervals.push_back({pending_on, ev.t_start_ns});
            pending_on = -1;
        }
    }
    // If siren is still on at end of session
    if (pending_on >= 0 && !geom.empty())
        intervals.push_back({pending_on, geom.back().ts_ns});

    ns_t min_dur_ns = static_cast<ns_t>(cfg_.scene.segment_min_duration_ms) * kNsPerMs;

    for (const auto& [t0, t1] : intervals) {
        if (t1 - t0 < min_dur_ns) continue;

        // Collect geometry in interval
        std::vector<SceneGeometry> seg_geom;
        for (const auto& g : geom)
            if (g.ts_ns >= t0 && g.ts_ns <= t1)
                seg_geom.push_back(g);

        SceneSegment seg;
        seg.segment_id = new_uuid();
        seg.t_start_ns = t0;
        seg.t_end_ns   = t1;
        seg.seg_class  = classify_segment(seg_geom,
                            cfg_.scene.segment_min_duration_ms / 1000.0,
                            cfg_.scene.classify_unknown_when_degraded);
        seg.confidence = seg_geom.empty() ? 0.3 : 0.9;

        // Compute metrics
        std::vector<double> ranges;
        for (const auto& g : seg_geom) if (g.valid) ranges.push_back(g.range_m);

        if (!ranges.empty()) {
            seg.metrics.range_min_m = *std::min_element(ranges.begin(), ranges.end());
            seg.metrics.range_max_m = *std::max_element(ranges.begin(), ranges.end());

            // Closest point
            auto it_min = std::min_element(seg_geom.begin(), seg_geom.end(),
                [](const SceneGeometry& a, const SceneGeometry& b) {
                    return a.valid && (!b.valid || a.range_m < b.range_m);
                });
            if (it_min != seg_geom.end() && it_min->valid) {
                seg.metrics.range_at_closest_m   = it_min->range_m;
                seg.metrics.azimuth_at_closest_deg = it_min->azimuth_deg;
            }

            // Max closing speed
            double max_cs = 0.0;
            for (const auto& g : seg_geom)
                if (g.valid) max_cs = std::max(max_cs, g.closing_speed_mps);
            seg.metrics.max_closing_speed_mps = max_cs;
        }
        seg.metrics.duration_s = static_cast<double>(t1 - t0) / 1e9;
        out.push_back(seg);
    }
    return out;
}

void SceneBuilder::link_segments_to_events(
    std::vector<SceneSegment>& segs,
    const std::vector<SceneEvent>& events) const
{
    for (auto& seg : segs) {
        // Find the siren_on event that starts this segment
        for (const auto& ev : events) {
            if (ev.type == "siren_on"
                && std::abs(ev.t_start_ns - seg.t_start_ns) < 500 * kNsPerMs) {
                seg.event_id = ev.event_id;
                break;
            }
        }
    }
}

// ── JSON output ───────────────────────────────────────────────────────────────

void SceneBuilder::write_scene_events(
    const std::vector<SceneEvent>& events,
    const std::string& session_id,
    const std::filesystem::path& out_path) const
{
    std::filesystem::create_directories(out_path.parent_path());
    using namespace ego_offline::json;

    std::vector<std::string> ev_objs;
    for (const auto& ev : events) {
        ObjectBuilder b(2, 2);
        b.field("event_id",        str(ev.event_id))
         .field("type",            str(ev.type))
         .field("t_start_ns",      num(static_cast<int64_t>(ev.t_start_ns)))
         .field("t_end_ns",        num(static_cast<int64_t>(ev.t_end_ns)))
         .field("source_ts_ns",    num(static_cast<int64_t>(ev.source_ts_ns)))
         .field("source_event_id", num(static_cast<int64_t>(ev.source_event_id)))
         .field("confidence",      num(ev.confidence, 3));

        std::vector<std::string> flags;
        for (const auto& f : ev.flags) flags.push_back(str(f));
        b.array("flags", flags);

        if (ev.geometry_at_event) {
            const auto& g = *ev.geometry_at_event;
            b.object("geometry_at_event", [&g](ObjectBuilder& gb) {
                gb.field("range_m",      json::num(g.range_m, 3))
                  .field("azimuth_deg",  json::num(g.azimuth_deg, 3))
                  .field("ego_lat_deg",  json::num(g.ego_lat_deg, 8))
                  .field("ego_lon_deg",  json::num(g.ego_lon_deg, 8))
                  .field("source_lat_deg", json::num(g.src_lat_deg, 8))
                  .field("source_lon_deg", json::num(g.src_lon_deg, 8));
            });
        }
        ev_objs.push_back(b.build());
    }

    ObjectBuilder root(2, 0);
    root.field("offline_pipeline_version", json::str(cfg_.offline_pipeline_version))
        .field("session_id",               json::str(session_id))
        .field("generated_at_utc",         json::str(utc_now_iso8601()))
        .raw_array("events",               ev_objs);

    std::ofstream f(out_path);
    if (!f) throw std::runtime_error("Cannot write scene_events: " + out_path.string());
    f << root.build() << '\n';
}

void SceneBuilder::write_scene_segments(
    const std::vector<SceneSegment>& segs,
    const std::string& session_id,
    const std::filesystem::path& out_path) const
{
    std::filesystem::create_directories(out_path.parent_path());
    using namespace ego_offline::json;

    std::vector<std::string> seg_objs;
    for (const auto& seg : segs) {
        ObjectBuilder b(2, 2);
        b.field("segment_id",  str(seg.segment_id))
         .field("event_id",    seg.event_id ? str(*seg.event_id) : null_val())
         .field("t_start_ns",  num(static_cast<int64_t>(seg.t_start_ns)))
         .field("t_end_ns",    num(static_cast<int64_t>(seg.t_end_ns)))
         .field("class",       str(to_string(seg.seg_class)))
         .field("confidence",  num(seg.confidence, 3));

        b.object("metrics", [&seg](ObjectBuilder& m) {
            m.field("range_min_m",          json::num(seg.metrics.range_min_m, 2))
             .field("range_max_m",          json::num(seg.metrics.range_max_m, 2))
             .field("range_at_closest_m",   json::num(seg.metrics.range_at_closest_m, 2))
             .field("azimuth_at_closest_deg", json::num(seg.metrics.azimuth_at_closest_deg, 2))
             .field("max_closing_speed_mps",json::num(seg.metrics.max_closing_speed_mps, 3))
             .field("duration_s",           json::num(seg.metrics.duration_s, 3));
        });

        seg_objs.push_back(b.build());
    }

    ObjectBuilder root(2, 0);
    root.field("offline_pipeline_version", json::str(cfg_.offline_pipeline_version))
        .field("session_id",               json::str(session_id))
        .field("generated_at_utc",         json::str(utc_now_iso8601()))
        .raw_array("segments",             seg_objs);

    std::ofstream f(out_path);
    if (!f) throw std::runtime_error("Cannot write scene_segments: " + out_path.string());
    f << root.build() << '\n';
}

} // namespace ego_offline::finalize
