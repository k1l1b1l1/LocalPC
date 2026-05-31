#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ego_contract {

inline std::uint32_t Crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<std::uint32_t>(data[i]);
        for (int j = 0; j < 8; ++j) {
            const std::uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

inline std::uint32_t Crc32(const std::vector<std::uint8_t>& data) {
    return Crc32(data.data(), data.size());
}

}  // namespace ego_contract
