#include "ego_runtime/nav_history.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>

namespace ego_runtime {
namespace {

std::string Trim(const std::string& value) {
    std::size_t first = 0U;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first])) != 0) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1U])) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

bool ExtractIntegerField(const std::string& json_line,
                         const std::string& key,
                         std::uint64_t* value) {
    if (value == nullptr) {
        return false;
    }
    const std::string marker = "\"" + key + "\"";
    const auto key_pos = json_line.find(marker);
    if (key_pos == std::string::npos) {
        return false;
    }
    const auto colon = json_line.find(':', key_pos + marker.size());
    if (colon == std::string::npos) {
        return false;
    }
    std::size_t start = colon + 1U;
    while (start < json_line.size() &&
           std::isspace(static_cast<unsigned char>(json_line[start])) != 0) {
        ++start;
    }
    std::size_t end = start;
    while (end < json_line.size() &&
           std::isdigit(static_cast<unsigned char>(json_line[end])) != 0) {
        ++end;
    }
    if (start == end) {
        return false;
    }
    try {
        *value = std::stoull(json_line.substr(start, end - start));
        return true;
    } catch (...) {
        return false;
    }
}

bool FileHasContent(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) &&
           std::filesystem::is_regular_file(path, ec) &&
           std::filesystem::file_size(path, ec) > 0U;
}

}  // namespace

std::string ResolveNavHistoryPath(const RuntimeConfig& config) {
    std::filesystem::path base(config.data_root);
    if (base.filename() == "sessions") {
        base = base.parent_path();
    }
    if (base.empty()) {
        base = std::filesystem::current_path();
    }
    return (base / "nav" / "ego_nav_history.jsonl").string();
}

std::string ResolveNavHistoryPath(const std::string& runtime_root) {
    if (!Trim(runtime_root).empty()) {
        return (std::filesystem::path(Trim(runtime_root)) / "var" / "nav" /
                "ego_nav_history.jsonl")
            .string();
    }
    RuntimeConfig fallback{};
    return ResolveNavHistoryPath(fallback);
}

std::uint64_t UtcNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

NavHistoryMaterializeResult MaterializeNavHistoryWindow(
    const std::string& runtime_root,
    const std::string& session_dir,
    const std::string& sidecar_filename,
    const std::uint64_t start_ts_ns,
    const std::uint64_t end_ts_ns) {
    NavHistoryMaterializeResult result;
    result.history_path = ResolveNavHistoryPath(runtime_root);
    if (Trim(session_dir).empty() || Trim(sidecar_filename).empty()) {
        return result;
    }

    const std::filesystem::path sidecar_path =
        std::filesystem::path(Trim(session_dir)) / Trim(sidecar_filename);
    result.sidecar_path = sidecar_path.string();

    if (FileHasContent(sidecar_path)) {
        result.used_existing_sidecar = true;
        return result;
    }

    if (start_ts_ns == 0U || end_ts_ns == 0U || end_ts_ns < start_ts_ns) {
        return result;
    }

    const std::filesystem::path history_path(result.history_path);
    if (!FileHasContent(history_path)) {
        return result;
    }
    result.history_available = true;

    std::ifstream input(history_path);
    if (!input.good()) {
        return result;
    }

    std::error_code ec;
    std::filesystem::create_directories(sidecar_path.parent_path(), ec);
    const std::filesystem::path tmp_path = sidecar_path.string() + ".tmp";
    std::ofstream output(tmp_path, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return result;
    }

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (Trim(line).empty()) {
            continue;
        }
        std::uint64_t ts_ns = 0U;
        if (!ExtractIntegerField(line, "ts_ns", &ts_ns)) {
            ++result.skipped_lines;
            continue;
        }
        if (ts_ns < start_ts_ns || ts_ns > end_ts_ns) {
            continue;
        }
        output << line << '\n';
        ++result.copied_samples;
    }
    output.close();

    if (result.copied_samples == 0U) {
        std::filesystem::remove(tmp_path, ec);
        result.sidecar_path.clear();
        return result;
    }

    std::filesystem::rename(tmp_path, sidecar_path, ec);
    if (ec) {
        std::filesystem::remove(sidecar_path, ec);
        std::filesystem::rename(tmp_path, sidecar_path, ec);
    }
    if (ec) {
        result.copied_samples = 0U;
        std::filesystem::remove(tmp_path, ec);
        result.sidecar_path.clear();
        return result;
    }
    result.sidecar_path = sidecar_path.string();
    return result;
}

}  // namespace ego_runtime
