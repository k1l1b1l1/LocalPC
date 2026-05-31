#include "ego_offline/upload/s3_client.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <process.h>
#endif

namespace ego_offline::upload {

namespace {

#ifdef _WIN32

std::vector<std::uint8_t> sha256_raw(const std::uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
    DWORD obj_len = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb, 0);
    std::vector<std::uint8_t> obj(obj_len);
    std::array<std::uint8_t, 32> digest{};
    if (BCryptCreateHash(alg, &hash, obj.data(), obj_len, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash failed");
    }
    BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0);
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return {digest.begin(), digest.end()};
}

std::vector<std::uint8_t> md5_raw(const std::uint8_t* data, size_t len) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0)
        throw std::runtime_error("BCryptOpenAlgorithmProvider MD5 failed");
    DWORD obj_len = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb, 0);
    std::vector<std::uint8_t> obj(obj_len);
    std::array<std::uint8_t, 16> digest{};
    if (BCryptCreateHash(alg, &hash, obj.data(), obj_len, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        throw std::runtime_error("BCryptCreateHash MD5 failed");
    }
    BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(len), 0);
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return {digest.begin(), digest.end()};
}

std::string base64_encode(const std::vector<std::uint8_t>& data) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16)
            | ((i + 1 < data.size() ? data[i + 1] : 0) << 8)
            | (i + 2 < data.size() ? data[i + 2] : 0);
        out.push_back(kAlphabet[(n >> 18) & 0x3F]);
        out.push_back(kAlphabet[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < data.size() ? kAlphabet[(n >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < data.size() ? kAlphabet[n & 0x3F] : '=');
    }
    return out;
}

std::string md5_base64(const std::vector<std::uint8_t>& data) {
    return base64_encode(md5_raw(data.data(), data.size()));
}

std::vector<std::uint8_t> hmac_sha256(const std::vector<std::uint8_t>& key,
                                      const std::string& msg) {
    std::vector<std::uint8_t> k = key;
    if (k.size() > 64) k = sha256_raw(k.data(), k.size());
    if (k.size() < 64) k.resize(64, 0);

    std::vector<std::uint8_t> ipad(64), opad(64);
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }
    std::vector<std::uint8_t> inner;
    inner.reserve(64 + msg.size());
    inner.insert(inner.end(), ipad.begin(), ipad.end());
    inner.insert(inner.end(), msg.begin(), msg.end());
    auto inner_hash = sha256_raw(inner.data(), inner.size());

    std::vector<std::uint8_t> outer;
    outer.reserve(64 + inner_hash.size());
    outer.insert(outer.end(), opad.begin(), opad.end());
    outer.insert(outer.end(), inner_hash.begin(), inner_hash.end());
    return sha256_raw(outer.data(), outer.size());
}

std::string to_hex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (auto b : bytes) oss << std::setw(2) << static_cast<int>(b);
    return oss.str();
}

std::string amz_date_now() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%04u%02u%02u%02u%02u%02uZ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string date_stamp_from_amz(const std::string& amz_date) {
    return amz_date.substr(0, 8);
}

std::vector<std::uint8_t> signing_key(const std::string& secret,
                                      const std::string& date_stamp,
                                      const std::string& region) {
    const std::string k_secret_str = "AWS4" + secret;
    std::vector<std::uint8_t> k_secret(k_secret_str.begin(), k_secret_str.end());
    auto k_date    = hmac_sha256(k_secret, date_stamp);
    auto k_region  = hmac_sha256(k_date, region);
    auto k_service = hmac_sha256(k_region, "s3");
    return hmac_sha256(k_service, "aws4_request");
}

struct UrlParts {
    std::string host;
};

UrlParts parse_endpoint(const std::string& endpoint) {
    UrlParts p;
    std::string s = endpoint;
    if (s.rfind("https://", 0) == 0) s = s.substr(8);
    else if (s.rfind("http://", 0) == 0) s = s.substr(7);
    const auto slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    p.host = std::move(s);
    return p;
}

std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string parse_etag_from_headers(const std::string& headers) {
    const std::string needle = "ETag:";
    auto pos = headers.find(needle);
    if (pos == std::string::npos) {
        pos = headers.find("etag:");
        if (pos == std::string::npos) return {};
        pos += 5;
    } else {
        pos += needle.size();
    }
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) ++pos;
    auto end = headers.find('\n', pos);
    std::string etag = headers.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
    while (!etag.empty() && (etag.back() == '\r' || etag.back() == ' ')) etag.pop_back();
    return etag;
}

std::string escape_curl_config_value(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char c : value) {
        if (c == '\\' || c == '"') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

#endif

std::string shell_quote(const std::string& arg) {
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out.push_back('"');
    return out;
}

#ifndef _WIN32
std::string sha256_hex_file_openssl(const std::filesystem::path& path) {
    std::ostringstream cmd;
    cmd << "openssl dgst -sha256 -hex " << shell_quote(path.string()) << " 2>/dev/null";
    FILE* pipe = popen(cmd.str().c_str(), "r");
    if (!pipe) return {};
    std::string output;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe)) output += buf;
    pclose(pipe);
    const auto eq = output.rfind('=');
    if (eq == std::string::npos) return {};
    std::string hex = output.substr(eq + 1);
    while (!hex.empty() && (hex.front() == ' ' || hex.front() == '\t')) hex.erase(hex.begin());
    while (!hex.empty() && (hex.back() == '\r' || hex.back() == '\n' || hex.back() == ' ')) hex.pop_back();
    return hex;
}
#endif

} // namespace

std::string sha256_hex(const std::vector<std::uint8_t>& data) {
#ifdef _WIN32
    return to_hex(sha256_raw(data.data(), data.size()));
#else
    (void)data;
    return {};
#endif
}

std::string sha256_hex_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
#ifdef _WIN32
    std::vector<std::uint8_t> buf(8192);
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    DWORD obj_len = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&obj_len), sizeof(obj_len), &cb, 0);
    std::vector<std::uint8_t> obj(obj_len);
    std::array<std::uint8_t, 32> digest{};
    if (BCryptCreateHash(alg, &hash, obj.data(), obj_len, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return {};
    }
    while (f) {
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        auto n = f.gcount();
        if (n > 0) BCryptHashData(hash, buf.data(), static_cast<ULONG>(n), 0);
    }
    BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return to_hex({digest.begin(), digest.end()});
#else
    return sha256_hex_file_openssl(path);
#endif
}

std::string url_encode_path(const std::string& path) {
    std::ostringstream oss;
    for (size_t i = 0; i < path.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(path[i]);
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            oss << static_cast<char>(c);
        } else {
            oss << '%' << std::uppercase << std::hex << std::setfill('0') << std::setw(2) << int(c);
        }
    }
    return oss.str();
}

S3PutResult s3_put_object(
    const std::string& endpoint,
    const std::string& region,
    const std::string& bucket,
    const std::string& object_key,
    const std::string& access_key,
    const std::string& secret_key,
    const std::vector<std::uint8_t>& body,
    const std::string& content_type,
    bool path_style)
{
    S3PutResult result;
    if (access_key.empty() || secret_key.empty()) {
        result.error = "missing S3 credentials";
        return result;
    }

    const std::string canonical_uri = path_style
        ? "/" + bucket + "/" + object_key
        : "/" + object_key;

    std::string put_url = endpoint;
    while (!put_url.empty() && put_url.back() == '/') put_url.pop_back();
    put_url += canonical_uri;

    const auto tmp_dir = std::filesystem::temp_directory_path();
    const auto tag = std::to_string(
#ifdef _WIN32
        GetTickCount64()
#else
        static_cast<unsigned long long>(std::chrono::steady_clock::now().time_since_epoch().count())
#endif
        );
    const auto body_path = tmp_dir / ("ego_s3_" + tag + ".bin");

    {
        std::ofstream body_out(body_path, std::ios::binary);
        if (!body_out) {
            result.error = "failed to write temp upload body";
            return result;
        }
        body_out.write(reinterpret_cast<const char*>(body.data()),
                       static_cast<std::streamsize>(body.size()));
    }

    const std::string sigv4 = "aws:amz:" + region + ":s3";
    const std::string userpass = access_key + ":" + secret_key;
#ifdef _WIN32
    const char* curl_bin = "curl.exe";
    const char* null_dev = "NUL";
#else
    const char* curl_bin = "curl";
    const char* null_dev = "/dev/null";
#endif

    std::ostringstream cmd;
    cmd << curl_bin << " -sS"
        << " -w \"HTTP_CODE:%{http_code}\""
        << " -o " << null_dev
        << " -X PUT"
        << " -T " << shell_quote(body_path.string())
        << " --aws-sigv4 " << shell_quote(sigv4)
        << " -u " << shell_quote(userpass);
    if (!content_type.empty())
        cmd << " -H " << shell_quote("Content-Type: " + content_type);
    cmd << " " << shell_quote(put_url);

    if (std::getenv("EGO_S3_DEBUG"))
        std::cerr << "[s3-debug] put " << canonical_uri << " cmd=" << cmd.str() << '\n';

    std::string curl_output;
#ifdef _WIN32
    FILE* pipe = _popen(cmd.str().c_str(), "r");
#else
    FILE* pipe = popen(cmd.str().c_str(), "r");
#endif
    if (pipe) {
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe)) curl_output += buf;
#ifdef _WIN32
        _pclose(pipe);
#else
        pclose(pipe);
#endif
    } else {
        std::error_code ec;
        std::filesystem::remove(body_path, ec);
        result.error = std::string("failed to run ") + curl_bin;
        return result;
    }

    std::error_code ec;
    std::filesystem::remove(body_path, ec);

    const auto code_pos = curl_output.rfind("HTTP_CODE:");
    if (code_pos != std::string::npos) {
        result.http_status = std::atoi(curl_output.c_str() + code_pos + 10);
    } else {
        result.http_status = 0;
        result.error = "curl produced no HTTP status";
        return result;
    }

    result.ok = (result.http_status >= 200 && result.http_status < 300);
    if (!result.ok)
        result.error = "HTTP " + std::to_string(result.http_status);
    return result;
}

} // namespace ego_offline::upload
