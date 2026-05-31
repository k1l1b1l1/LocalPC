#include "ego_offline/reports/report_builder.hpp"
#include "ego_offline/json_writer.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <stdexcept>

namespace ego_offline::reports {

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

double stream_score(const StreamValidation& sv) {
    if (!sv.file_ok || sv.packets_total == 0) return 0.0;
    double loss_ok = 1.0 - std::min(1.0,
        static_cast<double>(sv.packets_lost) / sv.packets_total);
    double mono_ok = sv.monotonic_violations > 0 ? 0.9 : 1.0;
    double gap_ok  = 1.0 - std::min(1.0,
        static_cast<double>(sv.audio_gap_count + sv.telemetry_gap_count)
        / std::max(uint64_t{1}, sv.packets_total / 100));
    return loss_ok * mono_ok * gap_ok;
}

} // namespace

ReportBuilder::ReportBuilder(const Config& cfg) : cfg_(cfg) {}

// ── RP-01 ──────────────────────────────────────────────────────────────────────

SessionReport ReportBuilder::build_session_report(
    const ValidationReport&        val_rpt,
    const SyncReport&              sync_rpt,
    const load::SessionBundle&     meta,
    const std::string&             session_id,
    const SessionReport::InputPaths& inputs,
    const SessionReport::Outputs&  outputs,
    const SessionReport::Timing&   timing,
    const std::vector<std::string>& errors) const
{
    SessionReport rpt;
    rpt.offline_pipeline_version = cfg_.offline_pipeline_version;
    rpt.session_id               = session_id;
    rpt.generated_at_utc         = utc_now_iso8601();
    rpt.input_paths              = inputs;
    rpt.outputs                  = outputs;
    rpt.timing                   = timing;
    rpt.errors                   = errors;

    rpt.validation_integrity = [&] {
        switch (val_rpt.integrity) {
            case validate::Integrity::ok:      return "ok";
            case validate::Integrity::warning: return "warning";
            default:                           return "failed";
        }
    }();

    rpt.sync_status = [&] {
        switch (sync_rpt.status) {
            case sync::SyncStatus::sync_ok:       return "sync_ok";
            case sync::SyncStatus::sync_degraded: return "sync_degraded";
            default:                              return "sync_failed";
        }
    }();

    bool ok = errors.empty()
        && val_rpt.integrity != validate::Integrity::failed
        && sync_rpt.status != sync::SyncStatus::sync_failed;
    bool partial = !errors.empty()
        || val_rpt.integrity == validate::Integrity::warning
        || sync_rpt.status == sync::SyncStatus::sync_degraded;

    rpt.pipeline_status = ok ? "success" : (partial ? "partial" : "failed");

    rpt.metadata.vehicle_id    = meta.session.vehicle_id;
    rpt.metadata.scenario_id   = meta.scenario ? meta.scenario->scenario_id : "";
    rpt.metadata.source_id     = meta.source   ? meta.source->source_id     : "";
    rpt.metadata.started_at_utc = meta.session.started_at_utc;
    rpt.metadata.stopped_at_utc = meta.session.stopped_at_utc;
    rpt.metadata.duration_s     = meta.session.duration_s;

    return rpt;
}

// ── RP-02 ──────────────────────────────────────────────────────────────────────

DataQualityReport ReportBuilder::build_data_quality_report(
    const ValidationReport& val_rpt,
    const std::string& session_id) const
{
    DataQualityReport rpt;
    rpt.offline_pipeline_version = cfg_.offline_pipeline_version;
    rpt.session_id               = session_id;
    rpt.generated_at_utc         = utc_now_iso8601();

    // Stream scores
    auto mk = [](const StreamValidation& sv, uint64_t sample_count, double rate, bool present) {
        DataQualityReport::StreamQuality q;
        q.present      = present && sv.file_ok;
        q.score        = present ? stream_score(sv) : 0.0;
        q.sample_count = sample_count;
        q.gap_count    = sv.audio_gap_count + sv.telemetry_gap_count
                       + static_cast<uint64_t>(sv.gap_events.size());
        q.mean_rate_hz = rate;
        return q;
    };

    double ego_dur = (val_rpt.ego.t_last_ns - val_rpt.ego.t_first_ns) / 1e9;
    double rate_audio = ego_dur > 0 ? val_rpt.ego.audio_gap_count / ego_dur : 0.0;
    double rate_imu   = ego_dur > 0 ? val_rpt.ego.telemetry_gap_count / ego_dur : 0.0;

    rpt.streams["audio"]        = mk(val_rpt.ego, val_rpt.ego.packets_valid, 48000.0, val_rpt.ego.file_ok);
    rpt.streams["imu"]          = mk(val_rpt.ego, val_rpt.ego.packets_valid, 100.0,   val_rpt.ego.file_ok);
    rpt.streams["gps_ego"]      = mk(val_rpt.ego, val_rpt.ego.packets_valid, 1.0,     val_rpt.ego.file_ok);
    rpt.streams["gps_source"]   = mk(val_rpt.source, val_rpt.source.packets_valid, 1.0, val_rpt.source.file_ok);
    rpt.streams["source_events"] = mk(val_rpt.source, val_rpt.source.packets_valid, 0.0, val_rpt.source.file_ok);

    // Valid time
    double total_dur = (val_rpt.ego.t_last_ns - val_rpt.ego.t_first_ns) / 1e9;
    rpt.valid_time.total_duration_s = total_dur;
    rpt.valid_time.valid_duration_s = val_rpt.summary.total_valid_duration_s;
    rpt.valid_time.valid_fraction   = val_rpt.summary.valid_fraction;
    rpt.valid_time.range_count      = static_cast<int>(val_rpt.valid_ranges.size());

    // Packet loss
    rpt.packet_loss.ego_packets_lost    = val_rpt.ego.packets_lost;
    rpt.packet_loss.source_packets_lost = val_rpt.source.packets_lost;
    rpt.packet_loss.ego_loss_fraction   =
        val_rpt.ego.packets_total > 0
        ? static_cast<double>(val_rpt.ego.packets_lost) / val_rpt.ego.packets_total
        : 0.0;
    rpt.packet_loss.source_loss_fraction =
        val_rpt.source.packets_total > 0
        ? static_cast<double>(val_rpt.source.packets_lost) / val_rpt.source.packets_total
        : 0.0;

    // Overall score: weighted average of stream scores
    double total_score = 0.0; int cnt = 0;
    for (const auto& [k, v] : rpt.streams) if (v.present) { total_score += v.score; ++cnt; }
    rpt.overall_score = cnt > 0 ? total_score / cnt : 0.0;
    rpt.usable_for_training = rpt.overall_score >= 0.7 && val_rpt.summary.valid_fraction >= 0.8;

    if (val_rpt.ego.packets_crc_failed > 0)  rpt.flags.push_back("ego_crc_errors");
    if (val_rpt.source.packets_crc_failed > 0) rpt.flags.push_back("source_crc_errors");
    if (!val_rpt.source.file_ok)              rpt.flags.push_back("source_missing");
    if (rpt.valid_time.valid_fraction < 0.5)  rpt.flags.push_back("low_valid_fraction");

    return rpt;
}

// ── RP-03 ──────────────────────────────────────────────────────────────────────

EventsReport ReportBuilder::build_events_report(
    const std::vector<SceneEvent>&   events,
    const std::vector<SceneSegment>& segments,
    const std::string&               session_id) const
{
    EventsReport rpt;
    rpt.offline_pipeline_version = cfg_.offline_pipeline_version;
    rpt.session_id               = session_id;
    rpt.generated_at_utc         = utc_now_iso8601();
    rpt.event_count              = static_cast<int>(events.size());

    // Build event rows
    for (const auto& ev : events) {
        EventsReport::EventRow row;
        row.event_id       = ev.event_id;
        row.type           = ev.type;
        row.t_start_ns     = ev.t_start_ns;
        row.t_end_ns       = ev.t_end_ns;
        row.source_event_id = ev.source_event_id;
        row.confidence     = ev.confidence;
        if (ev.geometry_at_event) {
            row.range_at_event_m     = ev.geometry_at_event->range_m;
            row.azimuth_at_event_deg = ev.geometry_at_event->azimuth_deg;
        }
        // Link segment IDs
        for (const auto& seg : segments)
            if (seg.event_id && *seg.event_id == ev.event_id)
                row.segment_ids.push_back(seg.segment_id);
        rpt.events.push_back(row);
    }

    // Segment summary
    rpt.segment_summary.segment_count = static_cast<int>(segments.size());
    for (const auto& seg : segments) {
        rpt.segment_summary.by_class[to_string(seg.seg_class)]++;
        if (seg.event_id) ++rpt.segment_summary.linked_to_event;
        else              ++rpt.segment_summary.unlinked;
    }

    return rpt;
}

// ── JSON ───────────────────────────────────────────────────────────────────────

std::string SessionReport::to_json() const {
    using namespace json;
    ObjectBuilder root(2, 0);
    root.field("offline_pipeline_version", str(offline_pipeline_version))
        .field("session_id",               str(session_id))
        .field("generated_at_utc",         str(generated_at_utc))
        .field("pipeline_status",          str(pipeline_status))
        .field("sync_status",              str(sync_status))
        .field("validation_integrity",     str(validation_integrity));

    root.object("input_paths", [this](ObjectBuilder& b) {
        b.field("session_dir",   json::str(input_paths.session_dir))
         .field("ego_manifest",  json::str(input_paths.ego_manifest))
         .field("source_bin",    json::str(input_paths.source_bin))
         .field("config",        json::str(input_paths.config));
    });

    root.object("outputs", [this](ObjectBuilder& b) {
        b.field("out_dir",            json::str(outputs.out_dir))
         .field("session_mf4",        json::str(outputs.session_mf4))
         .field("validation_report",  json::str(outputs.validation_report))
         .field("sync_report",        json::str(outputs.sync_report))
         .field("scene_events",       json::str(outputs.scene_events))
         .field("scene_segments",     json::str(outputs.scene_segments));
    });

    root.object("metadata", [this](ObjectBuilder& b) {
        b.field("vehicle_id",    json::str(metadata.vehicle_id))
         .field("scenario_id",   json::str(metadata.scenario_id))
         .field("source_id",     json::str(metadata.source_id))
         .field("started_at_utc", json::str(metadata.started_at_utc))
         .field("stopped_at_utc", json::str(metadata.stopped_at_utc))
         .field("duration_s",    json::num(metadata.duration_s, 3));
    });

    root.object("timing", [this](ObjectBuilder& b) {
        b.field("total_wall_s", json::num(timing.total_wall_s, 3));
        std::vector<std::string> stage_items;
        for (const auto& [k, v] : timing.stages_s)
            stage_items.push_back(json::str(k) + ": " + json::num(v, 3));
        // inline object for stages_s
        b.object("stages_s", [this](ObjectBuilder& sb) {
            for (const auto& [k, v] : timing.stages_s)
                sb.field(k, json::num(v, 3));
        });
    });

    std::vector<std::string> err_strs;
    for (const auto& e : errors) err_strs.push_back(str(e));
    root.array("errors", err_strs);

    return root.build();
}

void SessionReport::write(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write session_report: " + path.string());
    f << to_json() << '\n';
}

std::string DataQualityReport::to_json() const {
    using namespace json;
    ObjectBuilder root(2, 0);
    root.field("offline_pipeline_version", str(offline_pipeline_version))
        .field("session_id",               str(session_id))
        .field("generated_at_utc",         str(generated_at_utc))
        .field("overall_score",            num(overall_score, 4))
        .field("usable_for_training",      boolean(usable_for_training));

    root.object("streams", [this](ObjectBuilder& sb) {
        for (const auto& [name, q] : streams) {
            sb.object(name, [&q](ObjectBuilder& qb) {
                qb.field("present",      json::boolean(q.present))
                  .field("score",        json::num(q.score, 4))
                  .field("sample_count", json::num(static_cast<int64_t>(q.sample_count)))
                  .field("gap_count",    json::num(static_cast<int64_t>(q.gap_count)))
                  .field("mean_rate_hz", json::num(q.mean_rate_hz, 2))
                  .field("notes",        json::str(q.notes));
            });
        }
    });

    root.object("valid_time", [this](ObjectBuilder& b) {
        b.field("total_duration_s", json::num(valid_time.total_duration_s, 3))
         .field("valid_duration_s", json::num(valid_time.valid_duration_s, 3))
         .field("valid_fraction",   json::num(valid_time.valid_fraction, 4))
         .field("range_count",      json::num(static_cast<int64_t>(valid_time.range_count)));
    });

    root.object("packet_loss", [this](ObjectBuilder& b) {
        b.field("ego_packets_lost",     json::num(static_cast<int64_t>(packet_loss.ego_packets_lost)))
         .field("source_packets_lost",  json::num(static_cast<int64_t>(packet_loss.source_packets_lost)))
         .field("ego_loss_fraction",    json::num(packet_loss.ego_loss_fraction, 4))
         .field("source_loss_fraction", json::num(packet_loss.source_loss_fraction, 4));
    });

    root.object("navigation", [this](ObjectBuilder& b) {
        b.field("ego_rtk_fraction",    json::num(navigation.ego_rtk_fraction, 4))
         .field("source_rtk_fraction", json::num(navigation.source_rtk_fraction, 4))
         .field("mean_hdop_ego",       json::num(navigation.mean_hdop_ego, 3))
         .field("mean_hdop_source",    json::num(navigation.mean_hdop_source, 3));
    });

    std::vector<std::string> flag_strs;
    for (const auto& fl : flags) flag_strs.push_back(str(fl));
    root.array("flags", flag_strs);

    return root.build();
}

void DataQualityReport::write(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write data_quality_report: " + path.string());
    f << to_json() << '\n';
}

std::string EventsReport::to_json() const {
    using namespace json;
    ObjectBuilder root(2, 0);
    root.field("offline_pipeline_version", str(offline_pipeline_version))
        .field("session_id",               str(session_id))
        .field("generated_at_utc",         str(generated_at_utc))
        .field("event_count",              num(static_cast<int64_t>(event_count)));

    // events array
    std::vector<std::string> ev_objs;
    for (const auto& ev : events) {
        ObjectBuilder eb(2, 2);
        eb.field("event_id",        str(ev.event_id))
          .field("type",            str(ev.type))
          .field("t_start_ns",      num(static_cast<int64_t>(ev.t_start_ns)))
          .field("t_end_ns",        num(static_cast<int64_t>(ev.t_end_ns)))
          .field("source_event_id", num(static_cast<int64_t>(ev.source_event_id)))
          .field("confidence",      num(ev.confidence, 3))
          .field("range_at_event_m",   num(ev.range_at_event_m, 2))
          .field("azimuth_at_event_deg", num(ev.azimuth_at_event_deg, 2));

        std::vector<std::string> seg_ids;
        for (const auto& sid : ev.segment_ids) seg_ids.push_back(str(sid));
        eb.array("segment_ids", seg_ids);
        ev_objs.push_back(eb.build());
    }
    root.raw_array("events", ev_objs);

    // segment_summary
    root.object("segment_summary", [this](ObjectBuilder& b) {
        b.field("segment_count",   json::num(static_cast<int64_t>(segment_summary.segment_count)))
         .field("linked_to_event", json::num(static_cast<int64_t>(segment_summary.linked_to_event)))
         .field("unlinked",        json::num(static_cast<int64_t>(segment_summary.unlinked)));
        b.object("by_class", [this](ObjectBuilder& cb) {
            for (const auto& [k, v] : segment_summary.by_class)
                cb.field(k, json::num(static_cast<int64_t>(v)));
        });
    });

    return root.build();
}

void EventsReport::write(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write events_report: " + path.string());
    f << to_json() << '\n';
}

} // namespace ego_offline::reports
