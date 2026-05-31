#pragma once
// SY-01..06: time synchronisation ego <-> source

#include "ego_offline/types.hpp"
#include "ego_offline/config.hpp"

#include <vector>
#include <string>
#include <filesystem>
#include <optional>

namespace ego_offline::sync {

struct TimeModel {
    double   mean_rate_hz = 0.0;
    double   median_dt_ns = 0.0;
    uint32_t outliers     = 0;
};

TimeModel estimate_time_model(const std::vector<ns_t>& timestamps);

struct ClockEstimate {
    ns_t        offset_ns  = 0;
    double      drift_ppm  = 0.0;
    double      rmse_ns    = 0.0;
    double      confidence = 0.0;
    std::string method     = "unknown";
};

ClockEstimate sync_gnss_xcorr(
    const std::vector<GnssPoint>& ego_gps,
    const std::vector<GnssPoint>& src_gps,
    double search_window_s);

ClockEstimate sync_event_audio_fallback(
    const std::vector<SourceEvent>& src_events,
    const std::vector<ns_t>&        audio_onset_ts,
    double search_window_s);

enum class SyncStatus { sync_ok, sync_degraded, sync_failed };

struct SyncReport {
    std::string  offline_pipeline_version;
    std::string  session_id;
    std::string  generated_at_utc;
    SyncStatus   status        = SyncStatus::sync_failed;
    std::string  master_clock  = "ego";
    std::string  method        = "unknown";
    ns_t         offset_ns     = 0;
    double       drift_ppm     = 0.0;
    double       rmse_ns       = 0.0;
    double       confidence    = 0.0;
    double       search_window_s = 30.0;
    std::optional<TimeRange> aligned_valid_range;
    std::vector<std::string> notes;

    std::string to_json() const;
    void write(const std::filesystem::path& path) const;
};

class LogSynchroniser {
public:
    explicit LogSynchroniser(const Config& cfg);

    SyncReport synchronise(
        const std::vector<GnssPoint>&   ego_gps,
        const std::vector<GnssPoint>&   src_gps,
        const std::vector<SourceEvent>& src_events,
        const std::vector<ns_t>&        audio_onset_ts,
        const TimeRange&                valid_range,
        const std::string&              session_id) const;

    static void apply_offset(std::vector<SourceEvent>& events, ns_t offset_ns);
    static void apply_offset(std::vector<GnssPoint>&   gnss,   ns_t offset_ns);

private:
    Config cfg_;
};

} // namespace ego_offline::sync
