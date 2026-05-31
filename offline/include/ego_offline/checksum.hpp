#pragma once
// CRC32 (ISO 3309 / ITU-T V.42) -- matches prod/common/checksum.hpp

#include <cstdint>
#include <cstddef>

namespace ego_offline {

namespace detail {

inline const uint32_t* crc32_table() {
    static uint32_t tbl[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256u; ++i) {
            uint32_t v = i;
            for (int j = 0; j < 8; ++j)
                v = (v & 1u) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            tbl[i] = v;
        }
        init = true;
    }
    return tbl;
}

} // namespace detail

inline uint32_t crc32(const uint8_t* data, size_t len, uint32_t init_val = 0xFFFFFFFFu) {
    const uint32_t* tbl = detail::crc32_table();
    uint32_t val = init_val;
    for (size_t i = 0; i < len; ++i)
        val = tbl[(val ^ data[i]) & 0xFFu] ^ (val >> 8);
    return val ^ 0xFFFFFFFFu;
}

inline uint32_t crc32(const void* data, size_t len) {
    return crc32(static_cast<const uint8_t*>(data), len);
}

} // namespace ego_offline
