#include "ego_offline/parse/source_demuxer.hpp"
#include <cstring>
#include <string>

namespace ego_offline::parse {

bool SourceDemuxer::demux(uint16_t type, ns_t ts_ns,
                          const uint8_t* payload, uint32_t size,
                          DecodedSourceStreams& out) const {
    switch (static_cast<SourcePacketType>(type)) {
        case SourcePacketType::kSourceEvent:
            demux_event(ts_ns, payload, size, out); return true;
        case SourcePacketType::kSourceGnssFix:
            demux_gnss(ts_ns, payload, size, out);  return true;
        case SourcePacketType::kSourceRtkStatus:
            demux_rtk(ts_ns, payload, size, out);   return true;
        case SourcePacketType::kSourceScenarioMeta:
            demux_scenario_meta(ts_ns, payload, size, out); return true;
        case SourcePacketType::kSourceDiagnostics:
            demux_diagnostics(ts_ns, payload, size, out);   return true;
        default:
            out.warnings.push_back("Unknown source packet type: " + std::to_string(type));
            return false;
    }
}

void SourceDemuxer::demux_event(ns_t ts_ns, const uint8_t* p, uint32_t sz,
                                 DecodedSourceStreams& out) const {
    if (sz < sizeof(SourceEventPayload)) return;
    SourceEventPayload raw{};
    std::memcpy(&raw, p, sizeof(SourceEventPayload));
    SourceEvent ev;
    ev.ts_ns      = ts_ns;
    ev.event_id   = raw.event_id;
    ev.event_type = static_cast<SourceEventType>(raw.event_type);
    ev.flags      = raw.flags;
    ev.duration_ns = static_cast<ns_t>(raw.duration_ns);
    out.events.push_back(ev);
}

void SourceDemuxer::demux_gnss(ns_t ts_ns, const uint8_t* p, uint32_t sz,
                                DecodedSourceStreams& out) const {
    if (sz < sizeof(SourceGnssFixPayload)) return;
    SourceGnssFixPayload raw{};
    std::memcpy(&raw, p, sizeof(SourceGnssFixPayload));
    GnssPoint g;
    g.ts_ns         = ts_ns;
    g.latitude_deg  = raw.latitude_deg;
    g.longitude_deg = raw.longitude_deg;
    g.altitude_m    = raw.altitude_m;
    g.speed_mps     = raw.speed_mps;
    g.heading_deg   = raw.heading_deg;
    g.fix_quality   = raw.fix_quality;
    g.satellites    = raw.satellites;
    g.hdop          = raw.hdop;
    out.gnss.push_back(g);
}

void SourceDemuxer::demux_rtk(ns_t ts_ns, const uint8_t* p, uint32_t sz,
                               DecodedSourceStreams& out) const {
    if (sz < sizeof(SourceRtkStatusPayload)) return;
    SourceRtkStatusPayload raw{};
    std::memcpy(&raw, p, sizeof(SourceRtkStatusPayload));
    DecodedSourceStreams::RtkStatus s;
    s.ts_ns            = ts_ns;
    s.fix_type         = raw.fix_type;
    s.correction_age_s = raw.correction_age_s;
    s.baseline_mm      = raw.baseline_mm;
    s.solution_status  = raw.solution_status;
    out.rtk.push_back(s);
}

void SourceDemuxer::demux_scenario_meta(ns_t ts_ns, const uint8_t* p, uint32_t sz,
                                         DecodedSourceStreams& out) const {
    if (sz < 2u) return;
    DecodedSourceStreams::ScenarioMeta m;
    m.ts_ns = ts_ns;

    // scenario_id_len + scenario_id
    uint32_t off = 0;
    auto read_str = [&](std::string& dst) {
        if (off + 2 > sz) return;
        uint16_t len = 0;
        std::memcpy(&len, p + off, 2); off += 2;
        if (len > 64 || off + len > sz) return;
        dst.assign(reinterpret_cast<const char*>(p + off), len);
        off += len;
    };
    read_str(m.scenario_id);
    read_str(m.source_id);
    read_str(m.config_version);
    out.scenario_metas.push_back(std::move(m));
}

void SourceDemuxer::demux_diagnostics(ns_t ts_ns, const uint8_t* p, uint32_t sz,
                                       DecodedSourceStreams& out) const {
    if (sz < sizeof(SourceDiagnosticsHeader)) return;
    SourceDiagnosticsHeader raw{};
    std::memcpy(&raw, p, sizeof(SourceDiagnosticsHeader));

    DecodedSourceStreams::Diagnostics d;
    d.ts_ns         = ts_ns;
    d.health_state  = raw.health_state;
    d.siren_state   = raw.siren_state;
    d.battery_pct   = raw.battery_pct;
    d.disk_free_pct = raw.disk_free_pct;
    d.temperature_c = raw.temperature_c * 0.1f;

    uint32_t header_sz = static_cast<uint32_t>(sizeof(SourceDiagnosticsHeader));
    if (raw.message_len > 0 && header_sz + raw.message_len <= sz) {
        d.message.assign(reinterpret_cast<const char*>(p + header_sz), raw.message_len);
    }
    out.diagnostics.push_back(std::move(d));
}

} // namespace ego_offline::parse
