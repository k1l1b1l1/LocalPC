#pragma once

#include <cstdint>
#include <string>

namespace ego_runtime {

struct SessionCheckpoint {
    std::string session_id;
    std::string board_session_id;
    std::uint64_t last_seq = 0U;
    std::uint64_t last_ts_ns = 0U;
    std::uint32_t chunk_id = 0U;
    std::string data_link = "up";
    std::string updated_at_utc;
};

struct IndexTail {
    std::uint64_t last_seq = 0U;
    std::uint64_t last_ts_ns = 0U;
    std::uint32_t chunk_id = 0U;
    std::uint64_t chunk_offset = 0U;
    std::uint32_t line_count = 0U;
};

bool ReadIndexTail(const std::string& session_dir, IndexTail& out);
bool LoadCheckpoint(const std::string& session_dir, SessionCheckpoint& out);
bool WriteCheckpoint(const std::string& session_dir, const SessionCheckpoint& checkpoint);
std::string CheckpointPath(const std::string& session_dir);
std::string FindResumableSessionDir(const std::string& data_root);
bool SessionIsFinalized(const std::string& session_dir);
int AbandonResumableSessions(const std::string& data_root, const std::string& reason = "ipc_abandon");

}  // namespace ego_runtime
