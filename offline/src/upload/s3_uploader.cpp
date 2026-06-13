#include "ego_offline/upload/s3_uploader.hpp"
#include "ego_offline/upload/s3_client.hpp"
#include "ego_offline/json_writer.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <algorithm>

namespace ego_offline::upload {

namespace {

std::string utc_now_iso8601() {
    auto now = std::chrono::system_clock::now();
    auto tt  = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::string json_get_string(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    if (pos >= json.size()) return {};
    if (json[pos] == '"') {
        ++pos;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return {};
        return json.substr(pos, end - pos);
    }
    auto end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != '\n') ++end;
    std::string v = json.substr(pos, end - pos);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.pop_back();
    return v;
}

std::string content_type_for(const std::string& name) {
    if (name.size() >= 5 && name.substr(name.size() - 5) == ".json") return "application/json";
    if (name.size() >= 7 && name.substr(name.size() - 7) == ".sha256") return "text/plain";
    return "application/octet-stream";
}

std::vector<std::uint8_t> read_file_bytes(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    const auto sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> buf(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(buf.data()), sz);
    return buf;
}

} // namespace

void UploadReport::write(const std::filesystem::path& path) const {
    using namespace json;
    ObjectBuilder root(2, 0);
    root.field("upload_manifest_version", str(upload_manifest_version))
        .field("session_id", str(session_id))
        .field("generated_at_utc", str(generated_at_utc))
        .field("bucket", str(bucket))
        .field("prefix", str(prefix))
        .field("upload_status", str(upload_status));
    if (!blocked_reason.empty())
        root.field("blocked_reason", str(blocked_reason));

    std::vector<std::string> file_objs;
    for (const auto& f : files) {
        ObjectBuilder fb(2, 2);
        fb.field("local_path", str(f.local_path))
          .field("object_key", str(f.object_key))
          .field("size_bytes", num(static_cast<int64_t>(f.size_bytes)))
          .field("sha256", str(f.sha256))
          .field("etag", str(f.etag))
          .field("status", str(f.status))
          .field("message", str(f.message));
        file_objs.push_back(fb.build());
    }
    root.raw_array("files", file_objs);

    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path);
    out << root.build() << '\n';
}

S3Uploader::S3Uploader(Config cfg) : cfg_(std::move(cfg)) {}

std::string S3Uploader::read_pipeline_status(const std::filesystem::path& offline_dir) const {
    const auto p = offline_dir / "session_report.json";
    if (!std::filesystem::exists(p)) return "unknown";
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return json_get_string(ss.str(), "pipeline_status");
}

std::string S3Uploader::read_session_id(const std::filesystem::path& session_dir,
                                        const std::filesystem::path& offline_dir) const {
    const auto p = offline_dir / "session_report.json";
    if (std::filesystem::exists(p)) {
        std::ifstream f(p);
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string id = json_get_string(ss.str(), "session_id");
        if (!id.empty()) return id;
    }
    return session_dir.filename().string();
}

bool S3Uploader::gate_allowed(const std::filesystem::path& offline_dir,
                              const std::string& pipeline_status,
                              std::string& blocked_reason) const {
    const auto mf4 = offline_dir / "session.mf4";
    const auto placeholder = offline_dir / "session.mf4.placeholder";

    if (cfg_.s3.require_real_mdf4) {
        if (!std::filesystem::exists(mf4) || std::filesystem::file_size(mf4) == 0) {
            blocked_reason = "session.mf4 missing";
            return false;
        }
        if (std::filesystem::exists(placeholder)) {
            blocked_reason = "mdf4_stub";
            return false;
        }
    }

    if (cfg_.s3.require_pipeline_success) {
        if (pipeline_status != "success") {
            blocked_reason = "pipeline_status=" + pipeline_status;
            return false;
        }
    }

    if (cfg_.s3.bucket.empty()) {
        blocked_reason = "s3.bucket not configured";
        return false;
    }
    if (cfg_.s3.access_key.empty() || cfg_.s3.secret_key.empty()) {
        blocked_reason = "s3 credentials missing";
        return false;
    }

    return true;
}

std::vector<S3Uploader::UploadCandidate> S3Uploader::collect_files(
    const std::filesystem::path& session_dir,
    const std::filesystem::path& offline_dir,
    const std::string&           session_id) const
{
    std::vector<S3Uploader::UploadCandidate> out;
    for (const auto& name : cfg_.s3.include_files) {
        if (name == "upload_manifest.json") continue;
        const auto p = offline_dir / name;
        if (std::filesystem::exists(p) && std::filesystem::is_regular_file(p)) {
            out.push_back({p, object_key(session_id, "offline/" + p.filename().string())});
        }
    }

    const auto raw_dir = session_dir / "raw";
    if (std::filesystem::exists(raw_dir) && std::filesystem::is_directory(raw_dir)) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(raw_dir)) {
            if (!entry.is_regular_file()) continue;
            const auto rel = std::filesystem::relative(entry.path(), raw_dir).generic_string();
            out.push_back({entry.path(), object_key(session_id, "raw/" + rel)});
        }
    }
    return out;
}

std::string S3Uploader::object_key(const std::string& session_id,
                                   const std::string& relative_path) const {
    std::string prefix = cfg_.s3.prefix;
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    return prefix + "/" + session_id + "/" + relative_path;
}

ExitCode S3Uploader::upload_session(const std::filesystem::path& session_dir,
                                     const std::filesystem::path& offline_dir,
                                     bool dry_run)
{
    if (!cfg_.s3.enabled) {
        std::cerr << "[ego-offline] S3 upload skipped (s3.enabled=false)\n";
        return ExitCode::success;
    }

    UploadReport report;
    report.generated_at_utc = utc_now_iso8601();
    report.bucket = cfg_.s3.bucket;
    report.session_id = read_session_id(session_dir, offline_dir);

    const std::string prefix_trim = [&] {
        std::string p = cfg_.s3.prefix;
        while (!p.empty() && p.back() == '/') p.pop_back();
        return p;
    }();
    report.prefix = prefix_trim + "/" + report.session_id;

    const std::string pipeline_status = read_pipeline_status(offline_dir);
    std::string blocked_reason;
    if (!gate_allowed(offline_dir, pipeline_status, blocked_reason)) {
        report.upload_status = "blocked";
        report.blocked_reason = blocked_reason;
        report.write(offline_dir / "upload_report.json");
        std::cerr << "[ego-offline] S3 upload blocked: " << blocked_reason << '\n';
        return ExitCode::upload_blocked;
    }

    const auto files = collect_files(session_dir, offline_dir, report.session_id);
    if (files.empty()) {
        report.upload_status = "failed";
        report.blocked_reason = "no files to upload";
        report.write(offline_dir / "upload_report.json");
        return ExitCode::upload_failed;
    }

    if (dry_run) {
        for (const auto& f : files) {
            UploadFileResult fr;
            fr.local_path = f.path.string();
            fr.object_key = f.object_key;
            fr.status = "dry_run";
            report.files.push_back(std::move(fr));
        }
        report.upload_status = "skipped";
        report.write(offline_dir / "upload_report.json");
        std::cerr << "[ego-offline] S3 dry-run: " << files.size() << " file(s)\n";
        return ExitCode::success;
    }

    bool all_ok = true;
    for (const auto& file : files) {
        UploadFileResult fr;
        fr.local_path = file.path.string();
        const std::string basename = file.path.filename().string();
        fr.object_key = file.object_key;
        fr.size_bytes = std::filesystem::file_size(file.path);
        fr.sha256 = sha256_hex_file(file.path);

        const auto body = read_file_bytes(file.path);
        const std::string ctype = content_type_for(basename);

        S3PutResult last;
        for (int attempt = 0; attempt <= cfg_.s3.max_retries; ++attempt) {
            if (attempt > 0) {
                const int delay = cfg_.s3.retry_backoff_ms * attempt;
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }
            last = s3_put_object(cfg_.s3.endpoint, cfg_.s3.region, cfg_.s3.bucket,
                                 fr.object_key, cfg_.s3.access_key, cfg_.s3.secret_key,
                                 body, ctype, cfg_.s3.path_style);
            if (last.ok) break;
            if (last.http_status == 403 || last.http_status == 404) break;
        }

        fr.etag = last.etag;
        if (last.ok) {
            fr.status = "ok";
        } else {
            fr.status = "failed";
            fr.message = last.error;
            all_ok = false;
        }
        report.files.push_back(std::move(fr));
        std::cerr << "[ego-offline] S3 " << basename << ": " << (last.ok ? "ok" : last.error) << '\n';
    }

    report.upload_status = all_ok ? "ok" : "failed";
    report.write(offline_dir / "upload_manifest.json");
    report.write(offline_dir / "upload_report.json");

    return all_ok ? ExitCode::success : ExitCode::upload_failed;
}

} // namespace ego_offline::upload
