// Segment classifier unit tests (FN-05)

#include "ego_offline/finalize/scene_builder.hpp"
#include "ego_offline/types.hpp"

#include <cassert>
#include <iostream>
#include <vector>

using namespace ego_offline;
using namespace ego_offline::finalize;
using namespace ego_offline::scene;

static SceneGeometry make_geom(ns_t ts_ns, double range_m, double closing_mps, bool valid = true) {
    SceneGeometry g;
    g.ts_ns             = ts_ns;
    g.range_m           = range_m;
    g.closing_speed_mps = closing_mps;
    g.valid             = valid;
    return g;
}

// Build a linear approach: range decreasing 100→10m over 10s
static std::vector<SceneGeometry> make_approach(int steps = 100) {
    std::vector<SceneGeometry> g;
    for (int i = 0; i < steps; ++i) {
        ns_t ts  = static_cast<ns_t>(i) * 100'000'000LL; // 100ms steps
        double r = 100.0 - 90.0 * i / (steps - 1);
        g.push_back(make_geom(ts, r, 9.0)); // positive closing
    }
    return g;
}

// Recede: range increasing 10→100m
static std::vector<SceneGeometry> make_recede(int steps = 100) {
    std::vector<SceneGeometry> g;
    for (int i = 0; i < steps; ++i) {
        ns_t ts  = static_cast<ns_t>(i) * 100'000'000LL;
        double r = 10.0 + 90.0 * i / (steps - 1);
        g.push_back(make_geom(ts, r, -9.0)); // negative closing = receding
    }
    return g;
}

// Pass-by: range 100→10→100
static std::vector<SceneGeometry> make_pass_by(int steps = 100) {
    std::vector<SceneGeometry> g;
    for (int i = 0; i < steps; ++i) {
        ns_t ts  = static_cast<ns_t>(i) * 100'000'000LL;
        double t = i / static_cast<double>(steps - 1);
        double r = 100.0 - 90.0 * (1.0 - std::abs(2.0 * t - 1.0));
        double cs = (t < 0.5) ? 9.0 : -9.0;
        g.push_back(make_geom(ts, r, cs));
    }
    return g;
}

// Stationary
static std::vector<SceneGeometry> make_stationary(int steps = 100) {
    std::vector<SceneGeometry> g;
    for (int i = 0; i < steps; ++i) {
        ns_t ts = static_cast<ns_t>(i) * 100'000'000LL;
        g.push_back(make_geom(ts, 50.0, 0.1)); // tiny closing speed
    }
    return g;
}

static void test_classify_approach() {
    auto g = make_approach();
    auto cls = classify_segment(g, 1.0, false);
    assert(cls == SegmentClass::approach);
    std::cout << "[PASS] test_classify_approach\n";
}

static void test_classify_recede() {
    auto g = make_recede();
    auto cls = classify_segment(g, 1.0, false);
    assert(cls == SegmentClass::recede);
    std::cout << "[PASS] test_classify_recede\n";
}

static void test_classify_pass_by() {
    auto g = make_pass_by();
    auto cls = classify_segment(g, 1.0, false);
    assert(cls == SegmentClass::pass_by);
    std::cout << "[PASS] test_classify_pass_by\n";
}

static void test_classify_stationary() {
    auto g = make_stationary();
    auto cls = classify_segment(g, 1.0, false);
    assert(cls == SegmentClass::stationary);
    std::cout << "[PASS] test_classify_stationary\n";
}

static void test_empty_returns_unknown() {
    std::vector<SceneGeometry> g;
    auto cls = classify_segment(g, 1.0, true);
    assert(cls == SegmentClass::unknown);
    std::cout << "[PASS] test_empty_returns_unknown\n";
}

static void test_short_segment_unknown() {
    auto g = make_approach(5); // only 5 × 100ms = 0.5s < 1.0s min
    auto cls = classify_segment(g, 1.0, false);
    assert(cls == SegmentClass::unknown);
    std::cout << "[PASS] test_short_segment_unknown\n";
}

int main() {
    test_classify_approach();
    test_classify_recede();
    test_classify_pass_by();
    test_classify_stationary();
    test_empty_returns_unknown();
    test_short_segment_unknown();
    std::cout << "\nAll segment_classifier tests passed.\n";
    return 0;
}
