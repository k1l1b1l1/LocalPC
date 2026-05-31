#include "ego_offline/mdf4/mdf4_writer.hpp"
#include "ego_offline/mdf4/mdf4_channel_builder.hpp"
#include "ego_offline/mdf4/mdf4_lib_adapter.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace ego_offline;
using namespace ego_offline::mdf4;

// ── Synthetic data helpers ────────────────────────────────────────────────────

static align::AlignedSample make_sample(int i, ns_t t0) {
    align::AlignedSample s;
    s.t_ns        = t0 + (ns_t)i * 10'000'000LL; // 10 ms
    s.ego_lat     = 55.0 + i * 0.0001;
    s.ego_lon     = 37.0 + i * 0.0001;
    s.ego_alt     = 200.0;
    s.ego_speed   = 10.f + i * 0.1f;
    s.ego_heading = 90.f;
    s.ego_fix     = 4;
    s.src_lat     = 55.01 + i * 0.0001;
    s.src_lon     = 37.01 + i * 0.0001;
    s.src_speed   = 8.f;
    s.src_valid   = true;
    s.acc_x = 0.1f; s.acc_y = 0.0f; s.acc_z = 9.8f;
    s.gyr_x = 0.0f; s.gyr_y = 0.0f; s.gyr_z = 0.01f;
    s.imu_valid   = true;
    s.traj_lat    = s.ego_lat;
    s.traj_lon    = s.ego_lon;
    s.roll = 0.f; s.pitch = 1.f; s.yaw = 90.f;
    s.traj_valid  = true;
    return s;
}

static SceneGeometry make_scene(int i, ns_t t0) {
    SceneGeometry sg;
    sg.ts_ns = t0 + (ns_t)i * 10'000'000LL;
    sg.range_m           = 100.0 - i;
    sg.azimuth_deg       = 5.0;
    sg.closing_speed_mps = 2.0;
    sg.valid             = true;
    return sg;
}

static scene::StepMetrics make_metrics(int i, ns_t t0) {
    scene::StepMetrics sm;
    sm.t_ns            = t0 + (ns_t)i * 10'000'000LL;
    sm.ego_speed_mps   = 10.0 + i * 0.1;
    sm.src_speed_mps   = 8.0;
    sm.valid           = true;
    return sm;
}

// ── UT-MF-01: channel builder produces correct array lengths ─────────────────
static void test_channel_builder_lengths() {
    constexpr int N = 100;
    ns_t t0 = 1'700'000'000'000'000'000LL;

    std::vector<align::AlignedSample> samples;
    std::vector<SceneGeometry>        scene;
    std::vector<scene::StepMetrics>   metrics;
    for(int i=0;i<N;++i){
        samples.push_back(make_sample(i,t0));
        scene.push_back(make_scene(i,t0));
        metrics.push_back(make_metrics(i,t0));
    }

    load::SessionBundle meta;
    meta.session.session_id = "test-session";
    meta.session.vehicle_id = "test-vehicle";

    Config cfg;
    Mdf4Writer writer(cfg);
    writer.prepare(samples, scene, metrics, meta, t0);

    const auto& groups = writer.groups();
    assert(!groups.empty());

    for(const auto& g : groups){
        assert(g.time_axis_s.size() == (size_t)N ||
               g.name == "Scene" || g.name == "Scene.Metrics");
        for(const auto& ch : g.channels){
            assert(ch.data.size() == g.time_axis_s.size() &&
                   "channel data length mismatch");
            assert(ch.sample_count == ch.data.size());
        }
    }

    // Spot-check values
    for(const auto& g : groups){
        if(g.name == "Ego.GPS"){
            // latitude should be monotonically increasing
            for(size_t i=1;i<g.time_axis_s.size();++i)
                assert(g.channels[0].data[i] > g.channels[0].data[i-1]);
        }
        if(g.name == "Ego.IMU"){
            // acc_z should be ~9.8
            assert(std::abs(g.channels[2].data[0] - 9.8) < 0.01);
        }
    }

    std::cout << "PASS: test_channel_builder_lengths\n";
}

// ── UT-MF-02: write produces a valid MDF4 file ───────────────────────────────
static void test_write_file_exists_and_size() {
    constexpr int N = 10;
    ns_t t0 = 1'700'000'000'000'000'000LL;

    std::vector<align::AlignedSample> samples;
    std::vector<SceneGeometry>        scene;
    std::vector<scene::StepMetrics>   metrics;
    for(int i=0;i<N;++i){
        samples.push_back(make_sample(i,t0));
        scene.push_back(make_scene(i,t0));
        metrics.push_back(make_metrics(i,t0));
    }

    load::SessionBundle meta;
    meta.session.session_id = "test-mf02";

    Config cfg;
    Mdf4Writer writer(cfg);
    writer.prepare(samples, scene, metrics, meta, t0);

    auto tmp_dir = fs::temp_directory_path() / "ego_offline_mdf4_test";
    fs::create_directories(tmp_dir);
    auto out = tmp_dir / "test.mf4";
    fs::remove(out);

    bool ok = writer.write(out);
    assert(ok && "write() returned false");
    assert(fs::exists(out) && "output file not created");
    assert(fs::file_size(out) > 1024 && "file too small (< 1 KiB)");

    // Verify MDF4 magic — close file before cleanup (Windows requires this)
    {
        std::ifstream f(out, std::ios::binary);
        char magic[8] = {};
        f.read(magic,8);
        assert(std::strncmp(magic,"MDF     ",8)==0 && "MDF4 magic mismatch");
    }

    std::cout << "PASS: test_write_file_exists_and_size  size="
              << fs::file_size(out) << " bytes\n";

    // Cleanup
    fs::remove_all(tmp_dir);
}

// ── UT-MF-03: validate() creates SHA256 sidecar ──────────────────────────────
static void test_validate_sha256_sidecar() {
    constexpr int N = 20;
    ns_t t0 = 1'700'000'000'000'000'000LL;

    std::vector<align::AlignedSample> samples;
    for(int i=0;i<N;++i) samples.push_back(make_sample(i,t0));

    load::SessionBundle meta;
    meta.session.session_id = "test-mf03";

    Config cfg;
    Mdf4Writer writer(cfg);
    writer.prepare(samples, {}, {}, meta, t0);

    auto tmp_dir = fs::temp_directory_path() / "ego_offline_mdf4_sha_test";
    fs::create_directories(tmp_dir);
    auto out = tmp_dir / "session.mf4";
    fs::remove(out);

    assert(writer.write(out));
    bool valid = writer.validate(out);
    assert(valid && "validate() returned false");

    auto sidecar = out;
    sidecar += ".sha256";
    assert(fs::exists(sidecar) && "SHA256 sidecar not created");

    std::string hex, hex2;

    // SHA256 hex is 64 chars — use nested scope to close files before cleanup
    {
        std::ifstream sf(sidecar);
        sf >> hex;
    }
    assert(hex.size() == 64 && "SHA256 hex string wrong length");
    for(char c : hex)
        assert(std::isxdigit((unsigned char)c) && "SHA256 has non-hex character");

    // Idempotency: second write produces same hash (NFR-03)
    auto out2 = tmp_dir / "session2.mf4";
    writer.write(out2);
    auto sidecar2 = out2;
    sidecar2 += ".sha256";
    write_sha256_sidecar(out2);
    {
        std::ifstream sf2(sidecar2);
        sf2 >> hex2;
    }
    assert(hex == hex2 && "NFR-03: SHA256 not deterministic for same input");

    std::cout << "PASS: test_validate_sha256_sidecar  sha256=" << hex.substr(0,16) << "...\n";

    fs::remove_all(tmp_dir);
}

// ── UT-MF-04: NaN handling for invalid IMU samples ───────────────────────────
static void test_nan_for_invalid_samples() {
    constexpr int N = 5;
    ns_t t0 = 1'700'000'000'000'000'000LL;

    std::vector<align::AlignedSample> samples;
    for(int i=0;i<N;++i){
        auto s = make_sample(i,t0);
        if(i % 2 == 0) s.imu_valid = false; // mark even samples as invalid IMU
        samples.push_back(s);
    }

    load::SessionBundle meta;
    meta.session.session_id = "test-nan";
    Config cfg;
    Mdf4Writer writer(cfg);
    writer.prepare(samples,{},{},meta,t0);

    for(const auto& g : writer.groups()){
        if(g.name != "Ego.IMU") continue;
        for(int i=0;i<N;++i){
            if(i % 2 == 0)
                assert(std::isnan(g.channels[0].data[i]) && "expected NaN for invalid IMU");
            else
                assert(!std::isnan(g.channels[0].data[i]) && "unexpected NaN for valid IMU");
        }
    }
    std::cout << "PASS: test_nan_for_invalid_samples\n";
}

// ── UT-MF-05: DGBLOCK data link points to ##DT, comment link is not ##DT ─────
static void test_dg_block_links() {
    constexpr int N = 5;
    ns_t t0 = 1'700'000'000'000'000'000LL;

    std::vector<align::AlignedSample> samples;
    for(int i=0;i<N;++i) samples.push_back(make_sample(i,t0));

    load::SessionBundle meta;
    meta.session.session_id = "test-links";

    Config cfg;
    Mdf4Writer writer(cfg);
    writer.prepare(samples, {}, {}, meta, t0);

    auto tmp_dir = fs::temp_directory_path() / "ego_offline_mdf4_links_test";
    fs::create_directories(tmp_dir);
    auto out = tmp_dir / "links.mf4";
    fs::remove(out);
    assert(writer.write(out));

    std::vector<uint8_t> bytes;
    {
        std::ifstream f(out, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(f), {});
    }

    auto read_u64 = [&](size_t off) -> uint64_t {
        uint64_t v = 0;
        for(int i=0;i<8;++i) v |= uint64_t(bytes[off+i]) << (8*i);
        return v;
    };

    auto block_id = [&](size_t off) -> std::string {
        if(off + 4 > bytes.size()) return "";
        return {char(bytes[off]), char(bytes[off+1]), char(bytes[off+2]), char(bytes[off+3])};
    };

    // IDBLOCK=64, HDBLOCK at 64; links start at block+24
    assert(block_id(64) == "##HD");
    size_t hd_links = 64 + 24;
    uint64_t first_dg = read_u64(hd_links);
    uint64_t hd_comment = read_u64(hd_links + 40);
    assert(first_dg > 0);
    assert(block_id(first_dg) == "##DG");
    assert(hd_comment > 0);
    assert(block_id(hd_comment) == "##MD");

    size_t fh_off = read_u64(hd_links + 8);
    assert(fh_off > 0);
    assert(block_id(fh_off) == "##FH");
    size_t fh_links = fh_off + 24;
    assert(read_u64(fh_links) == 0); // fh link 1 = next FH
    assert(read_u64(fh_links + 8) == hd_comment); // fh link 2 = comment

    size_t dg_links = first_dg + 24;
    uint64_t dt_addr = read_u64(dg_links + 16);    // link 3 = data
    uint64_t dg_comment = read_u64(dg_links + 24); // link 4 = comment
    assert(dt_addr > 0);
    assert(block_id(dt_addr) == "##DT");
    assert(dg_comment == 0 && "DG comment link must not alias DT block");

    std::cout << "PASS: test_dg_block_links\n";
    fs::remove_all(tmp_dir);
}

// ─────────────────────────────────────────────────────────────────────────────

int main() {
    test_channel_builder_lengths();
    test_write_file_exists_and_size();
    test_validate_sha256_sidecar();
    test_nan_for_invalid_samples();
    test_dg_block_links();
    std::cout << "All MDF4 tests passed.\n";
    return 0;
}
