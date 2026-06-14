#include "ego_runtime/nav_provider.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace ego_runtime {
namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t NowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
}

std::string Trim(const std::string& value) {
    std::size_t first = 0U;
    while (first < value.size() && std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

std::vector<std::string> Split(const std::string& value, const char delim) {
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(item);
    }
    return out;
}

bool ParseInt(const std::string& value, int* out) {
    if (out == nullptr || value.empty()) {
        return false;
    }
    try {
        *out = std::stoi(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseFloat(const std::string& value, float* out) {
    if (out == nullptr || value.empty()) {
        return false;
    }
    try {
        *out = std::stof(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseDouble(const std::string& value, double* out) {
    if (out == nullptr || value.empty()) {
        return false;
    }
    try {
        *out = std::stod(value);
        return true;
    } catch (...) {
        return false;
    }
}

double NmeaToDegrees(const std::string& value, const bool is_latitude) {
    if (value.empty()) {
        return 0.0;
    }
    const int degree_digits = is_latitude ? 2 : 3;
    if (static_cast<int>(value.size()) <= degree_digits) {
        return 0.0;
    }
    const double degrees = std::stod(value.substr(0U, static_cast<std::size_t>(degree_digits)));
    const double minutes = std::stod(value.substr(static_cast<std::size_t>(degree_digits)));
    return degrees + (minutes / 60.0);
}

bool ValidateChecksum(const std::string& line) {
    if (line.size() < 4U || line.front() != '$') {
        return false;
    }
    const std::size_t star = line.find('*');
    if (star == std::string::npos || star + 2U >= line.size()) {
        return false;
    }
    std::uint8_t checksum = 0U;
    for (std::size_t i = 1U; i < star; ++i) {
        checksum ^= static_cast<std::uint8_t>(line[i]);
    }
    try {
        const int expected = std::stoi(line.substr(star + 1U, 2U), nullptr, 16);
        return checksum == static_cast<std::uint8_t>(expected);
    } catch (...) {
        return false;
    }
}

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

void CloseSocket(const SocketHandle socket_fd) {
#if defined(_WIN32)
    if (socket_fd != INVALID_SOCKET) {
        closesocket(socket_fd);
    }
#else
    if (socket_fd >= 0) {
        close(socket_fd);
    }
#endif
}

bool InitializeSockets() {
#if defined(_WIN32)
    static bool initialized = false;
    static bool ok = false;
    if (!initialized) {
        WSADATA wsa{};
        ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
        initialized = true;
    }
    return ok;
#else
    return true;
#endif
}

SocketHandle ConnectTcp(const std::string& host, const std::uint16_t port) {
    if (!InitializeSockets()) {
        return kInvalidSocket;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    const std::string port_string = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_string.c_str(), &hints, &result) != 0 || result == nullptr) {
        return kInvalidSocket;
    }

    SocketHandle socket_fd = kInvalidSocket;
    for (addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (socket_fd == kInvalidSocket) {
            continue;
        }
        if (connect(socket_fd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) {
            break;
        }
        CloseSocket(socket_fd);
        socket_fd = kInvalidSocket;
    }

    freeaddrinfo(result);
    return socket_fd;
}

int WaitReadable(const SocketHandle socket_fd, const long timeout_ms) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(socket_fd, &readfds);

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000L;
    tv.tv_usec = (timeout_ms % 1000L) * 1000L;

    return select(static_cast<int>(socket_fd) + 1, &readfds, nullptr, nullptr, &tv);
}

std::string SourceTag(const RuntimeConfig& config) {
    if (config.nav_mode == "tcp_nmea") {
        return "local_m2_tcp";
    }
    return "local_m2";
}

}  // namespace

NavProvider::NavProvider(RuntimeConfig config) : config_(std::move(config)) {
    status_ = IsEnabled() ? "idle" : "disabled";
}

NavProvider::~NavProvider() {
    Stop();
}

bool NavProvider::IsEnabled() const {
    return config_.nav_fallback_enabled && config_.nav_mode == "tcp_nmea";
}

bool NavProvider::Start() {
    if (!IsEnabled()) {
        UpdateStatus("disabled");
        return true;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (running_) {
        return true;
    }
    stop_requested_ = false;
    running_ = true;
    reader_thread_ = std::thread([this]() { ReaderLoop(); });
    return true;
}

void NavProvider::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!running_) {
            return;
        }
        stop_requested_ = true;
    }
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
}

bool NavProvider::ShouldStop() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stop_requested_;
}

bool NavProvider::GetSnapshot(NavSnapshot* snapshot) const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!has_snapshot_) {
        return false;
    }
    const std::uint64_t age_ms = NowMs() > last_update_ms_ ? (NowMs() - last_update_ms_) : 0U;
    if (config_.nav_stale_timeout_ms > 0U &&
        age_ms > static_cast<std::uint64_t>(config_.nav_stale_timeout_ms)) {
        return false;
    }
    if (snapshot != nullptr) {
        *snapshot = latest_;
    }
    return true;
}

std::string NavProvider::Status() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (has_snapshot_ && config_.nav_stale_timeout_ms > 0U) {
        const std::uint64_t age_ms = NowMs() > last_update_ms_ ? (NowMs() - last_update_ms_) : 0U;
        if (age_ms > static_cast<std::uint64_t>(config_.nav_stale_timeout_ms)) {
            if (!input_label_.empty()) {
                return "stale:" + input_label_;
            }
            return "stale";
        }
    }
    return status_;
}

void NavProvider::UpdateStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(mu_);
    status_ = status;
}

void NavProvider::ReaderLoop() {
    const std::string host = config_.nav_host.empty() ? "127.0.0.1" : config_.nav_host;
    const std::uint16_t port = config_.nav_port == 0U ? 3000U : config_.nav_port;
    const std::string label = host + ":" + std::to_string(port);

    NavSnapshot candidate{};
    candidate.source = SourceTag(config_);

    while (!ShouldStop()) {
        SocketHandle socket_fd = ConnectTcp(host, port);
        if (socket_fd == kInvalidSocket) {
            UpdateStatus("connect_failed:" + label);
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            input_label_ = label;
            status_ = "reading:" + label;
        }

        std::string buffer;
        buffer.reserve(4096U);

        while (!ShouldStop()) {
            const int ready = WaitReadable(socket_fd, 250L);
            if (ready < 0) {
                UpdateStatus("connect_failed:" + label);
                break;
            }
            if (ready == 0) {
                continue;
            }

            std::array<char, 512> chunk{};
#if defined(_WIN32)
            const int n = recv(socket_fd, chunk.data(), static_cast<int>(chunk.size()), 0);
#else
            const int n = static_cast<int>(recv(socket_fd, chunk.data(), chunk.size(), 0));
#endif
            if (n <= 0) {
                UpdateStatus("connect_failed:" + label);
                break;
            }

            buffer.append(chunk.data(), static_cast<std::size_t>(n));
            while (true) {
                const std::size_t newline = buffer.find('\n');
                if (newline == std::string::npos) {
                    break;
                }
                std::string line = Trim(buffer.substr(0U, newline));
                buffer.erase(0U, newline + 1U);
                if (line.empty() || !ValidateChecksum(line)) {
                    continue;
                }

                const std::size_t star = line.find('*');
                const std::string body = line.substr(1U, star - 1U);
                const auto fields = Split(body, ',');
                if (fields.empty()) {
                    continue;
                }

                const std::string type = fields[0];
                if (type.size() >= 3U && type.substr(type.size() - 3U) == "RMC") {
                    if (fields.size() < 9U || fields[2] != "A") {
                        continue;
                    }
                    float speed_knots = 0.0f;
                    if (ParseFloat(fields[7], &speed_knots)) {
                        candidate.speed_mps = speed_knots * 0.514444f;
                    }
                    float heading_deg = 0.0f;
                    if (ParseFloat(fields[8], &heading_deg)) {
                        candidate.heading_deg = heading_deg;
                    }
                    continue;
                }

                if (type.size() < 3U || type.substr(type.size() - 3U) != "GGA") {
                    continue;
                }
                if (fields.size() < 10U) {
                    continue;
                }

                int fix_quality = 0;
                int satellites = 0;
                if (!ParseInt(fields[6], &fix_quality) || fix_quality <= 0) {
                    continue;
                }

                double latitude_deg = NmeaToDegrees(fields[2], true);
                double longitude_deg = NmeaToDegrees(fields[4], false);
                if (fields[3] == "S") {
                    latitude_deg = -latitude_deg;
                }
                if (fields[5] == "W") {
                    longitude_deg = -longitude_deg;
                }
                if (latitude_deg == 0.0 && longitude_deg == 0.0) {
                    continue;
                }

                candidate.latitude_deg = latitude_deg;
                candidate.longitude_deg = longitude_deg;
                ParseDouble(fields[9], &candidate.altitude_m);
                ParseInt(fields[7], &satellites);
                ParseFloat(fields[8], &candidate.hdop);

                {
                    std::lock_guard<std::mutex> lock(mu_);
                    ++sample_counter_;
                    candidate.sample_id = sample_counter_;
                    candidate.received_ms = NowMs();
                    candidate.fix_quality = static_cast<std::uint8_t>(std::max(0, fix_quality));
                    candidate.satellites = static_cast<std::uint8_t>(std::max(0, satellites));
                    latest_ = candidate;
                    has_snapshot_ = true;
                    last_update_ms_ = candidate.received_ms;

                    std::ostringstream status;
                    status << "live:" << label
                           << " fix=" << static_cast<int>(latest_.fix_quality)
                           << " sats=" << static_cast<int>(latest_.satellites);
                    status_ = status.str();
                }
            }
        }

        CloseSocket(socket_fd);
        if (!ShouldStop()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
}

}  // namespace ego_runtime
