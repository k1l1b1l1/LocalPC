#!/usr/bin/env bash
# Refresh vendored ego-contract headers from the main Huawei repo.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${ROOT}/../ego-contract-main/headers/include/ego_protocol_packets.hpp"
CRC="${ROOT}/../shared/ego_contract/crc32.hpp"
DST="${ROOT}/common/ego_v1"
mkdir -p "${DST}/ego_contract"
cp "${SRC}" "${DST}/"
cp "${CRC}" "${DST}/ego_contract/"
echo "Synced ego_v1 from ego-contract-main + shared/ego_contract"
