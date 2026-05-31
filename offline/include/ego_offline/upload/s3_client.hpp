#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ego_offline::upload {

struct S3PutResult {
    bool        ok = false;
    int         http_status = 0;
    std::string etag;
    std::string error;
};

// AWS Signature V4 PUT (Timeweb Cloud: region ru-1, path-style)
S3PutResult s3_put_object(
    const std::string& endpoint,   // https://s3.twcstorage.ru
    const std::string& region,     // ru-1
    const std::string& bucket,
    const std::string& object_key, // prefix/session/file (no leading slash)
    const std::string& access_key,
    const std::string& secret_key,
    const std::vector<std::uint8_t>& body,
    const std::string& content_type,
    bool path_style = true);

std::string sha256_hex(const std::vector<std::uint8_t>& data);
std::string sha256_hex_file(const std::filesystem::path& path);
std::string url_encode_path(const std::string& path);

} // namespace ego_offline::upload
