#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace ego_runtime {

struct BufferedPacket {
    std::vector<std::uint8_t> bytes;
};

class PacketBuffer {
public:
    explicit PacketBuffer(std::size_t capacity_packets, std::uint64_t max_bytes = 512ULL * 1024ULL * 1024ULL)
        : capacity_packets_(capacity_packets), max_bytes_(max_bytes) {}

    void Push(BufferedPacket packet) {
        std::lock_guard<std::mutex> lock(mu_);
        while (!queue_.empty() &&
               (queue_.size() >= capacity_packets_ || total_bytes_ + packet.bytes.size() > max_bytes_)) {
            total_bytes_ -= queue_.front().bytes.size();
            queue_.pop_front();
            ++dropped_;
            degraded_ = true;
        }
        total_bytes_ += packet.bytes.size();
        queue_.push_back(std::move(packet));
    }

    std::optional<BufferedPacket> Pop() {
        std::lock_guard<std::mutex> lock(mu_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        BufferedPacket packet = std::move(queue_.front());
        queue_.pop_front();
        total_bytes_ -= packet.bytes.size();
        return packet;
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return queue_.size();
    }

    std::uint64_t Dropped() const {
        std::lock_guard<std::mutex> lock(mu_);
        return dropped_;
    }

    bool Degraded() const { return degraded_.load(); }

private:
    std::size_t capacity_packets_ = 0U;
    std::uint64_t max_bytes_ = 0U;
    mutable std::mutex mu_{};
    std::deque<BufferedPacket> queue_{};
    std::uint64_t total_bytes_ = 0U;
    std::uint64_t dropped_ = 0U;
    std::atomic<bool> degraded_{false};
};

}  // namespace ego_runtime
