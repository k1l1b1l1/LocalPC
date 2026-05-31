#include "ego_runtime/contract_frame_io.hpp"

#include <cstring>
#include <fstream>

#include "ego_contract/crc32.hpp"
#include "ego_protocol_packets.hpp"

namespace ego_runtime {
namespace {

using ego::protocol::v1::EgoFrameHeader;
using ego::protocol::v1::EGO_FRAME_MAGIC;

bool ParseFrameAt(const std::uint8_t* data, std::size_t size, std::size_t& offset, std::uint32_t max_payload,
                  std::vector<std::uint8_t>& frame_out, std::string* error) {
    if (offset + sizeof(EgoFrameHeader) > size) {
        if (offset == size) {
            return false;
        }
        if (error) {
            *error = "truncated frame header";
        }
        return false;
    }
    EgoFrameHeader header{};
    std::memcpy(&header, data + offset, sizeof(header));
    if (header.magic != EGO_FRAME_MAGIC || header.header_size != sizeof(EgoFrameHeader)) {
        if (error) {
            *error = "bad frame magic or header_size";
        }
        return false;
    }
    if (header.payload_size > max_payload) {
        if (error) {
            *error = "payload too large";
        }
        return false;
    }
    const std::size_t frame_size = sizeof(header) + static_cast<std::size_t>(header.payload_size);
    if (offset + frame_size > size) {
        if (error) {
            *error = "truncated frame payload";
        }
        return false;
    }
    frame_out.assign(data + offset, data + offset + frame_size);
    offset += frame_size;
    return true;
}

}  // namespace

bool ValidateContractFrame(const std::vector<std::uint8_t>& frame_bytes, const std::uint32_t max_payload_bytes,
                           std::string* error) {
    if (frame_bytes.size() < sizeof(EgoFrameHeader)) {
        if (error) {
            *error = "frame too short";
        }
        return false;
    }
    EgoFrameHeader header{};
    std::memcpy(&header, frame_bytes.data(), sizeof(header));
    if (header.magic != EGO_FRAME_MAGIC || header.header_size != sizeof(EgoFrameHeader)) {
        if (error) {
            *error = "bad header";
        }
        return false;
    }
    if (header.payload_size > max_payload_bytes) {
        if (error) {
            *error = "payload too large";
        }
        return false;
    }
    if (frame_bytes.size() != sizeof(header) + header.payload_size) {
        if (error) {
            *error = "frame size mismatch";
        }
        return false;
    }
    std::vector<std::uint8_t> payload;
    if (header.payload_size > 0U) {
        payload.assign(frame_bytes.begin() + static_cast<std::ptrdiff_t>(sizeof(header)), frame_bytes.end());
        if (ego_contract::Crc32(payload) != header.payload_crc32) {
            if (error) {
                *error = "payload crc mismatch";
            }
            return false;
        }
    }
    EgoFrameHeader zero_hdr = header;
    zero_hdr.header_crc32 = 0U;
    if (ego_contract::Crc32(reinterpret_cast<const std::uint8_t*>(&zero_hdr), sizeof(zero_hdr)) !=
        header.header_crc32) {
        if (error) {
            *error = "header crc mismatch";
        }
        return false;
    }
    return true;
}

bool ForEachContractFrame(const std::vector<std::uint8_t>& bytes, const std::uint32_t max_payload_bytes,
                          const std::function<bool(const std::vector<std::uint8_t>&)>& handler,
                          std::string* error) {
    std::size_t offset = 0U;
    while (offset < bytes.size()) {
        std::vector<std::uint8_t> frame;
        if (!ParseFrameAt(bytes.data(), bytes.size(), offset, max_payload_bytes, frame, error)) {
            return offset == bytes.size();
        }
        if (!ValidateContractFrame(frame, max_payload_bytes, error)) {
            return false;
        }
        if (!handler(frame)) {
            return false;
        }
    }
    return true;
}

bool ReadContractFramesFromFile(const std::string& path, const std::uint32_t max_payload_bytes,
                                const std::function<bool(const std::vector<std::uint8_t>&)>& handler,
                                std::string* error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        if (error) {
            *error = "cannot open input file";
        }
        return false;
    }
    const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return ForEachContractFrame(bytes, max_payload_bytes, handler, error);
}

std::size_t CountContractFramesInBytes(const std::vector<std::uint8_t>& bytes,
                                       const std::uint32_t max_payload_bytes) {
    std::size_t count = 0U;
    ForEachContractFrame(bytes, max_payload_bytes, [&](const std::vector<std::uint8_t>&) {
        ++count;
        return true;
    });
    return count;
}

}  // namespace ego_runtime
