#include "ego_offline/parse/contract_demuxer.hpp"
#include "ego_offline/checksum.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace ego_offline::parse {

namespace {

#pragma pack(push, 1)
struct AudioBlockBinaryHeader {
    uint64_t audio_block_id;
    uint64_t t0_ns;
    uint64_t t1_ns;
    uint32_t sample_rate_hz;
    uint16_t channels_count;
    uint16_t bytes_per_sample;
    uint32_t frames_count;
    uint32_t layout;
    uint32_t data_size;
    uint32_t reserved0;
};

struct ImuWindowPacket {
    uint64_t window_id;
    uint64_t t0_ns;
    uint64_t t1_ns;
    uint16_t sample_count;
    uint16_t flags;
    float accel_mean_x_mps2;
    float accel_mean_y_mps2;
    float accel_mean_z_mps2;
    float gyro_mean_x_rad_s;
    float gyro_mean_y_rad_s;
    float gyro_mean_z_rad_s;
    float delta_velocity_x_mps;
    float delta_velocity_y_mps;
    float delta_velocity_z_mps;
    float delta_angle_x_rad;
    float delta_angle_y_rad;
    float delta_angle_z_rad;
};

struct TrajectoryPointPacket {
    uint64_t t_ns;
    float x_m, y_m, z_m;
    float yaw_rad, pitch_rad, roll_rad;
    float yaw_rate_rad_s;
    float path_s_m;
    float vehicle_speed_mps;
    uint32_t flags;
    uint32_t reserved0;
};

struct GpsFixPacket {
    uint64_t t_ns;
    double lat_deg, lon_deg, alt_m;
    float speed_mps, heading_rad, h_acc_m, v_acc_m;
    uint8_t fix_type, rtk_status, satellites, reserved0;
    uint32_t flags;
};

struct CanRawFramePacket {
    uint64_t t_ns;
    uint32_t can_id;
    uint8_t dlc;
    uint8_t is_extended;
    uint8_t bus_id;
    uint8_t flags;
    uint8_t data[8];
};

struct TimeStatusPacket {
    uint64_t t_ns;
    uint64_t monotonic_ns;
    int64_t utc_offset_ns;
    uint32_t time_source;
    uint32_t sync_status;
    float estimated_drift_ppm;
    float sync_error_us;
};

struct SystemStatusPacket {
    uint64_t t_ns;
    uint32_t audio_status;
    uint32_t can_status;
    uint32_t imu_status;
    uint32_t gps_status;
    uint32_t network_status;
    uint32_t audio_overruns;
    uint32_t imu_fifo_overruns;
    uint32_t can_rx_errors;
    uint32_t dropped_frames;
    float cpu_load_arm;
    float cpu_load_sharc0;
    float cpu_load_sharc1;
};

struct SessionEventBinaryHeader {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t event_type;
    uint32_t flags;
    uint64_t session_id_hi;
    uint64_t session_id_lo;
    uint64_t t_ns;
    uint32_t metadata_text_size;
    uint32_t metadata_text_crc32;
    uint32_t config_generation;
    uint32_t reserved0;
};

struct ConfigSnapshotBinaryHeader {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t payload_format;
    uint32_t flags;
    uint64_t generated_t_ns;
    uint32_t text_size;
    uint32_t text_crc32;
    uint32_t config_generation;
    uint32_t active_mask;
    uint32_t required_mask;
    uint32_t invalid_mask;
    uint32_t reserved0;
};
#pragma pack(pop)

static constexpr uint32_t kSessionEventMagic = 0x31534553u;
static constexpr uint32_t kConfigSnapshotMagic = 0x31474643u;

std::string SessionIdFromParts(uint64_t hi, uint64_t lo) {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(hi),
                  static_cast<unsigned long long>(lo));
    return std::string(buf);
}

void ParseSessionEventBinary(const contract::EgoFrameHeader& header,
                             const uint8_t* payload,
                             uint32_t size,
                             DecodedEgoStreams& out) {
    if (size < sizeof(SessionEventBinaryHeader)) {
        return;
    }
    SessionEventBinaryHeader eh{};
    std::memcpy(&eh, payload, sizeof(eh));
    if (eh.magic != kSessionEventMagic) {
        return;
    }
    if (size < sizeof(eh) + eh.metadata_text_size) {
        return;
    }
    DecodedEgoStreams::SessionEvent ev;
    ev.frame_type = header.frame_type;
    ev.ts_ns = eh.t_ns;
    ev.session_id = SessionIdFromParts(eh.session_id_hi, eh.session_id_lo);
    if (eh.metadata_text_size > 0U) {
        ev.metadata_text.assign(reinterpret_cast<const char*>(payload + sizeof(eh)),
                                eh.metadata_text_size);
    }
    out.session_events.push_back(std::move(ev));
}

void ParseConfigSnapshotBinary(const uint8_t* payload, uint32_t size, DecodedEgoStreams& out) {
    if (size < sizeof(ConfigSnapshotBinaryHeader)) {
        return;
    }
    ConfigSnapshotBinaryHeader ch{};
    std::memcpy(&ch, payload, sizeof(ch));
    if (ch.magic != kConfigSnapshotMagic) {
        return;
    }
    if (size < sizeof(ch) + ch.text_size) {
        return;
    }
    if (ch.text_size > 0U) {
        out.config_snapshot_text.assign(reinterpret_cast<const char*>(payload + sizeof(ch)),
                                        ch.text_size);
    }
}

}  // namespace

bool ContractDemuxer::demux(const contract::EgoFrameHeader& header,
                            const uint8_t* payload,
                            uint32_t size,
                            DecodedEgoStreams& out) const {
    const auto t = static_cast<contract::FrameType>(header.frame_type);
    switch (t) {
        case contract::FrameType::kSessionStarted:
        case contract::FrameType::kSessionEnded:
            ParseSessionEventBinary(header, payload, size, out);
            return true;
        case contract::FrameType::kConfigSnapshot:
            ParseConfigSnapshotBinary(payload, size, out);
            return true;
        case contract::FrameType::kCanRaw: {
            if (size < sizeof(CanRawFramePacket)) {
                return true;
            }
            CanRawFramePacket raw{};
            std::memcpy(&raw, payload, sizeof(raw));
            DecodedEgoStreams::CanRawFrame frame;
            frame.ts_ns = raw.t_ns;
            frame.can_id = raw.can_id;
            frame.dlc = raw.dlc;
            frame.bus_id = raw.bus_id;
            std::memcpy(frame.data, raw.data, sizeof(frame.data));
            out.can_raw.push_back(frame);
            return true;
        }
        case contract::FrameType::kTimeStatus: {
            if (size < sizeof(TimeStatusPacket)) {
                return true;
            }
            TimeStatusPacket raw{};
            std::memcpy(&raw, payload, sizeof(raw));
            DecodedEgoStreams::TimeStatusSample sample;
            sample.ts_ns = raw.t_ns;
            sample.time_source = raw.time_source;
            sample.sync_status = raw.sync_status;
            sample.estimated_drift_ppm = raw.estimated_drift_ppm;
            out.time_status.push_back(sample);
            return true;
        }
        case contract::FrameType::kSystemStatus: {
            if (size < sizeof(SystemStatusPacket)) {
                return true;
            }
            SystemStatusPacket raw{};
            std::memcpy(&raw, payload, sizeof(raw));
            DecodedEgoStreams::SystemStatusSample sample;
            sample.ts_ns = raw.t_ns;
            sample.audio_status = raw.audio_status;
            sample.can_status = raw.can_status;
            sample.imu_status = raw.imu_status;
            sample.gps_status = raw.gps_status;
            sample.dropped_frames = raw.dropped_frames;
            out.system_status.push_back(sample);
            return true;
        }
        case contract::FrameType::kAudioBlock: {
            if (size < sizeof(AudioBlockBinaryHeader)) return true;
            AudioBlockBinaryHeader ah{};
            std::memcpy(&ah, payload, sizeof(ah));
            const uint32_t pcm_bytes = ah.data_size;
            if (size < sizeof(ah) + pcm_bytes) return true;
            DecodedEgoStreams::AudioBlock blk;
            blk.ts_ns = ah.t0_ns;
            blk.channels = static_cast<uint8_t>(ah.channels_count);
            blk.sample_rate_hz = ah.sample_rate_hz;
            blk.frame_count = ah.frames_count;
            const uint32_t n = ah.channels_count * ah.frames_count;
            blk.samples.resize(n);
            std::memcpy(blk.samples.data(), payload + sizeof(ah), n * sizeof(int16_t));
            out.audio.push_back(std::move(blk));
            return true;
        }
        case contract::FrameType::kImuWindow: {
            if (size < sizeof(ImuWindowPacket)) return true;
            ImuWindowPacket raw{};
            std::memcpy(&raw, payload, sizeof(raw));
            ImuSample s;
            s.ts_ns = raw.t0_ns;
            s.acc_x_mps2 = raw.accel_mean_x_mps2;
            s.acc_y_mps2 = raw.accel_mean_y_mps2;
            s.acc_z_mps2 = raw.accel_mean_z_mps2;
            s.gyro_x_rads = raw.gyro_mean_x_rad_s;
            s.gyro_y_rads = raw.gyro_mean_y_rad_s;
            s.gyro_z_rads = raw.gyro_mean_z_rad_s;
            out.imu.push_back(s);
            return true;
        }
        case contract::FrameType::kTrajectory: {
            if (size < sizeof(TrajectoryPointPacket)) return true;
            TrajectoryPointPacket raw{};
            std::memcpy(&raw, payload, sizeof(raw));
            TrajectoryPoint tp;
            tp.ts_ns = raw.t_ns;
            tp.yaw_deg = raw.yaw_rad * 180.f / 3.14159265f;
            tp.vel_n_mps = raw.vehicle_speed_mps * std::cos(raw.yaw_rad);
            tp.vel_e_mps = raw.vehicle_speed_mps * std::sin(raw.yaw_rad);
            out.trajectory.push_back(tp);
            DecodedEgoStreams::MotionSample m;
            m.ts_ns = raw.t_ns;
            m.speed_mps = raw.vehicle_speed_mps;
            m.yaw_rate_rads = raw.yaw_rate_rad_s;
            out.motion.push_back(m);
            return true;
        }
        case contract::FrameType::kGpsFix: {
            if (size < sizeof(GpsFixPacket)) return true;
            GpsFixPacket raw{};
            std::memcpy(&raw, payload, sizeof(raw));
            GnssPoint g;
            g.ts_ns = raw.t_ns;
            g.latitude_deg = raw.lat_deg;
            g.longitude_deg = raw.lon_deg;
            g.altitude_m = raw.alt_m;
            g.speed_mps = raw.speed_mps;
            g.heading_deg = raw.heading_rad * 180.f / 3.14159265f;
            out.gps.push_back(g);
            return true;
        }
        default:
            return true;
    }
}

}  // namespace ego_offline::parse
