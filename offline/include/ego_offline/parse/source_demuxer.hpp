#pragma once
// PR-03: source_demuxer — source PacketType 101-105 → структуры

#include "ego_offline/protocol.hpp"
#include "ego_offline/types.hpp"

#include <vector>
#include <string>

namespace ego_offline::parse {

struct DecodedSourceStreams {
    std::vector<SourceEvent>  events;
    std::vector<GnssPoint>    gnss;

    struct RtkStatus {
        ns_t    ts_ns            = 0;
        uint8_t fix_type         = 0;
        uint8_t correction_age_s = 0;
        uint32_t baseline_mm     = 0;
        uint16_t solution_status = 0;
    };
    std::vector<RtkStatus> rtk;

    struct ScenarioMeta {
        ns_t        ts_ns          = 0;
        std::string scenario_id;
        std::string source_id;
        std::string config_version;
    };
    std::vector<ScenarioMeta> scenario_metas;

    struct Diagnostics {
        ns_t        ts_ns        = 0;
        uint8_t     health_state = 0;
        uint8_t     siren_state  = 0;
        uint8_t     battery_pct  = 255;
        uint8_t     disk_free_pct = 0;
        float       temperature_c = 0.f;
        std::string message;
    };
    std::vector<Diagnostics> diagnostics;

    std::vector<std::string> warnings; // unknown types
};

class SourceDemuxer {
public:
    // Demux one source packet.
    // Returns false + appends warning for unknown types (PR-03).
    bool demux(uint16_t type, ns_t ts_ns,
               const uint8_t* payload, uint32_t size,
               DecodedSourceStreams& out) const;

private:
    void demux_event(ns_t ts_ns, const uint8_t* p, uint32_t sz, DecodedSourceStreams& out) const;
    void demux_gnss(ns_t ts_ns, const uint8_t* p, uint32_t sz, DecodedSourceStreams& out) const;
    void demux_rtk(ns_t ts_ns, const uint8_t* p, uint32_t sz, DecodedSourceStreams& out) const;
    void demux_scenario_meta(ns_t ts_ns, const uint8_t* p, uint32_t sz, DecodedSourceStreams& out) const;
    void demux_diagnostics(ns_t ts_ns, const uint8_t* p, uint32_t sz, DecodedSourceStreams& out) const;
};

} // namespace ego_offline::parse
