#include "ego_offline/load/ego_nav_sidecar_reader.hpp"

#include "ego_offline/json_reader.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ego_offline::load {

EgoNavSidecarLoadResult ReadEgoNavSidecar(const std::filesystem::path& path) {
    EgoNavSidecarLoadResult result;
    if (path.empty() || !std::filesystem::exists(path)) {
        return result;
    }

    std::ifstream input(path);
    if (!input.good()) {
        throw std::runtime_error("cannot open ego nav sidecar: " + path.string());
    }

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        try {
            const JsonValue json = json_parse(line);
            if (!json.is_object()) {
                ++result.invalid_lines;
                continue;
            }

            GnssPoint point;
            point.ts_ns = static_cast<ns_t>(json.value<int64_t>("ts_ns", 0));
            point.latitude_deg = json.value<double>("lat_deg", 0.0);
            point.longitude_deg = json.value<double>("lon_deg", 0.0);
            point.altitude_m = json.value<double>("alt_m", 0.0);
            point.speed_mps = static_cast<float>(json.value<double>("speed_mps", 0.0));
            point.heading_deg = static_cast<float>(json.value<double>("heading_deg", 0.0));
            point.fix_quality = static_cast<std::uint8_t>(json.value<int>("fix_quality", 0));
            point.satellites = static_cast<std::uint8_t>(json.value<int>("satellites", 0));
            point.hdop = static_cast<float>(json.value<double>("hdop", 99.9));
            if (result.source_label.empty()) {
                result.source_label = json.value<std::string>("source", "");
            }
            if (point.ts_ns <= 0 || point.fix_quality == 0) {
                ++result.invalid_lines;
                continue;
            }
            result.points.push_back(point);
        } catch (...) {
            ++result.invalid_lines;
        }
    }

    std::sort(result.points.begin(), result.points.end(),
              [](const GnssPoint& lhs, const GnssPoint& rhs) { return lhs.ts_ns < rhs.ts_ns; });
    result.points.erase(
        std::unique(result.points.begin(), result.points.end(),
                    [](const GnssPoint& lhs, const GnssPoint& rhs) { return lhs.ts_ns == rhs.ts_ns; }),
        result.points.end());
    return result;
}

}  // namespace ego_offline::load
