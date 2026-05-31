#include "ego_offline/sync/time_sync.hpp"
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

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ego_offline::sync {

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

double deg2rad(double d) { return d * M_PI / 180.0; }

// Haversine distance in metres
double haversine(double lat1, double lon1, double lat2, double lon2) {
    const double R = 6371000.0;
    double dlat = deg2rad(lat2 - lat1);
    double dlon = deg2rad(lon2 - lon1);
    double a = std::sin(dlat/2)*std::sin(dlat/2)
             + std::cos(deg2rad(lat1))*std::cos(deg2rad(lat2))
             * std::sin(dlon/2)*std::sin(dlon/2);
    return R * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0-a));
}

double median(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return (n % 2 == 0) ? (v[n/2-1] + v[n/2]) / 2.0 : v[n/2];
}

const char* sync_status_str(SyncStatus s) {
    switch (s) {
        case SyncStatus::sync_ok:       return "sync_ok";
        case SyncStatus::sync_degraded: return "sync_degraded";
        case SyncStatus::sync_failed:   return "sync_failed";
    }
    return "sync_failed";
}

} // namespace

// ── SY-01: time model estimator ──────────────────────────────────────────────

TimeModel estimate_time_model(const std::vector<ns_t>& timestamps) {
    TimeModel m;
    if (timestamps.size() < 2) return m;

    std::vector<double> dts;
    dts.reserve(timestamps.size() - 1);
    for (size_t i = 1; i < timestamps.size(); ++i) {
        double dt = static_cast<double>(timestamps[i] - timestamps[i-1]);
        if (dt > 0) dts.push_back(dt);
    }

    double med_dt = median(dts);
    double threshold = med_dt * 3.0;
    uint32_t outliers = 0;
    double sum = 0.0; uint32_t cnt = 0;
    for (double d : dts) {
        if (d > threshold) { ++outliers; continue; }
        sum += d; ++cnt;
    }
    m.median_dt_ns = med_dt;
    m.outliers     = outliers;
    m.mean_rate_hz = (cnt > 0 && sum > 0) ? (cnt * 1e9 / sum) : 0.0;
    return m;
}

// ── SY-03: GNSS cross-correlation ────────────────────────────────────────────

ClockEstimate sync_gnss_xcorr(
    const std::vector<GnssPoint>& ego_gps,
    const std::vector<GnssPoint>& src_gps,
    double search_window_s)
{
    ClockEstimate est;
    est.method = "gnss_xcorr";

    if (ego_gps.empty() || src_gps.empty()) {
        est.confidence = 0.0;
        return est;
    }

    // Filter to fix_quality > 0
    auto good = [](const GnssPoint& g) { return g.fix_quality > 0 && g.hdop < 5.0f; };
    std::vector<GnssPoint> ego_ok, src_ok;
    for (const auto& g : ego_gps) if (good(g)) ego_ok.push_back(g);
    for (const auto& g : src_gps) if (good(g)) src_ok.push_back(g);

    if (ego_ok.empty() || src_ok.empty()) {
        est.confidence = 0.0;
        return est;
    }

    // Build speed time-series for cross-correlation
    // Resample both to a common 1 Hz grid in ego time
    ns_t t0_ego = ego_ok.front().ts_ns;
    ns_t t1_ego = ego_ok.back().ts_ns;
    ns_t t0_src = src_ok.front().ts_ns;
    ns_t t1_src = src_ok.back().ts_ns;

    int grid_n = static_cast<int>((t1_ego - t0_ego) / kNsPerSec) + 1;
    if (grid_n < 5) { est.confidence = 0.0; return est; }

    // Resample ego speed to 1 Hz grid
    auto resample = [](const std::vector<GnssPoint>& pts, ns_t t0, int n) {
        std::vector<float> out(static_cast<size_t>(n), 0.f);
        size_t j = 0;
        for (int i = 0; i < n; ++i) {
            ns_t t = t0 + static_cast<ns_t>(i) * kNsPerSec;
            while (j + 1 < pts.size() && pts[j+1].ts_ns <= t) ++j;
            out[static_cast<size_t>(i)] = pts[j].speed_mps;
        }
        return out;
    };

    std::vector<float> ego_speed = resample(ego_ok, t0_ego, grid_n);

    // Search over offset range
    ns_t   max_off  = static_cast<ns_t>(search_window_s * kNsPerSec);
    ns_t   best_off = 0;
    double best_xcorr = -1e99;

    int steps = static_cast<int>(search_window_s * 2) * 10; // 0.1s resolution
    for (int s = -steps; s <= steps; ++s) {
        ns_t trial_off = static_cast<ns_t>(s) * 100'000'000LL; // 0.1 s steps
        if (std::abs(trial_off) > max_off) continue;

        // Resample source speed shifted by trial_off
        std::vector<GnssPoint> shifted = src_ok;
        for (auto& g : shifted) g.ts_ns -= trial_off; // shift source to ego time
        std::vector<float> src_speed = resample(shifted, t0_ego, grid_n);

        // Cross-correlation coefficient
        double s_xy = 0, s_x2 = 0, s_y2 = 0, mx = 0, my = 0;
        int cnt = 0;
        for (int i = 0; i < grid_n; ++i) { mx += ego_speed[static_cast<size_t>(i)]; my += src_speed[static_cast<size_t>(i)]; ++cnt; }
        if (cnt == 0) continue;
        mx /= cnt; my /= cnt;
        for (int i = 0; i < grid_n; ++i) {
            double dx = ego_speed[static_cast<size_t>(i)] - mx;
            double dy = src_speed[static_cast<size_t>(i)] - my;
            s_xy += dx*dy; s_x2 += dx*dx; s_y2 += dy*dy;
        }
        double denom = std::sqrt(s_x2 * s_y2);
        if (denom < 1e-9) continue;
        double corr = s_xy / denom;
        if (corr > best_xcorr) { best_xcorr = corr; best_off = trial_off; }
    }

    est.offset_ns  = best_off;
    est.confidence = std::max(0.0, std::min(1.0, (best_xcorr + 1.0) / 2.0));

    // Linear drift estimation: fit source_ts = a * ego_ts + b
    // Using pairs near best_off to refine
    {
        std::vector<double> ex, sy;
        for (const auto& eg : ego_ok) {
            // Find closest source fix
            ns_t target = eg.ts_ns + best_off;
            auto it = std::min_element(src_ok.begin(), src_ok.end(),
                [target](const GnssPoint& a, const GnssPoint& b) {
                    return std::abs(a.ts_ns - target) < std::abs(b.ts_ns - target);
                });
            if (it == src_ok.end()) continue;
            if (std::abs(it->ts_ns - target) > 5 * kNsPerSec) continue;
            ex.push_back(static_cast<double>(eg.ts_ns));
            sy.push_back(static_cast<double>(it->ts_ns));
        }
        if (ex.size() >= 3) {
            // Simple linear regression: y = a + b*x
            double n = static_cast<double>(ex.size());
            double sx = 0, sy_ = 0, sxx = 0, sxy = 0;
            for (size_t i = 0; i < ex.size(); ++i) {
                sx += ex[i]; sy_ += sy[i]; sxx += ex[i]*ex[i]; sxy += ex[i]*sy[i];
            }
            double b = (n*sxy - sx*sy_) / (n*sxx - sx*sx);
            double a = (sy_ - b*sx) / n;
            // drift_ppm = (b - 1) * 1e6
            est.drift_ppm = (b - 1.0) * 1e6;

            // RMSE
            double sse = 0;
            for (size_t i = 0; i < ex.size(); ++i) {
                double pred = a + b * ex[i];
                double res  = sy[i] - pred;
                sse += res*res;
            }
            est.rmse_ns = std::sqrt(sse / ex.size());
        }
    }
    return est;
}

// ── SY-03 fallback ────────────────────────────────────────────────────────────

ClockEstimate sync_event_audio_fallback(
    const std::vector<SourceEvent>& src_events,
    const std::vector<ns_t>&        audio_onset_ts,
    double search_window_s)
{
    ClockEstimate est;
    est.method = "event_audio";

    // Find siren_on events
    std::vector<ns_t> siren_ons;
    for (const auto& e : src_events)
        if (e.event_type == SourceEventType::kSirenOn)
            siren_ons.push_back(e.ts_ns);

    if (siren_ons.empty() || audio_onset_ts.empty()) {
        est.confidence = 0.0;
        return est;
    }

    ns_t max_off = static_cast<ns_t>(search_window_s * kNsPerSec);
    ns_t best_off = 0;
    int  best_matches = 0;

    // Grid search: for each offset, count matching event-audio pairs within ±500ms
    int steps = static_cast<int>(search_window_s * 10); // 100ms steps
    for (int s = -steps; s <= steps; ++s) {
        ns_t trial_off = static_cast<ns_t>(s) * 100'000'000LL;
        int matches = 0;
        for (ns_t son : siren_ons) {
            ns_t shifted = son - trial_off;
            for (ns_t ao : audio_onset_ts) {
                if (std::abs(ao - shifted) < 500'000'000LL) { ++matches; break; }
            }
        }
        if (matches > best_matches) { best_matches = matches; best_off = trial_off; }
    }

    est.offset_ns  = best_off;
    est.confidence = (best_matches == 0) ? 0.0 :
        std::min(1.0, static_cast<double>(best_matches) / siren_ons.size() * 0.8);
    return est;
}

// ── LogSynchroniser ───────────────────────────────────────────────────────────

LogSynchroniser::LogSynchroniser(const Config& cfg) : cfg_(cfg) {}

SyncReport LogSynchroniser::synchronise(
    const std::vector<GnssPoint>&   ego_gps,
    const std::vector<GnssPoint>&   src_gps,
    const std::vector<SourceEvent>& src_events,
    const std::vector<ns_t>&        audio_onset_ts,
    const TimeRange&               valid_range,
    const std::string&             session_id) const
{
    SyncReport rpt;
    rpt.offline_pipeline_version = cfg_.offline_pipeline_version;
    rpt.session_id               = session_id;
    rpt.generated_at_utc         = utc_now_iso8601();
    rpt.master_clock             = cfg_.sync.master_clock;
    rpt.search_window_s          = cfg_.sync.max_offset_search_s;

    // Filter GPS to valid range
    auto filter_gps = [&](const std::vector<GnssPoint>& v) {
        std::vector<GnssPoint> out;
        for (const auto& g : v)
            if (g.ts_ns >= valid_range.t_start_ns && g.ts_ns <= valid_range.t_end_ns)
                out.push_back(g);
        return out;
    };

    auto ego_filtered = filter_gps(ego_gps);
    auto src_filtered = filter_gps(src_gps);

    // Try primary: GNSS xcorr
    ClockEstimate est = sync_gnss_xcorr(ego_filtered, src_filtered,
                                         cfg_.sync.max_offset_search_s);

    if (est.confidence < cfg_.sync.min_confidence) {
        // Fallback: event + audio
        ClockEstimate fb = sync_event_audio_fallback(src_events, audio_onset_ts,
                                                      cfg_.sync.max_offset_search_s);
        if (fb.confidence > est.confidence) {
            est = fb;
            rpt.notes.push_back("GNSS xcorr confidence low; used event+audio fallback");
        }
    }

    rpt.offset_ns  = est.offset_ns;
    rpt.drift_ppm  = est.drift_ppm;
    rpt.rmse_ns    = est.rmse_ns;
    rpt.confidence = est.confidence;
    rpt.method     = est.method;

    if (est.confidence >= cfg_.sync.min_confidence) {
        rpt.status = SyncStatus::sync_ok;
        TimeRange ar;
        ar.t_start_ns = valid_range.t_start_ns;
        ar.t_end_ns   = valid_range.t_end_ns;
        rpt.aligned_valid_range = ar;
    } else if (est.confidence >= cfg_.sync.min_confidence * 0.5) {
        rpt.status = SyncStatus::sync_degraded;
        rpt.notes.push_back("Low confidence sync — results may be inaccurate");
    } else {
        rpt.status = SyncStatus::sync_failed;
        rpt.notes.push_back("Sync failed — insufficient overlap or quality");
    }
    return rpt;
}

void LogSynchroniser::apply_offset(std::vector<SourceEvent>& events, ns_t offset_ns) {
    for (auto& e : events) {
        e.ts_ego_ns = e.ts_ns - offset_ns;
    }
}

void LogSynchroniser::apply_offset(std::vector<GnssPoint>& gnss, ns_t offset_ns) {
    for (auto& g : gnss) g.ts_ns -= offset_ns;
}

// ── SyncReport JSON ───────────────────────────────────────────────────────────

std::string SyncReport::to_json() const {
    using namespace json;
    ObjectBuilder root(2, 0);
    root.field("offline_pipeline_version", str(offline_pipeline_version))
        .field("session_id",               str(session_id))
        .field("generated_at_utc",         str(generated_at_utc))
        .field("status",                   str(sync_status_str(status)))
        .field("master_clock",             str(master_clock))
        .field("method",                   str(method))
        .field("offset_ns",                num(static_cast<int64_t>(offset_ns)))
        .field("drift_ppm",                num(drift_ppm, 4))
        .field("rmse_ns",                  num(rmse_ns, 2))
        .field("confidence",               num(confidence, 4))
        .field("search_window_s",          num(search_window_s, 2));

    if (aligned_valid_range) {
        root.object("aligned_valid_range", [this](ObjectBuilder& b) {
            b.field("t_start_ns", num(static_cast<int64_t>(aligned_valid_range->t_start_ns)))
             .field("t_end_ns",   num(static_cast<int64_t>(aligned_valid_range->t_end_ns)));
        });
    }

    std::vector<std::string> note_strs;
    for (const auto& n : notes) note_strs.push_back(json::str(n));
    root.array("notes", note_strs);

    return root.build();
}

void SyncReport::write(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write sync_report: " + path.string());
    f << to_json() << '\n';
}

} // namespace ego_offline::sync
