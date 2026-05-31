#include "ego_offline/parse/packet_demuxer.hpp"
#include <cstring>

namespace ego_offline::parse {

bool PacketDemuxer::demux(uint16_t type, ns_t ts_ns,
                          const uint8_t* payload, uint32_t size,
                          DecodedEgoStreams& out) const {
    switch (static_cast<EgoPacketType>(type)) {
        case EgoPacketType::kAudioBlock:
            demux_audio(ts_ns, payload, size, out);         return true;
        case EgoPacketType::kCanFrame:
            demux_can(ts_ns, payload, size, out);           return true;
        case EgoPacketType::kImuSample:
            demux_imu(ts_ns, payload, size, out);           return true;
        case EgoPacketType::kMotionState:
            demux_motion(ts_ns, payload, size, out);        return true;
        case EgoPacketType::kGpsFix:
            demux_gps(ts_ns, payload, size, out);           return true;
        case EgoPacketType::kSessionFrame:
            demux_session_frame(ts_ns, payload, size, out); return true;
        case EgoPacketType::kDiagnostics:
            demux_diagnostics(ts_ns, payload, size, out);   return true;
        case EgoPacketType::kSessionMeta:
            demux_session_meta(ts_ns, payload, size, out);  return true;
        default:
            return false;
    }
}

DecodedEgoStreams PacketDemuxer::demux_all(
    const std::vector<std::pair<PacketHeader, std::vector<uint8_t>>>& packets) const
{
    DecodedEgoStreams out;
    for (const auto& [hdr, payload] : packets)
        demux(hdr.type, static_cast<ns_t>(hdr.ts_ns),
              payload.data(), hdr.payload_size, out);
    return out;
}

void PacketDemuxer::demux_audio(ns_t ts, const uint8_t* p, uint32_t sz,
                                 DecodedEgoStreams& out) const {
    if (sz < sizeof(AudioBlockHeader)) return;
    AudioBlockHeader hdr{};
    std::memcpy(&hdr, p, sizeof(hdr));
    uint32_t n_samples = static_cast<uint32_t>(hdr.channels) * hdr.frame_count;
    if (sz < sizeof(AudioBlockHeader) + n_samples * 2u) return;
    DecodedEgoStreams::AudioBlock blk;
    blk.ts_ns = ts; blk.channels = hdr.channels;
    blk.sample_rate_hz = hdr.sample_rate_hz; blk.frame_count = hdr.frame_count;
    blk.samples.resize(n_samples);
    std::memcpy(blk.samples.data(), p + sizeof(AudioBlockHeader), n_samples * 2u);
    out.audio.push_back(std::move(blk));
}

void PacketDemuxer::demux_can(ns_t, const uint8_t*, uint32_t,
                               DecodedEgoStreams&) const {}

void PacketDemuxer::demux_imu(ns_t ts, const uint8_t* p, uint32_t sz,
                               DecodedEgoStreams& out) const {
    if (sz < sizeof(ImuPayload)) return;
    ImuPayload raw{};
    std::memcpy(&raw, p, sizeof(raw));
    ImuSample s;
    s.ts_ns = ts;
    s.acc_x_mps2 = raw.acc_x_mps2; s.acc_y_mps2 = raw.acc_y_mps2;
    s.acc_z_mps2 = raw.acc_z_mps2; s.gyro_x_rads = raw.gyro_x_rads;
    s.gyro_y_rads = raw.gyro_y_rads; s.gyro_z_rads = raw.gyro_z_rads;
    out.imu.push_back(s);
}

void PacketDemuxer::demux_motion(ns_t ts, const uint8_t* p, uint32_t sz,
                                  DecodedEgoStreams& out) const {
    if (sz < sizeof(MotionStatePayload)) return;
    MotionStatePayload raw{};
    std::memcpy(&raw, p, sizeof(raw));
    DecodedEgoStreams::MotionSample m;
    m.ts_ns = ts; m.speed_mps = raw.speed_mps;
    m.steering_angle_deg = raw.steering_angle_deg;
    m.yaw_rate_rads = raw.yaw_rate_rads;
    m.acceleration_mps2 = raw.acceleration_mps2;
    out.motion.push_back(m);
}

void PacketDemuxer::demux_gps(ns_t ts, const uint8_t* p, uint32_t sz,
                               DecodedEgoStreams& out) const {
    if (sz < sizeof(GpsFixPayload)) return;
    GpsFixPayload raw{};
    std::memcpy(&raw, p, sizeof(raw));
    GnssPoint g;
    g.ts_ns = ts; g.latitude_deg = raw.latitude_deg;
    g.longitude_deg = raw.longitude_deg; g.altitude_m = raw.altitude_m;
    g.speed_mps = raw.speed_mps; g.heading_deg = raw.heading_deg;
    g.fix_quality = raw.fix_quality; g.satellites = raw.satellites;
    g.hdop = raw.hdop;
    out.gps.push_back(g);
}

void PacketDemuxer::demux_session_frame(ns_t ts, const uint8_t* p, uint32_t sz,
                                         DecodedEgoStreams& out) const {
    ++out.session_frame_count;
    if (sz < sizeof(SessionFramePayload)) return;
    SessionFramePayload raw{};
    std::memcpy(&raw, p, sizeof(raw));

    // Extract GPS sub-record
    if (raw.gps_fix_quality > 0
        && (raw.gps_lat_deg != 0.0 || raw.gps_lon_deg != 0.0)) {
        GnssPoint g;
        g.ts_ns = ts; g.latitude_deg = raw.gps_lat_deg;
        g.longitude_deg = raw.gps_lon_deg; g.altitude_m = raw.gps_alt_m;
        g.speed_mps = raw.gps_speed_mps; g.heading_deg = raw.gps_heading_deg;
        g.fix_quality = raw.gps_fix_quality; g.satellites = raw.gps_satellites;
        g.hdop = raw.gps_hdop;
        out.gps.push_back(g);
    }

    // Extract IMU sub-record
    if (raw.imu_acc_x_mps2 != 0.f || raw.imu_acc_y_mps2 != 0.f
        || raw.imu_acc_z_mps2 != 0.f) {
        ImuSample s;
        s.ts_ns = ts;
        s.acc_x_mps2 = raw.imu_acc_x_mps2; s.acc_y_mps2 = raw.imu_acc_y_mps2;
        s.acc_z_mps2 = raw.imu_acc_z_mps2; s.gyro_x_rads = raw.imu_gyro_x_rads;
        s.gyro_y_rads = raw.imu_gyro_y_rads; s.gyro_z_rads = raw.imu_gyro_z_rads;
        out.imu.push_back(s);
    }
}

void PacketDemuxer::demux_diagnostics(ns_t, const uint8_t*, uint32_t,
                                       DecodedEgoStreams&) const {}

void PacketDemuxer::demux_session_meta(ns_t, const uint8_t*, uint32_t,
                                        DecodedEgoStreams&) const {}

} // namespace ego_offline::parse
