#include "ego_runtime/session_checkpoint.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "ego_runtime/util.hpp"

namespace ego_runtime {
namespace {

bool ParseIndexLine(const std::string& line, IndexTail& out) {
    std::uint64_t offset = 0U;
    std::uint64_t t0_ns = 0U;
    std::uint32_t frame_type = 0U;
    std::uint64_t seq = 0U;
    std::uint32_t chunk_id = 0U;
    std::uint32_t payload_size = 0U;
    char comma = '\0';
    std::istringstream iss(line);
    if (!(iss >> offset >> comma >> t0_ns >> comma >> frame_type >> comma >> seq >> comma >> chunk_id >>
          comma >> payload_size)) {
        return false;
    }
    out.last_seq = seq;
    out.last_ts_ns = t0_ns;
    out.chunk_id = chunk_id;
    out.chunk_offset = offset;
    return true;
}

std::string ExtractJsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\": \"";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return "";
    }
    const auto start = pos + needle.size();
    const auto end = json.find('"', start);
    if (end == std::string::npos) {
        return "";
    }
    return json.substr(start, end - start);
}

std::uint64_t ExtractJsonUint64(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\": ";
    const auto pos = json.find(needle);
    if (pos == std::string::npos) {
        return 0U;
    }
    const auto start = pos + needle.size();
    return std::stoull(json.substr(start));
}

}  // namespace

std::string CheckpointPath(const std::string& session_dir) {
    return (std::filesystem::path(session_dir) / "logs" / "session_checkpoint.json").string();
}

bool SessionIsFinalized(const std::string& session_dir) {
    return std::filesystem::exists(std::filesystem::path(session_dir) / "final_runtime_summary.json");
}

bool ReadIndexTail(const std::string& session_dir, IndexTail& out) {
    const std::string index_path = (std::filesystem::path(session_dir) / "ego.index").string();
    std::ifstream in(index_path);
    if (!in.good()) {
        return false;
    }
    IndexTail tail{};
    std::string line;
    std::uint32_t lines = 0U;
    while (std::getline(in, line)) {
        if (line.empty()) {
            continue;
        }
        IndexTail parsed{};
        if (!ParseIndexLine(line, parsed)) {
            continue;
        }
        tail = parsed;
        ++lines;
    }
    if (lines == 0U) {
        return false;
    }
    tail.line_count = lines;
    out = tail;
    return true;
}

bool LoadCheckpoint(const std::string& session_dir, SessionCheckpoint& out) {
    const std::string path = CheckpointPath(session_dir);
    const auto bytes = ReadFileBytes(path);
    if (bytes.empty()) {
        IndexTail tail{};
        if (!ReadIndexTail(session_dir, tail)) {
            return false;
        }
        out.last_seq = tail.last_seq;
        out.last_ts_ns = tail.last_ts_ns;
        out.chunk_id = tail.chunk_id;
        const auto dir_name = std::filesystem::path(session_dir).filename().string();
        out.session_id = dir_name;
        return true;
    }
    const std::string json(bytes.begin(), bytes.end());
    out.session_id = ExtractJsonString(json, "session_id");
    out.board_session_id = ExtractJsonString(json, "board_session_id");
    out.last_seq = ExtractJsonUint64(json, "last_seq");
    out.last_ts_ns = ExtractJsonUint64(json, "last_ts_ns");
    out.chunk_id = static_cast<std::uint32_t>(ExtractJsonUint64(json, "chunks_flushed"));
    out.data_link = ExtractJsonString(json, "data_link");
    out.updated_at_utc = ExtractJsonString(json, "updated_at_utc");
    if (out.last_seq == 0U) {
        IndexTail tail{};
        if (ReadIndexTail(session_dir, tail) && tail.last_seq > out.last_seq) {
            out.last_seq = tail.last_seq;
            out.last_ts_ns = tail.last_ts_ns;
            out.chunk_id = tail.chunk_id;
        }
    }
    return !out.session_id.empty() || !session_dir.empty();
}

bool WriteCheckpoint(const std::string& session_dir, const SessionCheckpoint& checkpoint) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(session_dir) / "logs", ec);
    std::ostringstream json;
    json << "{\n";
    json << "  \"session_id\": \"" << JsonEscape(checkpoint.session_id) << "\",\n";
    json << "  \"board_session_id\": \"" << JsonEscape(checkpoint.board_session_id) << "\",\n";
    json << "  \"last_seq\": " << checkpoint.last_seq << ",\n";
    json << "  \"last_ts_ns\": " << checkpoint.last_ts_ns << ",\n";
    json << "  \"chunks_flushed\": " << checkpoint.chunk_id << ",\n";
    json << "  \"data_link\": \"" << JsonEscape(checkpoint.data_link) << "\",\n";
    json << "  \"updated_at_utc\": \"" << JsonEscape(checkpoint.updated_at_utc) << "\"\n";
    json << "}\n";
    return WriteTextAtomic(CheckpointPath(session_dir), json.str());
}

std::string FindResumableSessionDir(const std::string& data_root) {
    const std::filesystem::path sessions_root = std::filesystem::path(data_root) / "sessions";
    if (!std::filesystem::exists(sessions_root)) {
        return "";
    }
    std::filesystem::file_time_type best_time{};
    std::string best_dir;
    bool have_best = false;
    for (const auto& entry : std::filesystem::directory_iterator(sessions_root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string dir = entry.path().string();
        if (SessionIsFinalized(dir)) {
            continue;
        }
        if (!std::filesystem::exists(entry.path() / "ego.index") &&
            !std::filesystem::exists(CheckpointPath(dir))) {
            continue;
        }
        const auto mtime = entry.last_write_time();
        if (!have_best || mtime > best_time) {
            best_time = mtime;
            best_dir = dir;
            have_best = true;
        }
    }
    return best_dir;
}

int AbandonResumableSessions(const std::string& data_root, const std::string& reason) {
    const std::filesystem::path sessions_root = std::filesystem::path(data_root) / "sessions";
    if (!std::filesystem::exists(sessions_root)) {
        return 0;
    }
    const std::string ts = UtcNowIso8601();
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(sessions_root)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string dir = entry.path().string();
        if (SessionIsFinalized(dir)) {
            continue;
        }
        if (!std::filesystem::exists(entry.path() / "ego.index") &&
            !std::filesystem::exists(CheckpointPath(dir))) {
            continue;
        }
        std::filesystem::create_directories(entry.path() / "logs");
        std::ostringstream json;
        json << "{\n";
        json << "  \"status\": \"abandoned\",\n";
        json << "  \"reason\": \"" << JsonEscape(reason) << "\",\n";
        json << "  \"abandoned_at_utc\": \"" << JsonEscape(ts) << "\"\n";
        json << "}\n";
        WriteTextAtomic(dir + "/final_runtime_summary.json", json.str());
        WriteTextAtomic(dir + "/logs/abandon_reason.txt", reason + "\n");
        ++count;
    }
    return count;
}

}  // namespace ego_runtime
