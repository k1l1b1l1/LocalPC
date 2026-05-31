#include "ego_runtime/session_integrity.hpp"

#include <filesystem>
#include <fstream>

#include "ego_runtime/contract_frame_io.hpp"

namespace ego_runtime {
namespace {

bool HasTmpFiles(const std::filesystem::path& dir) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".tmp") {
            return true;
        }
    }
    return false;
}

}  // namespace

IntegrityReport CheckSessionIntegrity(const std::string& session_dir, const std::uint32_t max_payload_bytes) {
    IntegrityReport report{};
    const std::filesystem::path root(session_dir);
    if (!std::filesystem::exists(root)) {
        report.detail = "session directory missing";
        return report;
    }
    if (HasTmpFiles(root)) {
        report.result = IntegrityResult::kWarning;
        report.detail = "leftover .tmp files";
        return report;
    }
    const auto manifest = root / "ego_manifest.json";
    const auto index = root / "ego.index";
    if (!std::filesystem::exists(manifest) || !std::filesystem::exists(index)) {
        report.detail = "manifest or index missing";
        return report;
    }

    std::uint64_t indexed_packets = 0U;
    {
        std::ifstream in(index);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) {
                ++indexed_packets;
            }
        }
    }

    std::uint64_t file_packets = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        const std::string name = entry.path().filename().string();
        if (entry.path().extension() != ".bin" || name.rfind("ego_", 0) != 0) {
            continue;
        }
        std::ifstream in(entry.path(), std::ios::binary);
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::size_t count = CountContractFramesInBytes(bytes, max_payload_bytes);
        if (count == 0U && !bytes.empty()) {
            report.detail = "invalid contract frame stream in chunk";
            return report;
        }
        file_packets += count;
    }

    if (indexed_packets != file_packets) {
        report.result = IntegrityResult::kWarning;
        report.detail = "index count mismatch with chunk frames";
        return report;
    }

    report.result = IntegrityResult::kOk;
    report.detail = "ok";
    return report;
}

}  // namespace ego_runtime
