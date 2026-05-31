# ego-contract v1 (vendored)

Canonical spec: `Huawei/ego-contract-main` (README, `docs/FRAME_CONTRACT.md`).

This directory is copied into `rpi5` for self-contained deploy on Raspberry Pi 5.

| File | Source |
|------|--------|
| `ego_protocol_packets.hpp` | `ego-contract-main/headers/include/` |
| `ego_contract/crc32.hpp` | `Huawei/shared/ego_contract/` |

Refresh after upstream contract changes:

```bash
cp ../../ego-contract-main/headers/include/ego_protocol_packets.hpp .
cp ../../shared/ego_contract/crc32.hpp ego_contract/
```

Transport on board: **TCP** — Pi connects to SC589 Control `:5000`, Data `:5001`.
