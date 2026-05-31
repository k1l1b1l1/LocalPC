#pragma once

#include "ego_offline/ego_contract_protocol.hpp"
#include "ego_offline/parse/packet_demuxer.hpp"

namespace ego_offline::parse {

class ContractDemuxer {
public:
    bool demux(const contract::EgoFrameHeader& header,
               const uint8_t* payload,
               uint32_t size,
               DecodedEgoStreams& out) const;
};

}  // namespace ego_offline::parse
