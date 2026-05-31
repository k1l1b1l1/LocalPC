#pragma once
// VL-01..08: log_validator — валидация, gap-анализ, valid_ranges_extractor

#include "ego_offline/types.hpp"
#include "ego_offline/config.hpp"
#include "ego_offline/load/ego_log_reader.hpp"
#include "ego_offline/load/source_log_reader.hpp"
#include "ego_offline/parse/packet_demuxer.hpp"
#include "ego_offline/parse/source_demuxer.hpp"

#include <filesystem>
#include <vector>
#include <string>

namespace ego_offline::validate {

// ── Gap event ─────────────────────────────────────────────────────────────────
enum class GapType { seq, timestamp, audio_block, telemetry, file_truncated };
enum class GapSeverity { info, warning, error };

struct GapEvent {
    ns_t        t_ns     = 0;
    std::string stream_id;
    GapType     gap_type = GapType::seq;
    uint32_t    expected_seq = 0;
    uint32_t    actual_seq   = 0;
    ns_t        gap_duration_ns = 0;
    GapSeverity severity = GapSeverity::warning;
};

// ── Per-stream validation result ─────────────────────────────────────────────
struct StreamValidation {
    bool     file_ok              = true;
    uint64_t packets_total        = 0;
    uint64_t packets_valid        = 0;
    uint64_t packets_lost         = 0;
    uint64_t packets_crc_failed   = 0;
    uint64_t timestamp_violations = 0;
    uint64_t monotonic_violations = 0;
    uint64_t audio_gap_count      = 0;
    uint64_t telemetry_gap_count  = 0;
    ns_t     t_first_ns           = 0;
    ns_t     t_last_ns            = 0;
    std::vector<std::string> file_errors;
    std::vector<GapEvent>    gap_events;
};

// ── Full validation report ────────────────────────────────────────────────────
enum class Integrity { ok, warning, failed };

struct ValidationReport {
    std::string  offline_pipeline_version;
    std::string  session_id;
    std::string  generated_at_utc;
    Integrity    integrity    = Integrity::ok;
    StreamValidation ego;
    StreamValidation source;
    std::vector<TimeRange> valid_ranges;

    struct Summary {
        double total_valid_duration_s = 0.0;
        double valid_fraction         = 0.0;
    } summary;

    // Serialize to JSON string (schema validation_report.schema.json)
    std::string to_json() const;

    // Write to file
    void write(const std::filesystem::path& path) const;
};

// ── Validator ─────────────────────────────────────────────────────────────────
class LogValidator {
public:
    explicit LogValidator(const Config& cfg);

    // VL-01..08: validate ego stream
    StreamValidation validate_ego(const load::EgoLogReader& reader) const;

    // VL-01..08: validate source stream
    StreamValidation validate_source(const load::SourceLogReader& reader) const;

    // VL-08: extract valid time ranges from both streams
    std::vector<TimeRange> extract_valid_ranges(
        const StreamValidation& ego_val,
        const StreamValidation& src_val) const;

    // Build full ValidationReport
    ValidationReport build_report(
        const std::string& session_id,
        const StreamValidation& ego_val,
        const StreamValidation& src_val) const;

private:
    Config cfg_;

    // Checks a single time series for monotonicity violations
    void check_timestamps(const std::vector<ns_t>& timestamps,
                          const std::string& stream_id,
                          StreamValidation& out) const;

    // Detect gaps in a sorted seq series
    void check_seq_gaps(const std::vector<std::pair<seq_t, ns_t>>& seq_ts,
                        const std::string& stream_id,
                        StreamValidation& out) const;

    Integrity compute_integrity(const StreamValidation& ego_val,
                                const StreamValidation& src_val,
                                const std::vector<TimeRange>& ranges) const;
};

} // namespace ego_offline::validate
