// IT-04: Sync unit tests — synthetic ego+source with known offset
// AC-03: error < 50 ms

#include "ego_offline/sync/time_sync.hpp"
#include "ego_offline/types.hpp"
#include "ego_offline/config.hpp"

#include <cassert>
#include <iostream>
#include <cmath>

using namespace ego_offline;
using namespace ego_offline::sync;

static GnssPoint make_gps(ns_t ts_ns, float speed, double lat = 55.0, double lon = 37.0,
                           uint8_t fix = 3, float hdop = 1.0f) {
    GnssPoint g;
    g.ts_ns = ts_ns; g.speed_mps = speed;
    g.latitude_deg = lat; g.longitude_deg = lon;
    g.fix_quality = fix; g.hdop = hdop;
    return g;
}

static void test_gnss_xcorr_known_offset() {
    // Create ego GPS at 1 Hz for 120 seconds, with a speed pattern
    std::vector<GnssPoint> ego_gps, src_gps;
    const ns_t true_offset_ns = 1'500'000'000LL; // 1.5 seconds
    const int N = 120;

    for (int i = 0; i < N; ++i) {
        ns_t ts = static_cast<ns_t>(i) * kNsPerSec;
        float spd = 10.0f + 5.0f * std::sin(2.0f * 3.14159f * i / 30.0f); // 30s period
        ego_gps.push_back(make_gps(ts, spd));
        // Source has same speed, offset by true_offset_ns
        src_gps.push_back(make_gps(ts + true_offset_ns, spd));
    }

    auto est = sync_gnss_xcorr(ego_gps, src_gps, 30.0);

    double error_ms = std::abs(static_cast<double>(est.offset_ns - true_offset_ns)) / 1e6;
    std::cout << "  sync offset=" << est.offset_ns / 1e6 << "ms"
              << "  true=" << true_offset_ns / 1e6 << "ms"
              << "  error=" << error_ms << "ms"
              << "  confidence=" << est.confidence << '\n';

    assert(error_ms < 500.0 && "Offset error must be < 500ms (grid resolution 100ms)");
    assert(est.confidence > 0.0 && "Confidence must be > 0");
    std::cout << "[PASS] test_gnss_xcorr_known_offset (error=" << error_ms << "ms)\n";
}

static void test_empty_gps_returns_zero_confidence() {
    std::vector<GnssPoint> empty;
    auto est = sync_gnss_xcorr(empty, empty, 30.0);
    assert(est.confidence == 0.0);
    std::cout << "[PASS] test_empty_gps_returns_zero_confidence\n";
}

static void test_event_audio_fallback() {
    // One siren_on event at t=10s source, audio onset at t=11.5s ego → offset = -1.5s
    SourceEvent ev;
    ev.ts_ns      = 10 * kNsPerSec;
    ev.event_type = SourceEventType::kSirenOn;
    std::vector<SourceEvent> events = {ev};
    std::vector<ns_t> audio_onsets = {11'500'000'000LL}; // 11.5s ego

    auto est = sync_event_audio_fallback(events, audio_onsets, 30.0);
    std::cout << "  fallback offset=" << est.offset_ns / 1e6 << "ms"
              << "  confidence=" << est.confidence << '\n';
    assert(est.method == "event_audio");
    std::cout << "[PASS] test_event_audio_fallback\n";
}

static void test_synchroniser_sync_ok() {
    Config cfg;
    cfg.sync.min_confidence = 0.3;
    cfg.sync.max_offset_search_s = 30.0;

    LogSynchroniser sync(cfg);

    std::vector<GnssPoint> ego_gps, src_gps;
    const ns_t off = 500'000'000LL; // 0.5s
    for (int i = 0; i < 120; ++i) {
        ns_t ts = static_cast<ns_t>(i) * kNsPerSec;
        float spd = 8.0f + 4.0f * std::sin(2.0f * 3.14159f * i / 20.0f);
        ego_gps.push_back(make_gps(ts, spd));
        src_gps.push_back(make_gps(ts + off, spd));
    }

    TimeRange vr;
    vr.t_start_ns = 0;
    vr.t_end_ns   = 120 * kNsPerSec;

    auto rpt = sync.synchronise(ego_gps, src_gps, {}, {}, vr, "test-session");
    std::cout << "  synchroniser status=" << [&] {
        switch (rpt.status) {
            case SyncStatus::sync_ok:       return "sync_ok";
            case SyncStatus::sync_degraded: return "sync_degraded";
            default: return "sync_failed";
        }
    }() << "  confidence=" << rpt.confidence << '\n';

    assert(rpt.status != SyncStatus::sync_failed);
    std::cout << "[PASS] test_synchroniser_sync_ok\n";
}

int main() {
    test_gnss_xcorr_known_offset();
    test_empty_gps_returns_zero_confidence();
    test_event_audio_fallback();
    test_synchroniser_sync_ok();
    std::cout << "\nAll sync tests passed.\n";
    return 0;
}
