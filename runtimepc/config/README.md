# ego-runtime configuration

Per **ego-contract-main**: Pi is **TCP client** → SC589 **Control :5000**, **Data :5001**.

| Path | Role |
|------|------|
| `board.yaml` | Production RPi5 → `/etc/ego-runtime/config.yaml` |
| `dev/` | Local dev only — pass `--config config/dev/...` |

Contract headers: `rpi5/common/ego_v1/` (run `scripts/sync_ego_contract.sh` after upstream changes).
