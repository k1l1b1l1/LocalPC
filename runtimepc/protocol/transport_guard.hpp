#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "checksum.hpp"
#include "serialization.hpp"
#include "validation.hpp"

namespace ego::protocol {

struct TransportCounters {
    std::uint64_t accepted = 0U;
    std::uint64_t rejected = 0U;
    std::uint64_t out_of_order = 0U;
    std::uint64_t seq_gaps = 0U;
    std::uint64_t packets_lost = 0U;
    std::uint64_t wraparound_events = 0U;
};

enum class TransportRejectReason {
    kNone = 0,
    kParseError,
    kInvalidMagic,
    kInvalidVersion,
    kBadLength,
    kBadChecksum,
    kOutOfOrder,
    kUnknownType
};

struct TransportResult {
    bool accepted = false;
    TransportRejectReason reason = TransportRejectReason::kNone;
};

class TransportGuard {
public:
    explicit TransportGuard(std::uint32_t max_payload_bytes = 8192U)
        : max_payload_bytes_(max_payload_bytes) {}

    bool Validate(const std::vector<std::uint8_t>& packet_bytes) {
        return ValidateDetailed(packet_bytes).accepted;
    }

    TransportResult ValidateDetailed(const std::vector<std::uint8_t>& packet_bytes) {
        try {
            const auto packet = DeserializePacket(packet_bytes);
            const auto validation =
                ValidatePacketVerbose(packet.header, packet.payload, max_payload_bytes_);
            if (!validation.accepted) {
                ++counters_.rejected;
                return {false, ReasonFromValidation(validation.reason)};
            }
            if (seen_first_) {
                const std::int32_t diff = static_cast<std::int32_t>(packet.header.seq - last_seq_);
                if (diff <= 0) {
                    ++counters_.rejected;
                    ++counters_.out_of_order;
                    return {false, TransportRejectReason::kOutOfOrder};
                }
                if (packet.header.seq < last_seq_) {
                    ++counters_.wraparound_events;
                }
                if (diff > 1) {
                    ++counters_.seq_gaps;
                    counters_.packets_lost += static_cast<std::uint64_t>(diff - 1);
                }
            }
            last_seq_ = packet.header.seq;
            seen_first_ = true;
            ++counters_.accepted;
            return {true, TransportRejectReason::kNone};
        } catch (...) {
            ++counters_.rejected;
            return {false, TransportRejectReason::kParseError};
        }
    }

    const TransportCounters& Counters() const { return counters_; }

private:
    static TransportRejectReason ReasonFromValidation(const std::string& reason) {
        if (reason == "invalid magic") {
            return TransportRejectReason::kInvalidMagic;
        }
        if (reason == "checksum mismatch") {
            return TransportRejectReason::kBadChecksum;
        }
        if (reason == "payload_size mismatch" || reason == "payload_size exceeds limit") {
            return TransportRejectReason::kBadLength;
        }
        if (reason == "unsupported protocol_version" || reason == "unsupported payload_version") {
            return TransportRejectReason::kInvalidVersion;
        }
        if (reason == "unknown packet type") {
            return TransportRejectReason::kUnknownType;
        }
        return TransportRejectReason::kParseError;
    }

    std::uint32_t max_payload_bytes_ = 8192U;
    std::uint32_t last_seq_ = 0U;
    bool seen_first_ = false;
    TransportCounters counters_{};
};

}  // namespace ego::protocol
