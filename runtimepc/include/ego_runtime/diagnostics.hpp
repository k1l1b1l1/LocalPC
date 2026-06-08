#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ego_runtime {

struct RuntimeMetrics {
    std::uint64_t packets_received = 0U;
    std::uint64_t packets_written = 0U;
    std::uint64_t bad_packets = 0U;
    std::uint64_t seq_gaps = 0U;
    std::uint64_t packets_lost = 0U;
    std::uint64_t out_of_order = 0U;
    std::uint64_t packets_replayed = 0U;
    std::uint64_t reconnect_count = 0U;
    double data_link_down_sec = 0.0;
    std::uint64_t backfill_gap_frames = 0U;
    std::uint64_t time_gaps = 0U;
    std::uint64_t buffer_dropped = 0U;
    double write_mbps = 0.0;
    double disk_free_gb = 0.0;
    std::string health = "ok";
    std::string adsp_status = "unknown";
    std::unordered_map<std::string, std::uint64_t> reject_by_reason{};
};

class DiagnosticsCollector {
public:
    void OnPacketReceived() { ++packets_received_; }

    void OnPacketWritten(std::size_t bytes) {
        ++packets_written_;
        bytes_written_ += bytes;
    }

    void OnBadPacket(const std::string& reason) {
        ++bad_packets_;
        std::lock_guard<std::mutex> lock(mu_);
        reject_by_reason_[reason.empty() ? "unknown" : reason]++;
    }

    void OnTimeGap() { ++time_gaps_; }

    void OnSeqGap(std::uint64_t lost) {
        ++seq_gaps_;
        packets_lost_ += lost;
    }

    void OnOutOfOrder() { ++out_of_order_; }

    void OnPacketReplayed() { ++packets_replayed_; }

    void OnReconnect() { ++reconnect_count_; }

    void SetDataLinkDownSec(double sec) { data_link_down_sec_.store(sec); }

    void SetBackfillGapFrames(std::uint64_t frames) { backfill_gap_frames_.store(frames); }

    void SetBufferDropped(std::uint64_t dropped) { buffer_dropped_.store(dropped); }

    void SetDiskFreeGb(double gb) { disk_free_gb_.store(gb); }

    void SetHealth(const std::string& health) { health_ = health; }

    void SetAdspStatus(const std::string& status) { adsp_status_ = status; }

    RuntimeMetrics Snapshot(double elapsed_sec) const {
        RuntimeMetrics m{};
        m.packets_received = packets_received_.load();
        m.packets_written = packets_written_.load();
        m.bad_packets = bad_packets_.load();
        m.seq_gaps = seq_gaps_.load();
        m.packets_lost = packets_lost_.load();
        m.out_of_order = out_of_order_.load();
        m.packets_replayed = packets_replayed_.load();
        m.reconnect_count = reconnect_count_.load();
        m.data_link_down_sec = data_link_down_sec_.load();
        m.backfill_gap_frames = backfill_gap_frames_.load();
        m.time_gaps = time_gaps_.load();
        m.buffer_dropped = buffer_dropped_.load();
        m.disk_free_gb = disk_free_gb_.load();
        m.health = health_;
        m.adsp_status = adsp_status_;
        if (elapsed_sec > 0.0) {
            m.write_mbps = (static_cast<double>(bytes_written_.load()) * 8.0) / (elapsed_sec * 1'000'000.0);
        }
        std::lock_guard<std::mutex> lock(mu_);
        m.reject_by_reason = reject_by_reason_;
        return m;
    }

    void ResetRates() { bytes_written_ = 0U; }

private:
    std::atomic<std::uint64_t> packets_received_{0};
    std::atomic<std::uint64_t> packets_written_{0};
    std::atomic<std::uint64_t> bad_packets_{0};
    std::atomic<std::uint64_t> seq_gaps_{0};
    std::atomic<std::uint64_t> packets_lost_{0};
    std::atomic<std::uint64_t> out_of_order_{0};
    std::atomic<std::uint64_t> packets_replayed_{0};
    std::atomic<std::uint64_t> reconnect_count_{0};
    std::atomic<double> data_link_down_sec_{0.0};
    std::atomic<std::uint64_t> backfill_gap_frames_{0};
    std::atomic<std::uint64_t> time_gaps_{0};
    std::atomic<std::uint64_t> buffer_dropped_{0};
    std::atomic<std::uint64_t> bytes_written_{0};
    std::atomic<double> disk_free_gb_{0.0};
    std::string health_ = "ok";
    std::string adsp_status_ = "unknown";
    mutable std::mutex mu_{};
    std::unordered_map<std::string, std::uint64_t> reject_by_reason_{};
};

}  // namespace ego_runtime
