#include "ego_offline/validate/log_validator.hpp"
#include "ego_offline/parse/binary_parser.hpp"
#include "ego_offline/checksum.hpp"
#include "ego_offline/json_writer.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <stdexcept>
#include <cmath>

namespace ego_offline::validate {

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

const char* gap_type_str(GapType t) {
    switch (t) {
        case GapType::seq:           return "seq";
        case GapType::timestamp:     return "timestamp";
        case GapType::audio_block:   return "audio_block";
        case GapType::telemetry:     return "telemetry";
        case GapType::file_truncated: return "file_truncated";
    }
    return "seq";
}

const char* gap_sev_str(GapSeverity s) {
    switch (s) {
        case GapSeverity::info:    return "info";
        case GapSeverity::warning: return "warning";
        case GapSeverity::error:   return "error";
    }
    return "warning";
}

const char* integrity_str(Integrity i) {
    switch (i) {
        case Integrity::ok:      return "ok";
        case Integrity::warning: return "warning";
        case Integrity::failed:  return "failed";
    }
    return "failed";
}

} // namespace

LogValidator::LogValidator(const Config& cfg) : cfg_(cfg) {}

StreamValidation LogValidator::validate_ego(const load::EgoLogReader& reader) const {
    StreamValidation sv;
    parse::BinaryParser parser;

    // Check file existence
    for (const auto& chunk : reader.manifest().chunks) {
        if (!std::filesystem::exists(chunk.path)) {
            sv.file_ok = false;
            sv.file_errors.push_back("Missing chunk: " + chunk.path.string());
        }
    }
    if (!sv.file_ok) return sv;

    // Track state for gap detection
    seq_t    prev_seq   = 0;
    bool     first_pkt  = true;
    ns_t     prev_ts    = 0;
    ns_t     prev_audio_ts = 0;
    uint32_t nominal_audio_period_ns = 0; // computed from first audio block
    ns_t     prev_imu_ts = 0;

    std::vector<std::pair<seq_t, ns_t>> seq_ts_list;

    reader.scan([&](const load::RawPacket& raw) {
        ++sv.packets_total;

        auto res = parse::BinaryParser::validate(raw);
        if (!res.ok()) {
            if (res.status == parse::ParseStatus::crc_fail) {
                ++sv.packets_crc_failed;
            }
            ++sv.packets_lost;
            GapEvent g;
            g.t_ns       = static_cast<ns_t>(raw.header.ts_ns);
            g.stream_id  = "ego";
            g.gap_type   = GapType::file_truncated;
            g.severity   = GapSeverity::error;
            sv.gap_events.push_back(g);
            return true;
        }
        ++sv.packets_valid;

        ns_t ts = static_cast<ns_t>(raw.header.ts_ns);

        // VL-03: ts_ns == 0 check
        if (ts == 0) {
            ++sv.timestamp_violations;
            GapEvent g; g.t_ns = ts; g.stream_id = "ego";
            g.gap_type = GapType::timestamp; g.severity = GapSeverity::error;
            sv.gap_events.push_back(g);
        }

        // VL-04: monotonicity
        if (!first_pkt && ts < prev_ts) {
            ++sv.monotonic_violations;
            GapEvent g; g.t_ns = ts; g.stream_id = "ego";
            g.gap_type = GapType::timestamp; g.severity = GapSeverity::warning;
            g.gap_duration_ns = prev_ts - ts;
            sv.gap_events.push_back(g);
        }

        // VL-03: timestamp jump > max
        if (!first_pkt) {
            ns_t jump = ts - prev_ts;
            if (jump > static_cast<ns_t>(cfg_.max_timestamp_jump_ms) * kNsPerMs) {
                ++sv.timestamp_violations;
                GapEvent g; g.t_ns = ts; g.stream_id = "ego";
                g.gap_type = GapType::timestamp; g.severity = GapSeverity::warning;
                g.gap_duration_ns = jump;
                sv.gap_events.push_back(g);
            }
        }

        // Update first/last
        if (first_pkt || ts < sv.t_first_ns) sv.t_first_ns = ts;
        if (ts > sv.t_last_ns)               sv.t_last_ns  = ts;

        // VL-02 seq gap detection (accumulate, check after)
        seq_ts_list.push_back({raw.header.seq, ts});

        // VL-05 audio gap check
        if (raw.header.type == static_cast<uint16_t>(EgoPacketType::kAudioBlock)
            && raw.payload.size() >= sizeof(AudioBlockHeader)) {
            AudioBlockHeader ahdr{};
            std::memcpy(&ahdr, raw.payload.data(), sizeof(ahdr));
            ns_t period_ns = static_cast<ns_t>(ahdr.frame_count) * kNsPerSec / ahdr.sample_rate_hz;
            if (nominal_audio_period_ns == 0) nominal_audio_period_ns = static_cast<uint32_t>(period_ns);

            if (prev_audio_ts != 0 && nominal_audio_period_ns > 0) {
                ns_t gap = ts - prev_audio_ts;
                ns_t thresh = static_cast<ns_t>(cfg_.audio_gap_factor * nominal_audio_period_ns);
                if (gap > thresh) {
                    ++sv.audio_gap_count;
                    GapEvent g; g.t_ns = ts; g.stream_id = "ego.audio";
                    g.gap_type = GapType::audio_block; g.severity = GapSeverity::warning;
                    g.gap_duration_ns = gap;
                    sv.gap_events.push_back(g);
                }
            }
            prev_audio_ts = ts;
        }

        // VL-06 telemetry (IMU) gap check
        if (raw.header.type == static_cast<uint16_t>(EgoPacketType::kImuSample)) {
            if (prev_imu_ts != 0) {
                ns_t gap = ts - prev_imu_ts;
                if (gap > static_cast<ns_t>(cfg_.telemetry_gap_ms) * kNsPerMs) {
                    ++sv.telemetry_gap_count;
                    GapEvent g; g.t_ns = ts; g.stream_id = "ego.imu";
                    g.gap_type = GapType::telemetry; g.severity = GapSeverity::warning;
                    g.gap_duration_ns = gap;
                    sv.gap_events.push_back(g);
                }
            }
            prev_imu_ts = ts;
        }

        prev_ts  = ts;
        first_pkt = false;
        return true;
    });

    // VL-02: seq gap analysis
    check_seq_gaps(seq_ts_list, "ego", sv);

    // VL-01: compare with manifest
    if (reader.manifest().packet_count > 0
        && sv.packets_valid < reader.manifest().packet_count) {
        uint64_t diff = reader.manifest().packet_count - sv.packets_valid;
        sv.packets_lost += diff;
    }

    return sv;
}

StreamValidation LogValidator::validate_source(const load::SourceLogReader& reader) const {
    StreamValidation sv;

    for (const auto& chunk : reader.manifest().chunks) {
        if (!std::filesystem::exists(chunk.path)) {
            sv.file_ok = false;
            sv.file_errors.push_back("Missing source file: " + chunk.path.string());
        }
    }
    if (!sv.file_ok) return sv;

    bool first_pkt = true;
    ns_t prev_ts   = 0;
    seq_t prev_seq = 0;
    std::vector<std::pair<seq_t, ns_t>> seq_ts_list;

    reader.scan([&](const load::RawPacket& raw) {
        ++sv.packets_total;
        auto res = parse::BinaryParser::validate(raw);
        if (!res.ok()) {
            if (res.status == parse::ParseStatus::crc_fail) ++sv.packets_crc_failed;
            ++sv.packets_lost;
            return true;
        }
        ++sv.packets_valid;

        ns_t ts = static_cast<ns_t>(raw.header.ts_ns);
        if (ts == 0) ++sv.timestamp_violations;
        if (!first_pkt && ts < prev_ts) {
            ++sv.monotonic_violations;
        }
        if (first_pkt || ts < sv.t_first_ns) sv.t_first_ns = ts;
        if (ts > sv.t_last_ns)               sv.t_last_ns  = ts;

        seq_ts_list.push_back({raw.header.seq, ts});
        prev_ts = ts;
        first_pkt = false;
        return true;
    });

    check_seq_gaps(seq_ts_list, "source", sv);
    return sv;
}

void LogValidator::check_seq_gaps(const std::vector<std::pair<seq_t, ns_t>>& seq_ts,
                                   const std::string& stream_id,
                                   StreamValidation& out) const {
    if (seq_ts.size() < 2) return;
    for (size_t i = 1; i < seq_ts.size(); ++i) {
        seq_t expected = seq_ts[i-1].first + 1;
        seq_t actual   = seq_ts[i].first;
        if (actual != expected && actual > expected) {
            uint32_t lost = actual - expected;
            out.packets_lost += lost;
            GapEvent g;
            g.t_ns        = seq_ts[i].second;
            g.stream_id   = stream_id;
            g.gap_type    = GapType::seq;
            g.expected_seq = expected;
            g.actual_seq   = actual;
            g.gap_duration_ns = seq_ts[i].second - seq_ts[i-1].second;
            g.severity    = (lost > 10) ? GapSeverity::error : GapSeverity::warning;
            out.gap_events.push_back(g);
        }
    }
}

void LogValidator::check_timestamps(const std::vector<ns_t>& ts,
                                     const std::string& stream_id,
                                     StreamValidation& out) const {
    for (size_t i = 1; i < ts.size(); ++i) {
        if (ts[i] < ts[i-1]) ++out.monotonic_violations;
    }
}

std::vector<TimeRange> LogValidator::extract_valid_ranges(
    const StreamValidation& ego_val,
    const StreamValidation& src_val) const {

    // Build valid ranges: period [t_first, t_last] for each stream,
    // split around error-severity gap events.
    auto build_ranges = [&](const StreamValidation& sv, Stream s) {
        std::vector<TimeRange> ranges;
        if (!sv.file_ok || sv.t_last_ns <= sv.t_first_ns) return ranges;

        // Collect error breakpoints
        std::vector<ns_t> breaks;
        for (const auto& g : sv.gap_events) {
            if (g.severity == GapSeverity::error) breaks.push_back(g.t_ns);
        }
        std::sort(breaks.begin(), breaks.end());

        ns_t cur_start = sv.t_first_ns;
        for (ns_t bp : breaks) {
            if (bp <= cur_start) continue;
            TimeRange r; r.t_start_ns = cur_start; r.t_end_ns = bp; r.stream = s;
            if (r.duration_s() >= cfg_.validation.min_valid_duration_s)
                ranges.push_back(r);
            cur_start = bp;
        }
        // Final segment
        TimeRange r; r.t_start_ns = cur_start; r.t_end_ns = sv.t_last_ns; r.stream = s;
        if (r.duration_s() >= cfg_.validation.min_valid_duration_s)
            ranges.push_back(r);
        return ranges;
    };

    auto ego_ranges = build_ranges(ego_val, Stream::ego);
    auto src_ranges = build_ranges(src_val, Stream::source);

    // Intersect to produce "both" ranges
    std::vector<TimeRange> combined;
    for (const auto& er : ego_ranges) {
        for (const auto& sr : src_ranges) {
            ns_t t0 = std::max(er.t_start_ns, sr.t_start_ns);
            ns_t t1 = std::min(er.t_end_ns, sr.t_end_ns);
            if (t1 > t0) {
                TimeRange r; r.t_start_ns = t0; r.t_end_ns = t1; r.stream = Stream::both;
                if (r.duration_s() >= cfg_.validation.min_valid_duration_s)
                    combined.push_back(r);
            }
        }
    }

    // If source is absent, use ego-only ranges
    if (combined.empty() && !ego_ranges.empty()) {
        combined = ego_ranges;
    }
    return combined;
}

Integrity LogValidator::compute_integrity(const StreamValidation& ego_val,
                                          const StreamValidation& src_val,
                                          const std::vector<TimeRange>& ranges) const {
    if (!ego_val.file_ok) return Integrity::failed;

    // Check if any valid range exceeds minimum
    double total_valid = 0.0;
    for (const auto& r : ranges) total_valid += r.duration_s();

    if (total_valid < cfg_.validation.min_valid_duration_s)
        return Integrity::failed;

    bool has_errors = false;
    for (const auto& g : ego_val.gap_events)
        if (g.severity == GapSeverity::error) { has_errors = true; break; }
    for (const auto& g : src_val.gap_events)
        if (g.severity == GapSeverity::error) { has_errors = true; break; }

    if (ego_val.packets_crc_failed > 0 || ego_val.monotonic_violations > 5)
        return Integrity::warning;

    return has_errors ? Integrity::warning : Integrity::ok;
}

ValidationReport LogValidator::build_report(
    const std::string& session_id,
    const StreamValidation& ego_val,
    const StreamValidation& src_val) const {

    ValidationReport rpt;
    rpt.offline_pipeline_version = cfg_.offline_pipeline_version;
    rpt.session_id               = session_id;
    rpt.generated_at_utc         = utc_now_iso8601();
    rpt.ego                      = ego_val;
    rpt.source                   = src_val;
    rpt.valid_ranges             = extract_valid_ranges(ego_val, src_val);
    rpt.integrity                = compute_integrity(ego_val, src_val, rpt.valid_ranges);

    double total_dur = (ego_val.t_last_ns - ego_val.t_first_ns) / 1e9;
    double valid_dur = 0.0;
    for (const auto& r : rpt.valid_ranges) valid_dur += r.duration_s();
    rpt.summary.total_valid_duration_s = valid_dur;
    rpt.summary.valid_fraction = (total_dur > 0) ? (valid_dur / total_dur) : 0.0;
    return rpt;
}

// ── JSON serialization ────────────────────────────────────────────────────────

static std::string stream_validation_json(const StreamValidation& sv, int depth) {
    using namespace json;
    ObjectBuilder b(2, depth);
    b.field("file_ok",              boolean(sv.file_ok))
     .field("packets_total",        num(static_cast<int64_t>(sv.packets_total)))
     .field("packets_valid",        num(static_cast<int64_t>(sv.packets_valid)))
     .field("packets_lost",         num(static_cast<int64_t>(sv.packets_lost)))
     .field("packets_crc_failed",   num(static_cast<int64_t>(sv.packets_crc_failed)))
     .field("timestamp_violations", num(static_cast<int64_t>(sv.timestamp_violations)))
     .field("monotonic_violations", num(static_cast<int64_t>(sv.monotonic_violations)))
     .field("audio_gap_count",      num(static_cast<int64_t>(sv.audio_gap_count)))
     .field("telemetry_gap_count",  num(static_cast<int64_t>(sv.telemetry_gap_count)))
     .field("t_first_ns",           num(static_cast<int64_t>(sv.t_first_ns)))
     .field("t_last_ns",            num(static_cast<int64_t>(sv.t_last_ns)));

    // file_errors
    {
        std::vector<std::string> errs;
        for (const auto& e : sv.file_errors) errs.push_back(json::str(e));
        b.array("file_errors", errs);
    }

    // gap_events (first 100)
    {
        std::vector<std::string> gaps;
        size_t limit = std::min(sv.gap_events.size(), size_t{100});
        for (size_t i = 0; i < limit; ++i) {
            const auto& g = sv.gap_events[i];
            ObjectBuilder gb(2, depth + 2);
            gb.field("t_ns",            num(static_cast<int64_t>(g.t_ns)))
              .field("stream_id",       json::str(g.stream_id))
              .field("gap_type",        json::str(gap_type_str(g.gap_type)))
              .field("expected_seq",    num(static_cast<int64_t>(g.expected_seq)))
              .field("actual_seq",      num(static_cast<int64_t>(g.actual_seq)))
              .field("gap_duration_ns", num(static_cast<int64_t>(g.gap_duration_ns)))
              .field("severity",        json::str(gap_sev_str(g.severity)));
            gaps.push_back(gb.build());
        }
        b.raw_array("gap_events", gaps);
    }
    return b.build();
}

std::string ValidationReport::to_json() const {
    using namespace json;
    ObjectBuilder root(2, 0);
    root.field("offline_pipeline_version", str(offline_pipeline_version))
        .field("session_id",               str(session_id))
        .field("generated_at_utc",         str(generated_at_utc))
        .field("integrity",                str(integrity_str(integrity)));

    // ego
    root.field("ego", stream_validation_json(ego, 1));

    // source
    root.field("source", stream_validation_json(source, 1));

    // valid_ranges
    {
        std::vector<std::string> rng_objs;
        for (const auto& r : valid_ranges) {
            ObjectBuilder rb(2, 2);
            rb.field("t_start_ns", num(static_cast<int64_t>(r.t_start_ns)))
              .field("t_end_ns",   num(static_cast<int64_t>(r.t_end_ns)))
              .field("stream",     str(r.stream == Stream::ego ? "ego" :
                                      r.stream == Stream::source ? "source" : "both"))
              .field("reason",     str(r.reason));
            rng_objs.push_back(rb.build());
        }
        root.raw_array("valid_ranges", rng_objs);
    }

    // summary
    root.object("summary", [this](ObjectBuilder& sb) {
        sb.field("total_valid_duration_s", num(summary.total_valid_duration_s, 3))
          .field("valid_fraction",         num(summary.valid_fraction, 4));
    });

    return root.build();
}

void ValidationReport::write(const std::filesystem::path& path) const {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path);
    if (!f) throw std::runtime_error("Cannot write: " + path.string());
    f << to_json() << '\n';
}

} // namespace ego_offline::validate
