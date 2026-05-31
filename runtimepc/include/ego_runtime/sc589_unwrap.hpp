#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "ego_runtime/config.hpp"
#include "protocol/checksum.hpp"
#include "protocol/serialization.hpp"
#include "protocol/validation.hpp"

namespace ego_runtime {

struct UnwrapStats {
    std::uint64_t frames_seen = 0U;
    std::uint64_t packets_emitted = 0U;
    std::uint64_t packets_rejected = 0U;
    std::uint64_t reject_crc_outer = 0U;
    std::uint64_t sync_losses = 0U;
};

class Sc589Unwrap {
public:
    using PacketHandler = std::function<void(const std::vector<std::uint8_t>&)>;

    Sc589Unwrap(UnwrapPolicy policy, std::uint32_t max_payload_bytes, PacketHandler handler)
        : policy_(policy), max_payload_bytes_(max_payload_bytes), handler_(std::move(handler)) {}

    void Feed(const std::uint8_t* data, std::size_t size) {
        buffer_.insert(buffer_.end(), data, data + size);
        Consume();
    }

    void Feed(const std::vector<std::uint8_t>& chunk) { Feed(chunk.data(), chunk.size()); }

    void Finalize() {
        if (!buffer_.empty()) {
            if (policy_ == UnwrapPolicy::kOff) {
                TryConsumePacketOnly();
            }
        }
    }

    const UnwrapStats& Stats() const { return stats_; }

private:
    static constexpr std::array<std::uint8_t, 2> kSof{{0xA5U, 0x5AU}};

    void Consume() {
        if (policy_ == UnwrapPolicy::kOff) {
            while (TryConsumePacketOnly()) {
            }
            return;
        }
        if (policy_ == UnwrapPolicy::kOn) {
            while (TryConsumeWrappedFrame()) {
            }
            return;
        }
        while (!buffer_.empty()) {
            if (StartsWithSof()) {
                if (!TryConsumeWrappedFrame()) {
                    break;
                }
                continue;
            }
            if (StartsWithPacketMagic()) {
                if (!TryConsumePacketOnly()) {
                    break;
                }
                continue;
            }
            DropFront(1U, true);
        }
    }

    bool StartsWithSof() const {
        return buffer_.size() >= 2U && buffer_[0] == kSof[0] && buffer_[1] == kSof[1];
    }

    bool StartsWithPacketMagic() const {
        if (buffer_.size() < 4U) {
            return false;
        }
        const std::uint32_t value = static_cast<std::uint32_t>(buffer_[0]) |
                                    (static_cast<std::uint32_t>(buffer_[1]) << 8U) |
                                    (static_cast<std::uint32_t>(buffer_[2]) << 16U) |
                                    (static_cast<std::uint32_t>(buffer_[3]) << 24U);
        return value == ego::protocol::kPacketMagic;
    }

    bool TryConsumePacketOnly() {
        while (buffer_.size() >= ego::protocol::kPacketHeaderSize) {
            std::vector<std::uint8_t> header_bytes(
                buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(ego::protocol::kPacketHeaderSize));
            ego::protocol::PacketHeader header{};
            try {
                header = ego::protocol::DeserializeHeader(header_bytes);
            } catch (...) {
                ++stats_.packets_rejected;
                DropFront(1U, true);
                return true;
            }
            if (header.magic != ego::protocol::kPacketMagic ||
                header.payload_size > max_payload_bytes_) {
                ++stats_.packets_rejected;
                DropFront(1U, true);
                return true;
            }
            const std::size_t packet_size =
                ego::protocol::kPacketHeaderSize + static_cast<std::size_t>(header.payload_size);
            if (buffer_.size() < packet_size) {
                return false;
            }
            std::vector<std::uint8_t> packet(buffer_.begin(),
                                             buffer_.begin() + static_cast<std::ptrdiff_t>(packet_size));
            DropFront(packet_size, false);
            ++stats_.frames_seen;
            Emit(packet);
            return true;
        }
        return false;
    }

    bool TryConsumeWrappedFrame() {
        if (!StartsWithSof() || buffer_.size() < 4U) {
            return false;
        }
        const std::uint16_t wrapped_len =
            static_cast<std::uint16_t>(buffer_[2]) | (static_cast<std::uint16_t>(buffer_[3]) << 8U);
        const std::size_t total_frame_size = 4U + static_cast<std::size_t>(wrapped_len);
        constexpr std::size_t kMinWrappedLen = 1U + ego::protocol::kPacketHeaderSize + 4U;
        if (wrapped_len < kMinWrappedLen || total_frame_size > 4096U) {
            ++stats_.packets_rejected;
            DropFront(1U, true);
            return true;
        }
        if (buffer_.size() < total_frame_size) {
            return false;
        }
        const std::size_t inner_size = static_cast<std::size_t>(wrapped_len) - 1U - 4U;
        const std::size_t crc_offset = 4U + static_cast<std::size_t>(wrapped_len) - 4U;
        const std::uint32_t expected_crc = static_cast<std::uint32_t>(buffer_[crc_offset]) |
                                           (static_cast<std::uint32_t>(buffer_[crc_offset + 1U]) << 8U) |
                                           (static_cast<std::uint32_t>(buffer_[crc_offset + 2U]) << 16U) |
                                           (static_cast<std::uint32_t>(buffer_[crc_offset + 3U]) << 24U);
        std::vector<std::uint8_t> crc_region(
            buffer_.begin() + static_cast<std::ptrdiff_t>(4U),
            buffer_.begin() + static_cast<std::ptrdiff_t>(4U + 1U + inner_size));
        if (ego::protocol::Crc32(crc_region) != expected_crc) {
            ++stats_.reject_crc_outer;
            ++stats_.packets_rejected;
            DropFront(1U, true);
            return true;
        }
        std::vector<std::uint8_t> inner_packet(
            buffer_.begin() + static_cast<std::ptrdiff_t>(5U),
            buffer_.begin() + static_cast<std::ptrdiff_t>(5U + inner_size));
        DropFront(total_frame_size, false);
        ++stats_.frames_seen;
        Emit(inner_packet);
        return true;
    }

    void Emit(const std::vector<std::uint8_t>& packet) {
        try {
            const auto parsed = ego::protocol::DeserializePacket(packet);
            const auto validation =
                ego::protocol::ValidatePacketVerbose(parsed.header, parsed.payload, max_payload_bytes_);
            if (!validation.accepted) {
                ++stats_.packets_rejected;
                return;
            }
        } catch (...) {
            ++stats_.packets_rejected;
            return;
        }
        handler_(packet);
        ++stats_.packets_emitted;
    }

    void DropFront(std::size_t count, bool sync_loss) {
        count = std::min(count, buffer_.size());
        buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
        if (sync_loss) {
            ++stats_.sync_losses;
        }
    }

    UnwrapPolicy policy_ = UnwrapPolicy::kAuto;
    std::uint32_t max_payload_bytes_ = 8192U;
    PacketHandler handler_;
    std::vector<std::uint8_t> buffer_{};
    UnwrapStats stats_{};
};

}  // namespace ego_runtime
