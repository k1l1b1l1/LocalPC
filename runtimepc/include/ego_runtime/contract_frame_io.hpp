#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ego_runtime {

bool ValidateContractFrame(const std::vector<std::uint8_t>& frame_bytes, std::uint32_t max_payload_bytes,
                             std::string* error = nullptr);

bool ForEachContractFrame(const std::vector<std::uint8_t>& bytes, std::uint32_t max_payload_bytes,
                          const std::function<bool(const std::vector<std::uint8_t>&)>& handler,
                          std::string* error = nullptr);

bool ReadContractFramesFromFile(const std::string& path, std::uint32_t max_payload_bytes,
                                const std::function<bool(const std::vector<std::uint8_t>&)>& handler,
                                std::string* error = nullptr);

std::size_t CountContractFramesInBytes(const std::vector<std::uint8_t>& bytes, std::uint32_t max_payload_bytes);

}  // namespace ego_runtime
