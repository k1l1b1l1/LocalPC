#pragma once

#include "ego_offline/types.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace ego_offline::load {

struct EgoNavSidecarLoadResult {
    std::vector<GnssPoint> points;
    std::string source_label;
    std::size_t invalid_lines = 0U;
};

EgoNavSidecarLoadResult ReadEgoNavSidecar(const std::filesystem::path& path);

}  // namespace ego_offline::load
