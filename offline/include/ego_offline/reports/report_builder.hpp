#pragma once
// RP-01..03: session_report, data_quality_report, events_report

#include "ego_offline/types.hpp"
#include "ego_offline/config.hpp"
#include "ego_offline/validate/log_validator.hpp"
#include "ego_offline/sync/time_sync.hpp"
#include "ego_offline/finalize/scene_builder.hpp"
#include "ego_offline/load/metadata_loader.hpp"

#include <filesystem>
#include <string>
#include <vector>
#include <map>
#include <chrono>

namespace ego_offline::reports {

using validate::ValidationReport;
using validate::StreamValidation;
using sync::SyncReport;
using finalize::SceneEvent;
using finalize::SceneSegment;

// ── RP-01: session_report ─────────────────────────────────────────────────────
struct SessionReport {
    std::string  offline_pipeline_version;
    std::string  session_id;
    std::string  generated_at_utc;
    std::string  pipeline_status;    // success | partial | failed
    std::string  sync_status;        // sync_ok | sync_degraded | sync_failed | skipped
    std::string  validation_integrity;// ok | warning | failed | skipped

    struct InputPaths {
        std::string session_dir;
        std::string ego_manifest;
        std::string source_bin;
        std::string config;
    } input_paths;

    struct Outputs {
        std::string out_dir;
        std::string session_mf4;
        std::string validation_report;
        std::string sync_report;
        std::string scene_events;
        std::string scene_segments;
    } outputs;

    struct Meta {
        std::string vehicle_id;
        std::string scenario_id;
        std::string source_id;
        std::string started_at_utc;
        std::string stopped_at_utc;
        double      duration_s = 0.0;
    } metadata;

    struct Timing {
        double total_wall_s = 0.0;
        std::map<std::string, double> stages_s;
    } timing;

    std::vector<std::string> errors;

    std::string to_json() const;
    void write(const std::filesystem::path& path) const;
};

// ── RP-02: data_quality_report ────────────────────────────────────────────────
struct DataQualityReport {
    std::string  offline_pipeline_version;
    std::string  session_id;
    std::string  generated_at_utc;
    double       overall_score       = 0.0;
    bool         usable_for_training = false;

    struct StreamQuality {
        bool     present      = false;
        double   score        = 0.0;
        uint64_t sample_count = 0;
        uint64_t gap_count    = 0;
        double   mean_rate_hz = 0.0;
        std::string notes;
    };
    std::map<std::string, StreamQuality> streams;

    struct ValidTime {
        double total_duration_s = 0.0;
        double valid_duration_s = 0.0;
        double valid_fraction   = 0.0;
        int    range_count      = 0;
    } valid_time;

    struct PacketLoss {
        uint64_t ego_packets_lost    = 0;
        uint64_t source_packets_lost = 0;
        double   ego_loss_fraction   = 0.0;
        double   source_loss_fraction = 0.0;
    } packet_loss;

    struct Navigation {
        double ego_rtk_fraction    = 0.0;
        double source_rtk_fraction = 0.0;
        double mean_hdop_ego       = 0.0;
        double mean_hdop_source    = 0.0;
    } navigation;

    std::vector<std::string> flags;

    std::string to_json() const;
    void write(const std::filesystem::path& path) const;
};

// ── RP-03: events_report ──────────────────────────────────────────────────────
struct EventsReport {
    std::string  offline_pipeline_version;
    std::string  session_id;
    std::string  generated_at_utc;
    int          event_count = 0;

    struct EventRow {
        std::string event_id;
        std::string type;
        ns_t        t_start_ns = 0;
        ns_t        t_end_ns   = 0;
        uint64_t    source_event_id = 0;
        double      confidence  = 1.0;
        std::vector<std::string> segment_ids;
        double      range_at_event_m    = 0.0;
        double      azimuth_at_event_deg = 0.0;
    };
    std::vector<EventRow> events;

    struct SegmentSummary {
        int segment_count = 0;
        std::map<std::string, int> by_class;
        int linked_to_event = 0;
        int unlinked        = 0;
    } segment_summary;

    std::string to_json() const;
    void write(const std::filesystem::path& path) const;
};

// ── ReportBuilder ─────────────────────────────────────────────────────────────
class ReportBuilder {
public:
    explicit ReportBuilder(const Config& cfg);

    SessionReport build_session_report(
        const ValidationReport&        val_rpt,
        const SyncReport&              sync_rpt,
        const load::SessionBundle&     meta,
        const std::string&             session_id,
        const SessionReport::InputPaths& inputs,
        const SessionReport::Outputs&  outputs,
        const SessionReport::Timing&   timing,
        const std::vector<std::string>& errors) const;

    DataQualityReport build_data_quality_report(
        const ValidationReport&        val_rpt,
        const std::string&             session_id) const;

    EventsReport build_events_report(
        const std::vector<SceneEvent>&   events,
        const std::vector<SceneSegment>& segments,
        const std::string&               session_id) const;

private:
    Config cfg_;
};

} // namespace ego_offline::reports
