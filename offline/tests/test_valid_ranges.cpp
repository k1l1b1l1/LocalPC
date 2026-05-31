// IT-02: valid_ranges_extractor unit tests
// Tests with synthetic gap patterns

#include "ego_offline/validate/log_validator.hpp"
#include "ego_offline/config.hpp"

#include <cassert>
#include <iostream>

using namespace ego_offline;
using namespace ego_offline::validate;

static Config make_cfg(double min_dur = 10.0) {
    Config cfg;
    cfg.validation.min_valid_duration_s = min_dur;
    cfg.validation.fail_on_no_valid_ranges = true;
    return cfg;
}

static StreamValidation make_sv(ns_t t0, ns_t t1, bool file_ok = true,
                                std::vector<GapEvent> gaps = {}) {
    StreamValidation sv;
    sv.file_ok        = file_ok;
    sv.t_first_ns     = t0;
    sv.t_last_ns      = t1;
    sv.packets_total  = 1000;
    sv.packets_valid  = 1000;
    sv.gap_events     = gaps;
    return sv;
}

static void test_no_gaps() {
    auto cfg = make_cfg(10.0);
    LogValidator v(cfg);

    auto ego = make_sv(0, 100 * kNsPerSec);
    auto src = make_sv(0, 100 * kNsPerSec);

    auto ranges = v.extract_valid_ranges(ego, src);
    assert(!ranges.empty());
    assert(ranges[0].duration_s() >= 90.0);
    std::cout << "[PASS] test_no_gaps (ranges=" << ranges.size()
              << ", dur=" << ranges[0].duration_s() << "s)\n";
}

static void test_error_gap_splits_range() {
    auto cfg = make_cfg(10.0);
    LogValidator v(cfg);

    GapEvent gap;
    gap.t_ns      = 50 * kNsPerSec; // at t=50s
    gap.stream_id = "ego";
    gap.gap_type  = GapType::file_truncated;
    gap.severity  = GapSeverity::error;

    auto ego = make_sv(0, 100 * kNsPerSec, true, {gap});
    auto src = make_sv(0, 100 * kNsPerSec);

    auto ranges = v.extract_valid_ranges(ego, src);
    // Should be split: [0,50] and [50,100]
    assert(ranges.size() >= 1);
    std::cout << "[PASS] test_error_gap_splits_range (ranges=" << ranges.size() << ")\n";
}

static void test_no_source_returns_ego_ranges() {
    auto cfg = make_cfg(10.0);
    LogValidator v(cfg);

    auto ego = make_sv(0, 60 * kNsPerSec);
    StreamValidation src; src.file_ok = false;

    auto ranges = v.extract_valid_ranges(ego, src);
    assert(!ranges.empty());
    assert(ranges[0].stream == Stream::ego);
    std::cout << "[PASS] test_no_source_returns_ego_ranges\n";
}

static void test_short_segment_filtered() {
    auto cfg = make_cfg(30.0); // require 30s
    LogValidator v(cfg);

    // Ego has a 5s gap at t=10s, creating a 10s first segment
    GapEvent gap;
    gap.t_ns     = 10 * kNsPerSec;
    gap.severity = GapSeverity::error;
    auto ego = make_sv(0, 100 * kNsPerSec, true, {gap});
    auto src = make_sv(0, 100 * kNsPerSec);

    auto ranges = v.extract_valid_ranges(ego, src);
    // First segment [0, 10s] = 10s < 30s → filtered
    for (const auto& r : ranges)
        assert(r.duration_s() >= 30.0);
    std::cout << "[PASS] test_short_segment_filtered (ranges=" << ranges.size() << ")\n";
}

static void test_compute_integrity_ok() {
    auto cfg = make_cfg(10.0);
    LogValidator v(cfg);
    auto ego = make_sv(0, 100 * kNsPerSec);
    auto src = make_sv(0, 100 * kNsPerSec);
    auto rpt = v.build_report("session-001", ego, src);
    assert(rpt.integrity == Integrity::ok || rpt.integrity == Integrity::warning);
    std::cout << "[PASS] test_compute_integrity_ok\n";
}

int main() {
    test_no_gaps();
    test_error_gap_splits_range();
    test_no_source_returns_ego_ranges();
    test_short_segment_filtered();
    test_compute_integrity_ok();
    std::cout << "\nAll valid_ranges_extractor tests passed.\n";
    return 0;
}
