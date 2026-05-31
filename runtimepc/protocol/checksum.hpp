#pragma once

#include <cstdint>
#include <vector>

namespace ego::protocol {

inline std::uint32_t Crc32(const std::vector<std::uint8_t>& data) {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::uint8_t byte : data) {
        crc ^= byte;
        for (int i = 0; i < 8; ++i) {
            const bool bit = (crc & 1U) != 0U;
            crc >>= 1U;
            if (bit) {
                crc ^= 0xEDB88320U;
            }
        }
    }
    return ~crc;
}

}  // namespace ego::protocol
