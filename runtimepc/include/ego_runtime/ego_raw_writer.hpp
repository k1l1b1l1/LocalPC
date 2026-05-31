#pragma once

#include <vector>

#include "ego_runtime/chunk_writer.hpp"

namespace ego_runtime {

class EgoRawWriter {
public:
    explicit EgoRawWriter(ChunkWriter& writer) : writer_(writer) {}

    bool Write(const std::vector<std::uint8_t>& frame) { return writer_.WriteContractFrame(frame); }

    void Flush(bool fsync_now) { writer_.Flush(fsync_now); }

private:
    ChunkWriter& writer_;
};

}  // namespace ego_runtime
