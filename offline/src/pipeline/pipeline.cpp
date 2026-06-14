#include "ego_offline/pipeline/pipeline.hpp"

#include "ego_offline/checksum.hpp"
#include "ego_offline/ego_contract_protocol.hpp"
#include "ego_offline/load/ego_log_reader.hpp"
#include "ego_offline/load/ego_nav_sidecar_reader.hpp"
#include "ego_offline/load/source_log_reader.hpp"
#include "ego_offline/load/metadata_loader.hpp"
#include "ego_offline/parse/binary_parser.hpp"
#include "ego_offline/parse/contract_demuxer.hpp"
#include "ego_offline/parse/packet_demuxer.hpp"
#include "ego_offline/parse/source_demuxer.hpp"
#include "ego_offline/validate/log_validator.hpp"
#include "ego_offline/sync/time_sync.hpp"
#include "ego_offline/align/time_aligner.hpp"
#include "ego_offline/scene/scene_geometry.hpp"
#include "ego_offline/mdf4/mdf4_writer.hpp"
#include "ego_offline/upload/s3_uploader.hpp"
#include "ego_offline/finalize/scene_builder.hpp"
#include "ego_offline/reports/report_builder.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace ego_offline::pipeline {

namespace {

using Clock = std::chrono::steady_clock;

double elapsed_s(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

// P1-1: Combined parse + validation stats accumulation in a single scan pass.
// Collects everything LogValidator needs without a second full file read.
struct ScanAccumulator {
    validate::StreamValidation sv;
    bool first = true;
    ns_t prev_ts = 0;
    ns_t prev_audio_ts = 0;
    ns_t prev_imu_ts   = 0;
    uint32_t nominal_audio_period_ns = 0;
    seq_t    prev_seq  = 0;
    const Config* cfg  = nullptr;

    void feed(const load::RawPacket& raw, bool crc_ok) {
        ++sv.packets_total;
        if (!crc_ok) { ++sv.packets_crc_failed; ++sv.packets_lost; return; }
        ++sv.packets_valid;

        ns_t ts = static_cast<ns_t>(raw.header.ts_ns);

        // Seq gap check
        if (!first) {
            seq_t expected = prev_seq + 1;
            if (raw.header.seq != expected && raw.header.seq > expected) {
                uint32_t lost = raw.header.seq - expected;
                sv.packets_lost += lost;
                validate::GapEvent g;
                g.t_ns        = ts;
                g.stream_id   = "ego";
                g.gap_type    = validate::GapType::seq;
                g.expected_seq = expected;
                g.actual_seq   = raw.header.seq;
                g.gap_duration_ns = ts - prev_ts;
                g.severity    = (lost > 10) ? validate::GapSeverity::error
                                            : validate::GapSeverity::warning;
                sv.gap_events.push_back(g);
            }
        }
        prev_seq = raw.header.seq;

        // Timestamp checks
        if (ts == 0) { ++sv.timestamp_violations; }
        if (!first && ts < prev_ts) {
            ++sv.monotonic_violations;
            validate::GapEvent g;
            g.t_ns = ts; g.stream_id = "ego";
            g.gap_type = validate::GapType::timestamp;
            g.severity = validate::GapSeverity::warning;
            g.gap_duration_ns = prev_ts - ts;
            sv.gap_events.push_back(g);
        }
        if (!first && cfg) {
            ns_t jump = ts - prev_ts;
            if (jump > static_cast<ns_t>(cfg->max_timestamp_jump_ms) * kNsPerMs) {
                ++sv.timestamp_violations;
                validate::GapEvent g;
                g.t_ns = ts; g.stream_id = "ego";
                g.gap_type = validate::GapType::timestamp;
                g.severity = validate::GapSeverity::warning;
                g.gap_duration_ns = jump;
                sv.gap_events.push_back(g);
            }
        }

        if (first || ts < sv.t_first_ns) sv.t_first_ns = ts;
        if (ts > sv.t_last_ns)           sv.t_last_ns  = ts;

        // Audio gap check (type 1 = kAudioBlock)
        if (raw.header.type == 1 && raw.payload.size() >= sizeof(AudioBlockHeader)) {
            AudioBlockHeader ahdr{};
            std::memcpy(&ahdr, raw.payload.data(), sizeof(ahdr));
            ns_t period = static_cast<ns_t>(ahdr.frame_count) * kNsPerSec / ahdr.sample_rate_hz;
            if (nominal_audio_period_ns == 0)
                nominal_audio_period_ns = static_cast<uint32_t>(period);

            if (prev_audio_ts != 0 && nominal_audio_period_ns > 0 && cfg) {
                ns_t gap   = ts - prev_audio_ts;
                ns_t thresh = static_cast<ns_t>(cfg->audio_gap_factor * nominal_audio_period_ns);
                if (gap > thresh) {
                    ++sv.audio_gap_count;
                    validate::GapEvent g;
                    g.t_ns = ts; g.stream_id = "ego.audio";
                    g.gap_type = validate::GapType::audio_block;
                    g.severity = validate::GapSeverity::warning;
                    g.gap_duration_ns = gap;
                    sv.gap_events.push_back(g);
                }
            }
            prev_audio_ts = ts;
        }

        // IMU gap check (type 3 = kImuSample)
        if (raw.header.type == 3 && cfg) {
            if (prev_imu_ts != 0) {
                ns_t gap = ts - prev_imu_ts;
                if (gap > static_cast<ns_t>(cfg->telemetry_gap_ms) * kNsPerMs) {
                    ++sv.telemetry_gap_count;
                    validate::GapEvent g;
                    g.t_ns = ts; g.stream_id = "ego.imu";
                    g.gap_type = validate::GapType::telemetry;
                    g.severity = validate::GapSeverity::warning;
                    g.gap_duration_ns = gap;
                    sv.gap_events.push_back(g);
                }
            }
            prev_imu_ts = ts;
        }

        prev_ts = ts;
        first   = false;
    }
};

} // namespace

Pipeline::Pipeline(const Config& cfg) : cfg_(cfg) {}

std::filesystem::path Pipeline::out_dir(const RunOptions& opts) const {
    if (!opts.out_dir.empty()) return opts.out_dir;
    return opts.session_dir / "offline";
}

ExitCode Pipeline::run(const RunOptions& opts) {
    if (opts.only_validate) return run_validate(opts);
    return run_full(opts);
}

ExitCode Pipeline::run_validate(const RunOptions& opts) {
    // ── Load metadata ─────────────────────────────────────────────────────────
    load::SessionBundle meta;
    try { meta = load::SessionBundle::load(opts.session_dir); }
    catch (const std::exception& e) {
        std::cerr << "[ego-offline] metadata warning: " << e.what() << '\n';
    }

    // ── Load ego reader ───────────────────────────────────────────────────────
    load::EgoManifest manifest;
    try {
        manifest = load::EgoManifest::from_json(opts.session_dir / "ego_manifest.json");
    } catch (const std::exception& e) {
        std::cerr << "[ego-offline] manifest error: " << e.what() << '\n';
        return ExitCode::validation_error;
    }

    load::EgoLogReader ego_reader(manifest);

    // P1-1: single scan — parse + accumulate validation stats simultaneously
    ScanAccumulator ego_acc; ego_acc.cfg = &cfg_;
    ego_acc.sv.file_ok = !manifest.chunks.empty();
    parse::PacketDemuxer demuxer;
    parse::ContractDemuxer contract_demuxer;
    parse::DecodedEgoStreams ego_streams;

    ego_reader.scan([&](const load::RawPacket& raw) {
        bool ok = false;
        if (raw.is_contract) {
            const auto& h = raw.contract_header;
            ok = (h.magic == contract::kFrameMagic) && (h.payload_size == raw.payload.size());
            if (ok && h.payload_size > 0) {
                ok = (crc32(raw.payload.data(), h.payload_size) == h.payload_crc32);
            }
            if (ok) {
                contract::EgoFrameHeader z = h;
                z.header_crc32 = 0;
                ok = (crc32(reinterpret_cast<const uint8_t*>(&z), sizeof(z)) == h.header_crc32);
            }
            if (ok) {
                contract_demuxer.demux(h, raw.payload.data(), h.payload_size, ego_streams);
            }
        } else {
            auto res = parse::BinaryParser::validate(raw);
            ok = res.ok();
            if (ok) {
                demuxer.demux(raw.header.type,
                              static_cast<ns_t>(raw.header.ts_ns),
                              raw.payload.data(), raw.header.payload_size,
                              ego_streams);
            }
        }
        ego_acc.feed(raw, ok);
        return true;
    });

    // P1-2: source.bin optional
    auto src_bin = opts.source_bin.empty()
        ? opts.session_dir / "source.bin"
        : opts.source_bin;
    const bool source_present = std::filesystem::exists(src_bin);

    validate::StreamValidation src_sv;
    if (!source_present) {
        src_sv.file_ok = false;
        src_sv.file_errors.push_back("source.bin not found (optional — skipping)");
    } else {
        ScanAccumulator src_acc; src_acc.cfg = &cfg_;
        src_acc.sv.file_ok = true;
        load::SourceLogReader src_reader(load::SourceManifest::single_file(src_bin));
        parse::SourceDemuxer src_demuxer;
        parse::DecodedSourceStreams src_streams;
        src_reader.scan([&](const load::RawPacket& raw) {
            auto res = parse::BinaryParser::validate(raw);
            src_acc.feed(raw, res.ok());
            if (res.ok())
                src_demuxer.demux(raw.header.type,
                                  static_cast<ns_t>(raw.header.ts_ns),
                                  raw.payload.data(), raw.header.payload_size,
                                  src_streams);
            return true;
        });
        src_sv = src_acc.sv;
    }

    validate::LogValidator validator(cfg_);
    auto val_rpt = validator.build_report(
        meta.session.session_id, ego_acc.sv, src_sv);

    auto out = out_dir(opts);
    val_rpt.write(out / "validation_report.json");

    std::cerr << "[ego-offline] validation: " << [&] {
        switch (val_rpt.integrity) {
            case validate::Integrity::ok:      return "OK";
            case validate::Integrity::warning: return "WARNING";
            default:                           return "FAILED";
        }
    }() << "  packets=" << ego_acc.sv.packets_total
        << "  session_frames=" << ego_streams.session_frame_count << '\n';

    if (val_rpt.integrity == validate::Integrity::failed
        && cfg_.validation.fail_on_no_valid_ranges)
        return ExitCode::validation_error;

    return ExitCode::success;
}

ExitCode Pipeline::run_full(const RunOptions& opts) {
    auto wall_start = Clock::now();
    std::vector<std::string> errors;
    reports::SessionReport::Timing timing;

    // ─────────────────────────────────────────────────────────────────────────
    // STAGE 1: Load metadata
    // ─────────────────────────────────────────────────────────────────────────
    auto t1 = Clock::now();
    load::SessionBundle meta;
    try { meta = load::SessionBundle::load(opts.session_dir); }
    catch (const std::exception& e) {
        errors.push_back(std::string("metadata: ") + e.what());
        std::cerr << "[ego-offline] " << errors.back() << '\n';
    }
    timing.stages_s["load_metadata"] = elapsed_s(t1);

    // ─────────────────────────────────────────────────────────────────────────
    // STAGE 2+4 (ego): Single scan — parse + accumulate validation stats
    // P1-1: eliminates the second full ego scan that validate_ego() used to do
    // ─────────────────────────────────────────────────────────────────────────
    t1 = Clock::now();
    load::EgoManifest manifest;
    try {
        manifest = load::EgoManifest::from_json(opts.session_dir / "ego_manifest.json");
    } catch (const std::exception& e) {
        std::cerr << "[ego-offline] FATAL: " << e.what() << '\n';
        return ExitCode::internal_error;
    }

    load::EgoLogReader ego_reader(manifest);
    parse::PacketDemuxer demuxer;
    parse::ContractDemuxer contract_demuxer;
    parse::DecodedEgoStreams ego_streams;
    ScanAccumulator ego_acc; ego_acc.cfg = &cfg_;
    ego_acc.sv.file_ok = !manifest.chunks.empty();

    ego_reader.scan([&](const load::RawPacket& raw) {
        bool ok = false;
        if (raw.is_contract) {
            const auto& h = raw.contract_header;
            ok = (h.magic == contract::kFrameMagic) && (h.payload_size == raw.payload.size());
            if (ok && h.payload_size > 0) {
                ok = (crc32(raw.payload.data(), h.payload_size) == h.payload_crc32);
            }
            if (ok) {
                contract::EgoFrameHeader z = h;
                z.header_crc32 = 0;
                ok = (crc32(reinterpret_cast<const uint8_t*>(&z), sizeof(z)) == h.header_crc32);
            }
            if (ok) {
                contract_demuxer.demux(h, raw.payload.data(), h.payload_size, ego_streams);
            }
        } else {
            auto res = parse::BinaryParser::validate(raw);
            ok = res.ok();
            if (ok) {
                demuxer.demux(raw.header.type,
                              static_cast<ns_t>(raw.header.ts_ns),
                              raw.payload.data(), raw.header.payload_size,
                              ego_streams);
            }
        }
        ego_acc.feed(raw, ok);
        return true;
    });
    timing.stages_s["parse_and_validate_ego"] = elapsed_s(t1);

    // ─────────────────────────────────────────────────────────────────────────
    // STAGE 3+4 (source): Single scan — parse + accumulate
    // P1-2: source.bin is optional; missing file is NOT a hard failure
    // ─────────────────────────────────────────────────────────────────────────
    t1 = Clock::now();
    auto src_bin = opts.source_bin.empty()
        ? opts.session_dir / "source.bin"
        : opts.source_bin;
    const bool source_present = std::filesystem::exists(src_bin);

    parse::DecodedSourceStreams src_streams;
    validate::StreamValidation src_sv;

    if (!source_present) {
        src_sv.file_ok = false;
        src_sv.file_errors.push_back("source.bin not found (optional)");
        std::cerr << "[ego-offline] source.bin absent -- source/sync stages will be skipped\n";
    } else {
        ScanAccumulator src_acc; src_acc.cfg = &cfg_;
        src_acc.sv.file_ok = true;
        load::SourceLogReader src_reader(load::SourceManifest::single_file(src_bin));
        parse::SourceDemuxer src_demuxer;

        src_reader.scan([&](const load::RawPacket& raw) {
            auto res = parse::BinaryParser::validate(raw);
            src_acc.feed(raw, res.ok());
            if (res.ok())
                src_demuxer.demux(raw.header.type,
                                  static_cast<ns_t>(raw.header.ts_ns),
                                  raw.payload.data(), raw.header.payload_size,
                                  src_streams);
            return true;
        });
        src_sv = src_acc.sv;
    }
    timing.stages_s["parse_and_validate_source"] = elapsed_s(t1);

    // ─────────────────────────────────────────────────────────────────────────
    // STAGE 4: Build validation report from already-accumulated stats
    // ─────────────────────────────────────────────────────────────────────────
    t1 = Clock::now();
    validate::LogValidator validator(cfg_);
    auto val_rpt = validator.build_report(
        meta.session.session_id, ego_acc.sv, src_sv);

    auto out = out_dir(opts);
    val_rpt.write(out / "validation_report.json");
    timing.stages_s["validation_report"] = elapsed_s(t1);

    std::cerr << "[ego-offline] ego packets=" << ego_acc.sv.packets_total
              << "  session_frames=" << ego_streams.session_frame_count
              << "  gps=" << ego_streams.gps.size()
              << "  imu=" << ego_streams.imu.size() << '\n';

    if (val_rpt.integrity == validate::Integrity::failed
        && cfg_.validation.fail_on_no_valid_ranges) {
        errors.push_back("validation failed: no valid ranges >= "
            + std::to_string(cfg_.validation.min_valid_duration_s) + "s");
        std::cerr << "[ego-offline] " << errors.back() << '\n';
        return ExitCode::validation_error;
    }
    if (opts.only_validate) {
        timing.total_wall_s = elapsed_s(wall_start);
        return ExitCode::success;
    }

    t1 = Clock::now();
    const auto ego_nav_sidecar_path = opts.session_dir / cfg_.fallback.ego_nav_sidecar_filename;
    std::string ego_gps_source = ego_streams.gps.empty() ? "missing" : "upstream_sc589";
    bool ego_gps_fallback_used = false;

    if (ego_streams.gps.empty() && cfg_.fallback.ego_nav_sidecar_enabled) {
        try {
            const auto sidecar = load::ReadEgoNavSidecar(ego_nav_sidecar_path);
            if (!sidecar.points.empty()) {
                ego_streams.gps = sidecar.points;
                ego_gps_source = "local_m2_fallback";
                ego_gps_fallback_used = true;
            }
            if (sidecar.invalid_lines > 0U) {
                std::cerr << "[ego-offline] ego_nav sidecar ignored "
                          << sidecar.invalid_lines << " invalid lines\n";
            }
        } catch (const std::exception& e) {
            errors.push_back(std::string("ego_nav_sidecar: ") + e.what());
            std::cerr << "[ego-offline] " << errors.back() << '\n';
        }
    }
    const std::uint64_t ego_gps_points = static_cast<std::uint64_t>(ego_streams.gps.size());
    timing.stages_s["ego_gps_select"] = elapsed_s(t1);
    std::cerr << "[ego-offline] ego_gps_source=" << ego_gps_source
              << " points=" << ego_gps_points << '\n';

    // ─────────────────────────────────────────────────────────────────────────
    // STAGE 5: Sync
    // ─────────────────────────────────────────────────────────────────────────
    t1 = Clock::now();
    TimeRange valid_range;
    if (!val_rpt.valid_ranges.empty()) valid_range = val_rpt.valid_ranges.front();

    std::vector<ns_t> audio_onsets;
    for (const auto& ab : ego_streams.audio) audio_onsets.push_back(ab.ts_ns);

    sync::SyncReport sync_rpt;
    sync_rpt.offline_pipeline_version = cfg_.offline_pipeline_version;
    sync_rpt.session_id               = meta.session.session_id;

    if (source_present) {
        sync::LogSynchroniser synchroniser(cfg_);
        sync_rpt = synchroniser.synchronise(
            ego_streams.gps, src_streams.gnss,
            src_streams.events, audio_onsets,
            valid_range, meta.session.session_id);
        sync_rpt.write(out / "sync_report.json");
    } else {
        sync_rpt.status = sync::SyncStatus::sync_failed;
        sync_rpt.notes.push_back("source.bin absent -- sync skipped");
        sync_rpt.write(out / "sync_report.json");
    }
    timing.stages_s["sync"] = elapsed_s(t1);

    // P1-3: exit code 2 on hard sync failure when configured
    if (sync_rpt.status == sync::SyncStatus::sync_failed) {
        const std::string msg = "sync failed: confidence=" + std::to_string(sync_rpt.confidence);
        std::cerr << "[ego-offline] WARNING: " << msg << '\n';
        if (cfg_.sync.fail_on_sync_error) {
            errors.push_back(msg);
            timing.total_wall_s = elapsed_s(wall_start);
            return ExitCode::sync_error;
        }
    }

    // Apply clock offset to source data
    if (source_present && sync_rpt.offset_ns != 0) {
        sync::LogSynchroniser::apply_offset(src_streams.events, sync_rpt.offset_ns);
        sync::LogSynchroniser::apply_offset(src_streams.gnss,   sync_rpt.offset_ns);
    }

    if (opts.only_sync) {
        timing.total_wall_s = elapsed_s(wall_start);
        return ExitCode::success;
    }

    // ── STAGE 6: Align ────────────────────────────────────────────────────────
    t1 = Clock::now();
    align::TimeGrid grid = align::TimeGrid::build(
        valid_range.t_start_ns, valid_range.t_end_ns, cfg_.grid_dt_ms);

    align::TimeAligner aligner(cfg_);
    auto aligned = aligner.align(grid, ego_streams.gps, src_streams.gnss,
                                 ego_streams.imu, ego_streams.trajectory);
    timing.stages_s["align"] = elapsed_s(t1);

    // ── STAGE 7: Scene geometry ───────────────────────────────────────────────
    t1 = Clock::now();
    scene::SceneCalculator sc_calc;
    auto scene_geom   = sc_calc.compute(aligned);
    auto step_metrics = sc_calc.step_metrics(scene_geom, cfg_.grid_dt_ms / 1000.0);
    timing.stages_s["scene_geometry"] = elapsed_s(t1);

    // ── STAGE 8: MDF4 (stub until ADR-001) ───────────────────────────────────
    t1 = Clock::now();
    std::string mdf4_path_str;
    if (!opts.only_scenes) {
        mdf4::Mdf4Writer mdf_writer(cfg_);
        mdf_writer.prepare(aligned, scene_geom, step_metrics, meta, valid_range.t_start_ns);
        auto mdf4_path = out / cfg_.mdf4.output_filename;
        if (mdf_writer.write(mdf4_path)) {
            mdf4_path_str = mdf4_path.string();
            mdf_writer.validate(mdf4_path);
            std::cerr << "[ego-offline] MDF4: " << mdf4_path_str << '\n';
        } else {
            errors.push_back("MDF4 write failed");
            std::cerr << "[ego-offline] MDF4: write failed\n";
        }
    }
    timing.stages_s["mdf4"] = elapsed_s(t1);

    // ── STAGE 9: Scene events + segments ─────────────────────────────────────
    t1 = Clock::now();
    finalize::SceneBuilder scene_builder(cfg_);
    auto scene_events = scene_builder.extract_events(src_streams.events, scene_geom);
    auto scene_segs   = scene_builder.build_segments(scene_events, scene_geom);
    scene_builder.link_segments_to_events(scene_segs, scene_events);
    scene_builder.write_scene_events(scene_events, meta.session.session_id,
                                     out / "scene_events.json");
    scene_builder.write_scene_segments(scene_segs, meta.session.session_id,
                                       out / "scene_segments.json");
    timing.stages_s["finalize"] = elapsed_s(t1);

    // ── STAGE 10: Reports ─────────────────────────────────────────────────────
    t1 = Clock::now();
    timing.total_wall_s = elapsed_s(wall_start);

    reports::ReportBuilder rpt_builder(cfg_);

    reports::SessionReport::InputPaths inputs;
    inputs.session_dir  = opts.session_dir.string();
    inputs.ego_manifest = (opts.session_dir / "ego_manifest.json").string();
    inputs.ego_nav_sidecar = std::filesystem::exists(ego_nav_sidecar_path) ? ego_nav_sidecar_path.string() : "";
    inputs.source_bin   = src_bin.string();
    inputs.config       = opts.config_path.string();

    reports::SessionReport::Outputs outputs_info;
    outputs_info.out_dir           = out.string();
    outputs_info.session_mf4       = mdf4_path_str;
    outputs_info.validation_report = (out / "validation_report.json").string();
    outputs_info.sync_report       = (out / "sync_report.json").string();
    outputs_info.scene_events      = (out / "scene_events.json").string();
    outputs_info.scene_segments    = (out / "scene_segments.json").string();

    timing.stages_s["reports"] = elapsed_s(t1);

    auto sess_rpt = rpt_builder.build_session_report(
        val_rpt, sync_rpt, meta, meta.session.session_id,
        inputs, outputs_info, timing,
        ego_gps_source, ego_gps_points, ego_gps_fallback_used, errors);
    sess_rpt.write(out / "session_report.json");

    auto dq_rpt = rpt_builder.build_data_quality_report(
        val_rpt, meta.session.session_id,
        ego_gps_source, ego_gps_points, ego_gps_fallback_used);
    dq_rpt.write(out / "data_quality_report.json");

    auto ev_rpt = rpt_builder.build_events_report(
        scene_events, scene_segs, meta.session.session_id);
    ev_rpt.write(out / "events_report.json");

    std::cerr << "[ego-offline] done in " << timing.total_wall_s << "s  status: "
              << sess_rpt.pipeline_status << '\n';

    if (!opts.skip_s3 && !opts.only_validate && !opts.only_sync
        && !opts.only_mdf4 && !opts.only_scenes && cfg_.s3.enabled) {
        upload::S3Uploader uploader(cfg_);
        const ExitCode up = uploader.upload_session(opts.session_dir, out, opts.upload_dry_run);
        if (up != ExitCode::success && up != ExitCode::upload_blocked)
            return up;
        if (up == ExitCode::upload_blocked && !opts.upload_dry_run)
            return ExitCode::upload_blocked;
    }

    if (errors.empty())
        return ExitCode::success;
    return ExitCode::internal_error;
}

} // namespace ego_offline::pipeline
