#pragma once
// EX-01..04: Native ASAM MDF4 v4.1 binary writer (ADR-001, Option D)

#include "ego_offline/mdf4/mdf4_writer.hpp"

#include <filesystem>
#include <string>

namespace ego_offline::mdf4 {

// EX-01..02: Write MDF4 v4.1 binary file.
// Writes to a temp file first, then renames (PI-04 idempotency).
bool write_mdf4(
    const std::filesystem::path&     out_path,
    const std::vector<ChannelGroup>& groups,
    const load::SessionBundle&       meta,
    ns_t                             t0_ns);

// EX-04: Re-open the written file, verify magic bytes and sample counts.
bool validate_mdf4(
    const std::filesystem::path&     mdf_path,
    const std::vector<ChannelGroup>& groups);

// EX-04: Write SHA256 hex digest of mdf_path to mdf_path + ".sha256".
bool write_sha256_sidecar(const std::filesystem::path& mdf_path);

} // namespace ego_offline::mdf4
