# ego-contract v1.3 — baseline для LocalPC

**Эталон:** [SoulCraftedWorld/ego-contract](https://github.com/SoulCraftedWorld/ego-contract) v1.3  
**Локальная копия:** `ego-contract-main/`  
**Дата фиксации:** 2026-06-07

## Версии

| Компонент | Значение |
|-----------|----------|
| Proto package | `ego.v1` |
| Data header | `EgoFrameHeader` 72 байта, magic `EGO1` |
| Control header | `EgoControlMessageHeader` 32 байта, magic `EGOC` |
| Protocol version | 2 |
| Control TCP | `:5000` |
| Data TCP | `:5001` |
| Local IPC | `:19002` |

## Control framing

Использовать **headers** (`EgoControlMessageHeader` + protobuf payload + CRC32), как в `micro/ego_control_framing.py`.

Не использовать legacy framing `uint32_le size` + protobuf из старых черновиков `CONTROL_API.md`.

## Матрица «контракт ↔ код»

| Модуль | Control `:5000` | Data `:5001` | Служебные фреймы offline |
|--------|-----------------|--------------|--------------------------|
| `runtimepc` | `contract_control_client` | `contract_tcp_client` | pass-through в ego.bin |
| `offline` | — | scan ego.bin | `contract_demuxer` 1/2/900/103/200/201 |
| `micro` | `ego_device_server` server | Data server | симулятор SC589 |
| `prod` | ❌ (фаза 5) | Data server | firmware |

## Синхронизация headers

```bash
./scripts/sync_ego_contract.sh
```

Копирует `ego_protocol_packets.hpp` и `crc32.hpp` в `common/ego_v1/`.

## Protobuf C++ (runtimepc)

Сгенерированные файлы: `ego-contract-main/generated/cpp/`  
Сборка: `find_package(Protobuf)` в `runtimepc/CMakeLists.txt`  
На Pi: `sudo apt install libprotobuf-dev`

Перегенерация:

```bash
cd ego-contract-main && ./scripts/gen_cpp.sh
```

## Жизненный цикл сессии (Pi)

1. `./run.sh run` — демон (IPC), без записи
2. `./run.sh start` — HELLO + START_SESSION → Data → запись
3. `./run.sh stop` — STOP_SESSION → SessionEnded → finalize → offline

## Стенд (micro)

- `auto_start_on_data_connect=False` (сессия только после Control START)
- Pi инициирует `./run.sh start`, не ПК
