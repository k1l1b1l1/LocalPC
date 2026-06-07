#include "ego_offline/parse/contract_demuxer.hpp"
#include "ego_offline/checksum.hpp"

#include <cmath>
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
#pragma pack(pop)

}  // namespace

bool ContractDemuxer::demux(const contract::EgoFrameHeader& header,
                            const uint8_t* payload,
                            uint32_t size,
                            DecodedEgoStreams& out) const {
    const auto t = static_cast<contract::FrameType>(header.frame_type);
    switch (t) {
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
