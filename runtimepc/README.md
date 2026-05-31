# ego-runtime (Raspberry Pi 5)

Бортовой runtime-модуль: приём UDP-потока пакетов от `prod`, валидация по контракту, запись сырого лога `ego_*.bin` и метаданных сессии.

## Сборка

```bash
cd runtimepc
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Кросс-сборка для RPi5:

```bash
cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=aarch64-linux-gnu.cmake
cmake --build build-arm
```

## CLI

| Команда | Описание |
|---------|----------|
| `ego-runtime run` | Долгоживущий демон (systemd): UDP + запись до SIGTERM |
| `ego-runtime start` | Старт записи в работающем демоне (IPC) |
| `ego-runtime stop` | Остановка записи в демоне (IPC) |
| `ego-runtime status` | Состояние сессии (IPC) |
| `ego-runtime diagnostics` | Сводка метрик (IPC) |
| `ego-runtime --version` | Semver + git hash `ego_protocol` |

**Коды выхода:** `0` ok, `1` session_busy, `2` not_recording, `3` storage, `4` config, `5` internal.

Dev ingest из файла (без IPC): `ego-runtime start --input path.bin`.

IPC: Unix socket `{data_root}/ego-runtime.sock` (Linux) или TCP `127.0.0.1:19002` (Windows).

## Конфигурация

**Production:** `config/board.yaml` — TCP client к SC589 (`ego_host`, `control_port: 5000`, `data_port: 5001`).

**Dev:** `config/dev/stand.pc.yaml` — localhost TCP.

Контракт: `rpi5/common/ego_v1/` (ego-contract-main). Без `/etc/ego-runtime/config.yaml` и без `--config` — exit **4**.

Env: `EGO_RUNTIME_DATA_ROOT`, `EGO_RUNTIME_EGO_HOST`, `EGO_RUNTIME_VEHICLE_ID`.

**Offline hook** (production `board.yaml` → `offline:`): после finalize сессии автоматически запускается `ego-offline process` в фоне. Env: `EGO_OFFLINE_BINARY`, `EGO_OFFLINE_CONFIG`, `EGO_OFFLINE_S3_CONFIG`, `EGO_OFFLINE_ENABLED`.

## Связь с SC589

Спецификация: `ego-contract-main` — Data TCP фреймы `EgoFrameHeader` + payload → `ego_*.bin` байт-в-байт.
