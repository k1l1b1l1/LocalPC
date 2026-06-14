#include "ego_runtime/nav_sidecar_writer.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <sstream>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

#include "ego_runtime/util.hpp"

namespace ego_runtime {
namespace {

bool SyncFile(std::FILE* file) {
    if (file == nullptr) {
        return false;
    }
    if (std::fflush(file) != 0) {
        return false;
    }
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

}  // namespace

NavSidecarWriter::NavSidecarWriter(RuntimeConfig config) : config_(std::move(config)) {}

NavSidecarWriter::~NavSidecarWriter() {
    Close();
}

bool NavSidecarWriter::Open(const std::string& session_dir) {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_ != nullptr) {
        (void)FlushLocked(true);
        std::fclose(file_);
        file_ = nullptr;
    }
    path_.clear();

    if (session_dir.empty() || config_.nav_sidecar_filename.empty()) {
        return false;
    }

    const std::filesystem::path path =
        std::filesystem::path(session_dir) / config_.nav_sidecar_filename;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    file_ = std::fopen(path.string().c_str(), "ab");
    if (file_ == nullptr) {
        return false;
    }
    path_ = path.string();
    last_sample_id_ = 0U;
    last_ts_ns_ = 0U;
    samples_written_ = 0U;
    dirty_samples_ = 0U;
    return true;
}

void NavSidecarWriter::Close() {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_ == nullptr) {
        path_.clear();
        return;
    }
    (void)FlushLocked(true);
    std::fclose(file_);
    file_ = nullptr;
    path_.clear();
}

bool NavSidecarWriter::Write(const NavSnapshot& snapshot, const std::uint64_t ts_ns) {
    std::lock_guard<std::mutex> lock(mu_);
    if (file_ == nullptr || snapshot.sample_id == 0U || ts_ns == 0U) {
        return false;
    }
    if (snapshot.sample_id == last_sample_id_ || ts_ns <= last_ts_ns_) {
        return false;
    }

    std::ostringstream line;
    line << "{\"ts_ns\":" << ts_ns
         << ",\"lat_deg\":" << std::fixed << std::setprecision(8) << snapshot.latitude_deg
         << ",\"lon_deg\":" << std::fixed << std::setprecision(8) << snapshot.longitude_deg
         << ",\"alt_m\":" << std::fixed << std::setprecision(3) << snapshot.altitude_m
         << ",\"speed_mps\":" << std::fixed << std::setprecision(3) << snapshot.speed_mps
         << ",\"heading_deg\":" << std::fixed << std::setprecision(3) << snapshot.heading_deg
         << ",\"fix_quality\":" << static_cast<unsigned int>(snapshot.fix_quality)
         << ",\"satellites\":" << static_cast<unsigned int>(snapshot.satellites)
         << ",\"hdop\":" << std::fixed << std::setprecision(3) << snapshot.hdop
         << ",\"source\":\"" << JsonEscape(snapshot.source) << "\"}\n";

    const std::string text = line.str();
    if (std::fwrite(text.data(), 1U, text.size(), file_) != text.size()) {
        return false;
    }

    last_sample_id_ = snapshot.sample_id;
    last_ts_ns_ = ts_ns;
    ++samples_written_;
    ++dirty_samples_;

    if (dirty_samples_ >= 5U) {
        return FlushLocked(true);
    }
    return FlushLocked(false);
}

std::string NavSidecarWriter::Path() const {
    std::lock_guard<std::mutex> lock(mu_);
    return path_;
}

std::uint64_t NavSidecarWriter::SamplesWritten() const {
    std::lock_guard<std::mutex> lock(mu_);
    return samples_written_;
}

bool NavSidecarWriter::FlushLocked(const bool force_sync) {
    if (file_ == nullptr) {
        return false;
    }
    if (std::fflush(file_) != 0) {
        return false;
    }
    if (force_sync && dirty_samples_ > 0U) {
        if (!SyncFile(file_)) {
            return false;
        }
        dirty_samples_ = 0U;
        return true;
    }
    if (!force_sync) {
        return true;
    }
    dirty_samples_ = 0U;
    return true;
}

}  // namespace ego_runtime
