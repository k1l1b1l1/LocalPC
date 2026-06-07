#pragma once
// PR-02: packet_demuxer -- ego PacketType 1-8 -> structures
// Types MUST match prod/common/types.hpp (P0-1 fix)

#include "ego_offline/protocol.hpp"
#include "ego_offline/types.hpp"

#include <string>
#include <vector>
#include <cstring>

namespace ego_offline::parse {

struct DecodedEgoStreams {
    std::vector<GnssPoint>       gps;
    std::vector<ImuSample>       imu;
    std::vector<TrajectoryPoint> trajectory;

    struct AudioBlock {
        ns_t                 ts_ns          = 0;
        uint8_t              channels       = 0;
        uint32_t             sample_rate_hz = 0;
        uint32_t             frame_count    = 0;
        std::vector<int16_t> samples;
    };
    std::vector<AudioBlock> audio;

    struct RtkSample {
        ns_t     ts_ns            = 0;
        uint8_t  fix_type         = 0;
        uint8_t  correction_age_s = 0;
        uint32_t baseline_mm      = 0;
        uint16_t solution_status  = 0;
    };
    std::vector<RtkSample> rtk;

    struct MotionSample {
        ns_t  ts_ns              = 0;
        float speed_mps          = 0.f;
        float steering_angle_deg = 0.f;
        float yaw_rate_rads      = 0.f;
        float acceleration_mps2  = 0.f;
    };
    std::vector<MotionSample> motion;

    struct SessionEvent {
        uint32_t frame_type = 0;
        ns_t     ts_ns = 0;
        std::string session_id;
        std::string metadata_text;
    };
    std::vector<SessionEvent> session_events;
    std::string config_snapshot_text;

    struct CanRawFrame {
        ns_t     ts_ns = 0;
        uint32_t can_id = 0;
        uint8_t  dlc = 0;
        uint8_t  bus_id = 0;
        uint8_t  data[8]{};
    };
    std::vector<CanRawFrame> can_raw;

    struct TimeStatusSample {
        ns_t  ts_ns = 0;
        uint32_t time_source = 0;
        uint32_t sync_status = 0;
        float estimated_drift_ppm = 0.f;
    };
    std::vector<TimeStatusSample> time_status;

    struct SystemStatusSample {
        ns_t  ts_ns = 0;
        uint32_t audio_status = 0;
        uint32_t can_status = 0;
        uint32_t imu_status = 0;
        uint32_t gps_status = 0;
        uint32_t dropped_frames = 0;
    };
    std::vector<SystemStatusSample> system_status;

    uint64_t session_frame_count = 0;
};

class PacketDemuxer {
public:
    // Returns false only for truly unknown types.
    bool demux(uint16_t type, ns_t ts_ns,
               const uint8_t* payload, uint32_t size,
               DecodedEgoStreams& out) const;

    DecodedEgoStreams demux_all(
        const std::vector<std::pair<PacketHeader, std::vector<uint8_t>>>& packets) const;

private:
    void demux_audio(ns_t ts, const uint8_t* p, uint32_t sz, DecodedEgoStreams& out) const;
    void demux_can(ns_t ts, const uint8_t* p, uint32_t sz, DecodedEgoStreams& out) const;
    void demux_imu(ns_t ts, const uint8_t* p, uint32_t sz, DecodedEgoStreams& out) const;
    void demux_motion(ns_t ts, const uint8_t* p, uint32_t sz, DecodedEgoStreams& out) const;
    void demux_gps(ns_t ts, const uint8_t* p, uint32_t sz, DecodedEgoStreams& out) const;
    void demux_session_frame(ns_t ts, const uint8_t* p, uint32_t sz, DecodedEgoStreams& out) const;
    void demux_diagnostics(ns_t ts, const uint8_t* p, uint32_t sz, DecodedEgoStreams& out) const;
    void demux_session_meta(ns_t ts, const uint8_t* p, uint32_t sz, DecodedEgoStreams& out) const;
};

} // namespace ego_offline::parse
