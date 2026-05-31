# Архитектурный аудит `runtimepc` (ego-runtime)

**Дата:** 28.05.2026  
**Объект:** `C:\Users\User\Desktop\Huawei\runtimepc`  
**Эталон:** `TZ_ego_runtime_rpi5.md`, таблица ТЗ «Бортовой runtime-модуль мини-ПК»

---

## Вердикт

| Критерий | Оценка |
|----------|--------|
| Соответствие ТЗ (MVP по коду) | **~65–75%** — каркас модулей есть, логика частично реализована |
| Готовность к **реальной** эксплуатации на RPi5 | **Нет** — есть блокеры уровня P0 |

Модуль пригоден как **прототип / dev-стенд** (file ingest, локальные тесты). Для бортовой эксплуатации с `prod` по UDP и `systemd` требуется доработка.

---

## Соответствие по модулям ТЗ

### 1. Контракт данных

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| DC-01 | `header` | ✅ | `protocol/serialization.hpp`, `protocol/types.hpp` |
| DC-02 | `types` | ✅ | `protocol/types.hpp`, `IsKnownPacketType` |
| DC-03 | `validation` | ✅ (строже `prod`) | `protocol/validation.hpp` |
| DC-04 | `protocol_version_check` | ✅ | `ValidatePacketVerbose` + счётчик `bad_packets` |
| DC-05 | `range_validation` | ✅ | `max_payload_bytes` в конфиге |

**Минусы:**

- Тесты из `prod/tests/test_main.cpp` не портированы (требование DC-03).
- Эталонный hex-тест (`sc589_uart_full_session_stream.hex`) в CI не гарантирован — файл часто отсутствует.
- Каталог `protocol/` — копия из `prod/common/`; нет автоматической синхронизации (риск расхождения CRC/seq).

---

### 2. Управление сессией

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| SM-01 | `session_manager` | ✅ | `src/session_manager.cpp` |
| SM-02 | `recording_control` | ⚠️ | API есть, межпроцессного управления нет |
| SM-03 | `session_metadata.json` | ✅ | `WriteSessionMetadata()` |
| SM-04 | `scenario_metadata` | ✅ | `WriteScenarioMetadata()` |
| — | `E_SESSION_BUSY` | ✅ | `RuntimeErrorCode::kSessionBusy` |

**Минусы:**

- Состояние `kStopping` объявлено, но в пайплайне записи почти не используется.
- **`ego-runtime start`** без `--input` печатает «recording started» и **сразу завершает процесс** — в деструкторе вызывается `StopDaemon()`, сессия обрывается.
- **`ego-runtime stop` / `status` / `diagnostics`** в отдельном процессе не видят активную сессию — работает только монолитный режим `run` в одном процессе.

---

### 3. Приём данных от ADSP-SC589 / prod

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| RX-01 | `network_receiver` | ⚠️ | `src/network_receiver.cpp` — UDP + поток, **без epoll** |
| RX-02 | `sequence_counter_check` | ✅ | `protocol/transport_guard.hpp` |
| RX-03 | `packet_checksum_check` | ✅ | validation + unwrap `frame_crc32` |
| RX-04 | `packet_buffer` | ✅ | `include/ego_runtime/packet_buffer.hpp` |

Дополнительно: SC589 unwrap — `include/ego_runtime/sc589_unwrap.hpp`.

**Минусы:**

- ТЗ требует non-blocking + **epoll**; реализован poll-loop с `sleep` (1 ms Win / 500 µs Linux).
- Нет настройки `SO_RCVBUF`, batch recv, метрик отбросов на уровне ОС.
- `packets_received` инкрементируется в `OnDatagram` **до** валидации — метрика включает отброшенные датаграммы.
- Whitelist IP: пустой список `allowed_sources` = принять **всех** (небезопасно для LAN).

---

### 4. Первичная запись сырого лога

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| WR-01 | `ego_raw_writer` | ⚠️ | Только alias в `include/ego_runtime/ego_raw_writer.hpp` |
| WR-02 | `chunk_writer` | ✅ | `src/chunk_writer.cpp`, `ego_manifest.json` |
| WR-03 | `ego.index` | ✅ | append CSV в `ChunkWriter::AppendIndex` |
| WR-04 | `safe_flush_policy` | ⚠️ | `Flush()` — частично |

Writer-поток: `RuntimeService::WriterLoop()` в `src/runtime_service.cpp`.

**Минусы (критичные):**

- При `stop` / `emergency_stop` состояние → `kStopping`, а `WriterLoop` обрабатывает только `kRecording` — **очередь `PacketBuffer` не сливается**, пакеты теряются.
- **`ego-runtime run --input file`** не вызывает `ProcessFileInput` — сразу `StopRecording("file_eof")`.
- Ротация по времени (`chunk_max_sec`) использует `last_flush_time_`, а не время открытия чанка — интервал 60 мин может не сработать при частом flush.
- `fsync` только для `.bin` на Linux; `ego.index` без `fsync`. На Windows `fsync` отсутствует.

---

### 5. Контроль хранилища

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| ST-01 | `storage_monitor` | ✅ | `src/storage_monitor.cpp`, ~5 с |
| ST-02 | `storage_alerts` | ✅ | warning/critical по GB и % |
| ST-03 | `emergency_stop` | ✅ | `RuntimeService::EmergencyStop("storage_critical")` |

**Минусы:**

- `warn_free_percent` / `critical_free_percent` есть в `RuntimeConfig`, но не в `config/config.yaml.example` и не парсятся из YAML.
- `test_stand_config` не читается из конфига (поле в metadata захардкожено через defaults).
- Нет гистерезиса алертов (повторные callback при длительном warning).

---

### 6. Диагностика входящего потока

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| IN-01 | `packet_loss_counter` | ✅ | `TransportGuard` → `DiagnosticsCollector` |
| IN-02 | `bad_packet_counter` | ✅ | `OnBadPacket` + `reject_by_reason` |
| IN-03 | `time_gap_monitor` | ✅ | `OnUnwrappedPacket`, порог из конфига |
| IN-04 | `adsp_status_monitor` | ⚠️ | Только текстовый payload `kDiagnostics` |

**Минусы:**

- `reject_by_reason` собирается, но **не экспортируется** в `runtime_report.json`.
- Нет структурного разбора diagnostics (audio, CAN, IMU, GPS, queues) — нужен согласованный payload с `prod`.

---

### 7. Диагностика записи

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| RD-01 | `runtime_report.json` | ✅ | `WriteRuntimeReport()`, интервал из конфига |
| RD-02 | `runtime_error_log` | ✅ | `src/error_log.cpp`, ротация 10×50 MB |

**Минусы:**

- `final_runtime_summary.json` **без поля `duration`** (требование FN-02 в ТЗ).

---

### 8. Служебный интерфейс

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| UI-01 | `cli_status` | ⚠️ | `src/main.cpp` — команды есть |
| UI-02 | `diagnostic_panel` | ⚠️ | `diagnostics` / `stats` |

**Минусы:**

- `status`, `stop`, `diagnostics` бесполезны без живого демона (отдельные процессы).
- Коды выхода документированы частично в `main.cpp`, отдельной таблицы в README нет.

---

### 9. Завершение сессии

| Пункт ТЗ | Компонент | Статус | Реализация |
|----------|-----------|--------|------------|
| FN-01 | `finalize_session` | ⚠️ | `FinalizeActiveSession()` — без drain буфера |
| FN-02 | `final_runtime_summary` | ⚠️ | `WriteFinalSummary()` — без `duration` |
| FN-03 | `session_integrity_check` | ✅ | `src/session_integrity.cpp` |

---

## Архитектура процесса (фактическая)

```
┌──────────────────────────────────────────────────────────────┐
│ NetworkRxThread          │  Sc589Unwrap + TransportGuard      │
│  UDP recv (poll+sleep)   │  → OnUnwrappedPacket               │
├─────────────────────────┴──────────────────────────────────┤
│ WriterThread          │ StorageMonitorThread │ ReportThread  │
│  PacketBuffer → ChunkWriter │ statvfs, alerts  │ runtime_report│
│  (ego_*.bin + index)      │ → emergency_stop   │               │
├───────────────────────┴──────────────────────┴───────────────┤
│ Main: SessionManager, CLI (main.cpp)                         │
└──────────────────────────────────────────────────────────────┘
```

Соответствует целевой схеме ТЗ §6 в целом, но RX не на epoll, а CLI не рассчитан на multi-process.

---

## Блокеры эксплуатации (P0)

### P0-1. `systemd` + `ego-runtime run` — мгновенный выход

В `src/main.cpp` режим `run` ждёт строку со **stdin**:

```cpp
std::getline(std::cin, line);
service.StopRecording("stdin_stop");
```

При закрытом stdin (типично для `Type=simple` без TTY) `getline` сразу завершается → сессия падает после старта.  
**Нужно:** бесконечный цикл до `SIGTERM`/`SIGINT` или IPC, не stdin.

Файл: `systemd/ego-runtime.service` — `ExecStart=... ego-runtime run`.

---

### P0-2. CLI `start` / `stop` не для production

| Команда | Проблема |
|---------|----------|
| `start` | Процесс завершается → `StopDaemon()` в деструкторе |
| `stop` | Новый процесс, нет состояния сессии → `kNotRecording` |
| `status` / `diagnostics` | Всегда пустые метрики без живого демона |

**Нужно:** долгоживущий демон + IPC (Unix socket, pid-файл, D-Bus).

---

### P0-3. Потеря пакетов при остановке

`SessionManager::Stop()` переводит в `kStopping`.  
`WriterLoop` проверяет только `kRecording` → writer перестаёт читать буфер до `join`.  
**Нужно:** drain `PacketBuffer` в состоянии `kStopping` или до смены состояния.

---

### P0-4. Нет приёмки MVP

В ТЗ §9.3 чекбоксы AC-01…AC-06 не закрыты. Нет интеграционных тестов IT-01…IT-05 с `prod`, нет 8-ч soak, нет замеров p99 recv→fsync.

---

### P0-5. Целевая платформа

| Требование ТЗ | Факт |
|---------------|------|
| RPi5, aarch64 | Сборка проверена на Windows x64 |
| `aarch64-linux-gnu` toolchain | В репозитории отсутствует |
| `--version` + git hash | `EGO_RUNTIME_GIT_HASH="unknown"` без git |

---

## Прочие замечания (P1–P2)

| Приоритет | Проблема | Рекомендация |
|-----------|----------|--------------|
| P1 | Нет epoll / recvmmsg | `epoll_wait` + увеличить `SO_RCVBUF` |
| P1 | Двойная проверка CRC в `TransportGuard` | Убрать дубликат после `ValidatePacketVerbose` |
| P1 | `EgoRawWriter` не используется | Подключить или удалить мёртвый слой |
| P1 | `service_mu_` не используется | Удалить или защитить shared state |
| P1 | `run --input` не читает файл | Вызвать `ProcessFileInput` до stop |
| P1 | Ротация чанка по времени | Отдельный `chunk_opened_at_` |
| P1 | Index без fsync при power loss | Групповый fsync bin + index |
| P1 | JSON вручную через `ostringstream` | Библиотека JSON или строгие шаблоны |
| P1 | `SessionState::kError` не выставляется | При фатальных ошибках диска/сети |
| P1 | Нет SIGINT/SIGTERM handler | Graceful shutdown |
| P2 | `-W1` в CMake | `-Wall -Wextra`, опционально ASan в CI |
| P2 | `adsp_status` — сырой текст | Согласовать бинарный формат с `prod` |
| P2 | Нет версии формата `ego.index` | Поле version в заголовке индекса |

---

## Что сделано хорошо

- Контракт `protocol/` согласован с `prod/common` (типы, CRC, сериализация).
- `TransportGuard` совпадает по логике `seq` / gap с `prod/arm_core/transport_guard.hpp`.
- Разделение потоков RX / writer / storage / report — по духу ТЗ §6.
- Атомарная запись JSON (`WriteTextAtomic`), chunk + manifest + integrity check.
- SC589 unwrap + unit-тесты: contract, transport, chunk, integrity.
- `systemd` unit с `Restart=on-failure`, `StartLimitBurst=3` — заготовка корректная.

---

## Выходные артефакты (ТЗ §4)

| Файл | Статус |
|------|--------|
| `ego_000.bin`, … | ✅ |
| `ego_manifest.json` | ✅ |
| `ego.index` | ✅ |
| `session_metadata.json` | ✅ |
| `scenario_metadata.json` | ✅ |
| `runtime_report.json` | ✅ (при активной сессии) |
| `final_runtime_summary.json` | ⚠️ без `duration` |
| `logs/runtime_error.log` | ✅ |

---

## Тестирование

| Тип | Статус |
|-----|--------|
| Unit (`tests/test_main.cpp`) | ✅ проходят на Windows |
| Hex-эталон 830 байт / 10 пакетов | ⏭ skip если нет `data/sc589_uart_full_session_stream.hex` |
| Порт тестов из `prod/tests` | ❌ |
| IT-01…IT-05 с `prod` | ❌ |

---

## План доработки до пилота на RPi5

1. **Демон:** цикл до SIGTERM; убрать зависимость от stdin; IPC для `stop` / `status`.
2. **Stop path:** drain `PacketBuffer` в `kStopping`, `fsync`, затем `finalize_session`.
3. **systemd:** проверить на RPi с реальным UDP от `prod`; при необходимости `Type=notify`.
4. **Сборка:** toolchain `aarch64-linux-gnu`, деплой на NVMe, CI с hex-эталоном.
5. **Приёмка:** закрыть AC-01…AC-06 и IT-01…IT-05 из `TZ_ego_runtime_rpi5.md` §9.

---

## Итог для программиста

Функциональный **скелет по таблице ТЗ собран**, базовые unit-тесты проходят. **Эксплуатационная готовность отсутствует** из‑за:

- жизненного цикла процесса (stdin / systemd, однопроцессный CLI);
- потери данных в буфере при `stop`;
- отсутствия интеграции и формальной приёмки с `prod`.

Ориентир: **1–2 итерации** (этап M3 + hardening) до пилота на RPi5.

---

## Ссылки

| Артефакт | Путь |
|----------|------|
| ТЗ модуля | `runtimepc/TZ_ego_runtime_rpi5.md` |
| README | `runtimepc/README.md` |
| Конфиг-пример | `runtimepc/config/config.yaml.example` |
| systemd | `runtimepc/systemd/ego-runtime.service` |
| Источник контракта | `../prod/common/` |

---

## Повторный аудит (после правок программиста)

**Дата:** 28.05.2026  
**Сборка/тесты:** Release на Windows, `ego_runtime_tests` — OK (hex-эталон skip).

### Вердикт повторной проверки

| Критерий | Было | Стало |
|----------|------|-------|
| Соответствие ТЗ (MVP) | ~65–75% | **~85–90%** |
| Готовность к эксплуатации на RPi5 | Нет | **Условно** (пилот после IT с `prod`) |

Критические блокеры P0 из первого аудита **в основном сняты**. До промышленной эксплуатации остаются интеграционная приёмка, периодический `fsync` по ТЗ и toolchain RPi.

---

### Закрытые замечания (исправлено)

| ID | Замечание | Решение |
|----|-----------|---------|
| P0-1 | stdin / мгновенный выход под systemd | `shutdown.cpp`: SIGTERM/SIGINT, `WaitForShutdown()` |
| P0-2 | CLI start/stop в отдельных процессах | `control_ipc.cpp`: Unix socket / TCP 19002, pid-файл |
| P0-3 | Потеря пакетов в буфере при stop | `kStopping` в `WriterLoop`, `DrainPacketBuffer()`, ожидание drain |
| — | `run --input` не читал файл | `ProcessFileInput` в `RunDaemon` |
| — | `packets_received` до валидации | Счётчик после `TransportGuard` |
| — | `final_runtime_summary` без duration | Поле `duration_sec` |
| — | `reject_by_reason` не в отчёте | Добавлено в `runtime_report.json` |
| — | Ротация чанка по времени | `chunk_opened_at_` |
| — | index без fsync | `fsync` index при `fsync_now` (Linux) |
| — | epoll / SO_RCVBUF | `epoll_wait` + drain loop (Linux), `SO_RCVBUF` |
| — | allow all sources по умолчанию | `allow_all_sources: false`, пустой whitelist = deny |
| — | Дубль CRC в TransportGuard | Убран повторный `Crc32` |
| — | `-W1` | `-Wall -Wextra -Wpedantic` (GCC/Clang) |
| — | `SetError`, `EgoRawWriter` | Используются при ошибках диска и в writer/drain |
| — | Конфиг | `allow_all_sources`, `udp_rcvbuf_bytes`, `control.port`, `test_stand_config`, проценты диска |

**Новые файлы:** `control_ipc.cpp/hpp`, `shutdown.cpp/hpp`.

---

### Оставшиеся замечания

#### P1 — до пилота на стенде

| # | Проблема | Детали |
|---|----------|--------|
| 1 | **Периодический fsync (WR-04)** | `Flush(false)` делает только `flush()`, без `fsync`. По ТЗ — fsync каждые N пакетов / T сек. Сейчас `fsync` при ротации, `Close` и `Flush(true)`. |
| 2 | **Нет IT с `prod`** | AC-01…AC-06, IT-01…IT-05 не выполнены. |
| 3 | **Нет `aarch64-linux-gnu.cmake`** | README ссылается на файл, в репозитории отсутствует. |
| 4 | **Нет unit-тестов IPC / drain / shutdown** | Регрессии возможны незаметно. |
| 5 | **Hex-эталон в CI** | `../data/sc589_uart_full_session_stream.hex` — skip. |
| 6 | **Порт prod-тестов** | `prod/tests` не портированы. |
| 7 | **Stop: таймаут буфера 5 с** | При перегрузке возможен выход из ожидания до полного drain; затем `DrainPacketBuffer` — обычно достаточно, но нет метрики «потеряно при stop». |
| 8 | **`ego-runtime run` сразу стартует запись** | После `stop` по IPC нужен `start`; для systemd это ок, но документировать сценарий. |
| 9 | **Ошибка диска → `SetError`** | Запись в буфер может продолжаться до ручного stop. |
| 10 | **IN-04 adsp_status** | По-прежнему текст/binary stub, не структурный разбор. |

#### P2 — hardening

| # | Проблема |
|---|----------|
| 1 | Windows: UDP без epoll (приемлемо для dev) |
| 2 | JSON ручной; нет `Type=notify` для systemd |
| 3 | NFR: 8 ч soak, p99 recv→fsync — не измерялись |
| 4 | `protocol/` — ручная синхронизация с `prod` |

---

### Сводка по модулям ТЗ (актуально)

| Модуль | Статус |
|--------|--------|
| 1. Контракт данных | ✅ |
| 2. Управление сессией | ✅ (IPC) |
| 3. Приём данных | ✅ Linux epoll; ⚠️ Windows poll |
| 4. Запись сырого лога | ⚠️ периодический fsync |
| 5. Контроль хранилища | ✅ |
| 6–7. Диагностика | ✅; ⚠️ IN-04 |
| 8. Служебный интерфейс | ✅ с работающим демоном |
| 9. Завершение сессии | ✅ |

---

### Итог для программиста (повторно)

Правки **существенно повысили зрелость**: демон пригоден для `systemd`, CLI через IPC, корректный stop-path, метрики и конфиг ближе к ТЗ.

**Рекомендация:** можно выходить на **ограниченный пилот** (RPi5 + `prod` UDP 10+ мин) после:

1. Сборки под `aarch64` и проверки на целевом SSD.
2. IT-01…IT-04 на стенде.
3. Исправления периодического `fsync` по WR-04.

Полная эксплуатационная приёмка — после закрытия AC и soak NFR.
