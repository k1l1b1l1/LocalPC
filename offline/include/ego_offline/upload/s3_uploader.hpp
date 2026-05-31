#pragma once

#include "ego_offline/config.hpp"
#include "ego_offline/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ego_offline::upload {

struct UploadFileResult {
    std::string local_path;
    std::string object_key;
    std::uint64_t size_bytes = 0;
    std::string sha256;
    std::string etag;
    std::string status; // ok | failed | skipped
    std::string message;
};

struct UploadReport {
    std::string upload_manifest_version = "1.0";
    std::string session_id;
    std::string generated_at_utc;
    std::string bucket;
    std::string prefix;
    std::string upload_status; // ok | failed | skipped | blocked
    std::string blocked_reason;
    std::vector<UploadFileResult> files;

    void write(const std::filesystem::path& path) const;
};

class S3Uploader {
public:
    explicit S3Uploader(Config cfg);

    ExitCode upload_session(const std::filesystem::path& session_dir,
                            const std::filesystem::path& offline_dir,
                            bool dry_run = false);

private:
    Config cfg_;

    std::string read_pipeline_status(const std::filesystem::path& offline_dir) const;
    std::string read_session_id(const std::filesystem::path& session_dir,
                                const std::filesystem::path& offline_dir) const;
    bool gate_allowed(const std::filesystem::path& offline_dir,
                      const std::string& pipeline_status,
                      std::string& blocked_reason) const;
    std::vector<std::filesystem::path> collect_files(
        const std::filesystem::path& offline_dir) const;
    std::vector<std::filesystem::path> collect_session_files(
        const std::filesystem::path& session_dir) const;
    std::string object_key(const std::string& session_id,
                           const std::string& subdir,
                           const std::string& basename) const;
    ExitCode upload_file_batch(const std::vector<std::filesystem::path>& files,
                               const std::string& session_id,
                               const std::string& subdir,
                               UploadReport& report,
                               bool& all_ok);
};

} // namespace ego_offline::upload
