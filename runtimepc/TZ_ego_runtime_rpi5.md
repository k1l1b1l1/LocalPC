# Техническое задание на разработку

## Бортовой runtime-модуль мини-ПК (Raspberry Pi 5)

| | |
|---|---|
| **Код проекта (рабочее имя)** | `ego-runtime` |
| **Версия ТЗ** | 1.0 |
| **Дата** | 28.05.2026 |

---

## 1. Область применения

### 1.1. Что разрабатывается

Сервис на **Raspberry Pi 5**, который:

- принимает готовый бинарный поток пакетов по сети;
- проверяет поток по контракту;
- управляет сессией записи;
- пишет сырой лог и метаданные на диск;
- ведёт диагностику и служебный интерфейс;
- корректно завершает сессию.

### 1.2. Что **не** входит в задачу

| Не разрабатывается в этом ТЗ | Где реализовано / кто отвечает |
|-----------------------------|--------------------------------|
| Захват A2B, CAN, IMU, GPS | Прошивка / модуль на ADSP-SC589 |
| Sensor fusion, траектория, агрегация кадров | `C:\Users\User\Desktop\Huawei\prod` (`sharc0_audio`, `sharc1_motion`, `arm_core`) |
| Формирование payload, `SerializePacket`, отправка UDP | `prod` (`arm_core/pipeline.hpp`, `network_streamer.hpp`) |
| Offline-обработка, S3, Local PC | Отдельные модули |

### 1.3. Граница с модулем `prod`

**`prod` — поставщик данных (upstream).**  
Runtime на RPi — **потребитель (downstream)**.

```
[prod на SC589 / стенде]  ──UDP──►  [ego-runtime на RPi5]  ──►  NVMe/SSD
     SerializePacket              приём, валидация, ego.bin
     NetworkStreamer :19001
```

Разработчик runtime **не меняет** контракт без согласования с владельцем `prod`. При расхождении — источник истины: файлы в `prod/common/` (см. §3).

---

## 2. Целевая среда

| Параметр | Требование |
|----------|------------|
| Платформа | Raspberry Pi 5, 64-bit OS |
| Язык | **C++17** (совместимость контракта с `prod`) |
| Сборка | CMake, target `aarch64-linux-gnu` |
| Деплой | `systemd`-unit `ego-runtime.service` |
| Диск | NVMe/USB SSD; каталог данных вынесен из системного раздела |
| Сеть | Gigabit Ethernet, изолированная LAN со SC589 |

---

## 3. Входной интерфейс (контракт с `prod`)

### 3.1. Транспорт

| Параметр | Значение (как в `prod`) |
|----------|-------------------------|
| Протокол | **UDP** |
| Роль RPi | **сервер** (bind), `prod` — клиент (`sendto`) |
| Порт по умолчанию | `19001` (`SessionConfig::stream_port` в `prod/common/config.hpp`) |
| Единица передачи | 1 UDP-датаграмма = 1 логический пакет **или** фрагмент обёртки (см. §3.3) |
| IP источника | whitelist в конфиге RPi |

Референс отправителя: `prod/arm_core/network_streamer.hpp`.

### 3.2. Формат пакета (обязательно)

Сериализация — `prod/common/serialization.hpp`, типы — `prod/common/types.hpp`.

**Заголовок:** 34 байта, little-endian.

| Смещение | Поле | Тип | Значение / примечание |
|----------|------|-----|------------------------|
| 0 | `magic` | `uint32` | `0x45574F48` |
| 4 | `protocol_version` | `uint16` | `1` |
| 6 | `payload_version` | `uint16` | `1` |
| 8 | `type` | `uint16` | см. таблицу типов |
| 10 | `payload_size` | `uint32` | длина payload |
| 14 | `ts_ns` | `uint64` | метка времени |
| 22 | `seq` | `uint32` | монотонный счётчик (глобальный на поток) |
| 26 | `status_flags` | `uint32` | биты из `StatusFlags` |
| 30 | `checksum` | `uint32` | CRC32 payload |

**CRC32:** алгоритм из `prod/common/checksum.hpp` (полином `0xEDB88320`, init `0xFFFFFFFF`, финал `~crc`).

**Тело:** `payload_size` байт сразу после заголовка.

**Типы пакетов (`PacketType`):**

| ID | Имя | Ожидание на RPi |
|----|-----|-----------------|
| 1 | `kAudioBlock` | записывать, если приходит |
| 2 | `kCanFrame` | записывать, если приходит |
| 3 | `kImuSample` | записывать, если приходит |
| 4 | `kMotionState` | записывать, если приходит |
| 5 | `kGpsFix` | записывать, если приходит |
| 6 | `kSessionFrame` | **основной** рабочий тип |
| 7 | `kDiagnostics` | парсить для `adsp_status_monitor` |
| 8 | `kSessionMeta` | старт/стоп, метаданные с SC589 |

На этапе MVP достаточно корректной записи **всех** принятых валидных типов без интерпретации payload (кроме `kDiagnostics` / `kSessionMeta`).

### 3.3. Обёртка SC589 UART (опционально на LAN)

Если поток приходит с префиксом `A5 5A`, разбор — как в `data/sc589_uart_full_session_format.md` и `prod/arm_core/uart_session_ingest.hpp`:

- SOF `A5 5A`, LEN (LE), VER, внутренний `PacketHeader` + payload, `frame_crc32`;
- режимы: `auto` | `on` | `off` (аналог `UartUnwrapPolicy`).

Для отладки без сети допускается ingest из файла (поведение как `prod_uart_session_to_ego`).

### 3.4. Семантика `seq` (как в `prod`)

Логика **должна совпадать** с `prod/arm_core/transport_guard.hpp`:

- первый пакет принимается без проверки порядка;
- `seq` строго возрастает; `diff > 1` → gap, `packets_lost += diff - 1`;
- `seq <= last_seq` → reject, `out_of_order++`;
- дублировать проверку CRC после `ValidatePacket`.

### 3.5. Stand / тест без железа

- Генератор: `data/sc589_uart_stand_generator.py`, эталонный поток: `data/sc589_uart_full_session_stream.hex`;
- Отправка: `prod` demo или replay-tool → UDP на RPi;
- Приём файла: режим `--input file.bin` для регрессии (не в проде, только dev).

---

## 4. Выходные артефакты модуля

Корень: `{data_root}/sessions/{session_id}/`

| Файл | Назначение |
|------|------------|
| `ego_000.bin`, … | Сырой лог: последовательность `[header][payload]` |
| `ego_manifest.json` | Список чанков, размеры, интервалы времени |
| `ego.index` | Индекс: offset, `ts_ns`, `type`, `seq`, `chunk_id` |
| `session_metadata.json` | Метаданные сессии (§5.2) |
| `scenario_metadata.json` | Сценарий испытаний (§5.2) |
| `runtime_report.json` | Периодический снимок (§5.7) |
| `final_runtime_summary.json` | Итог при stop (§5.9) |
| `logs/runtime_error.log` | Ошибки приёма/диска/сети |

Формат `ego.bin`: **байт-в-байт** как принятый валидный пакет после unwrap (без перекодирования).

---

## 5. Функциональные требования (по модулям таблицы)

### 5.1. Контракт данных

| ID | Компонент | Требование | Критерий готовности |
|----|-----------|------------|---------------------|
| DC-01 | `header` | Парсинг 34 байт LE | Unit-тест на эталоне из `sc589_uart_full_session_stream` |
| DC-02 | `types` | Распознавание `type` 1–8 | Unknown type → reject + log |
| DC-03 | `validation` | `ValidatePacketVerbose` эквивалент `prod` | Тесты из `prod/tests/test_main.cpp` портировать |
| DC-04 | `protocol_version_check` | Отклонение `protocol_version != 1` | Счётчик `bad_packet` |
| DC-05 | `range_validation` | Лимиты: `payload_size <= max_payload_bytes` (8192 по умолчанию, как ingest) | Конфигурируемый порог |

**Зависимость:** допустимо вынести `types.hpp`, `checksum.hpp`, `serialization.hpp`, `validation.hpp` в общую static-библиотеку `ego_protocol` (копия из `prod/common/`) — **без дублирования алгоритмов вручную**.

### 5.2. Управление сессией

| ID | Компонент | Требование |
|----|-----------|------------|
| SM-01 | `session_manager` | UUID или `session-{utc_ms}-{rand}`; состояния: `Idle`, `Recording`, `Stopping`, `Closed`, `Error` |
| SM-02 | `recording_control` | API: `start`, `stop`, `emergency_stop`; автостарт по `kSessionMeta` — опционально (флаг конфига) |
| SM-03 | `session_metadata.json` | Поля: `session_id`, `started_at_utc`, `stopped_at_utc`, `vehicle_id`, `test_stand_config`, `software_version`, `prod_protocol_version`, `source_ip`, `storage_path` |
| SM-04 | `scenario_metadata` | `scenario_id`, `scenario_name`, `operator`, `notes`; задаётся при `start` из CLI/конфига |

**Правило:** одна активная сессия; повторный `start` без `stop` → ошибка `E_SESSION_BUSY`.

### 5.3. Приём данных (от `prod` по сети)

| ID | Компонент | Требование |
|----|-----------|------------|
| RX-01 | `network_receiver` | UDP bind, non-blocking/epoll, поток приёма |
| RX-02 | `sequence_counter_check` | `TransportGuard`-совместимая логика (§3.4) |
| RX-03 | `packet_checksum_check` | CRC заголовка + при unwrap — `frame_crc32` |
| RX-04 | `packet_buffer` | Ring buffer валидных пакетов; при overflow — drop oldest, `health=degraded` |

Конфиг по умолчанию: глубина буфера ≥ 4096 пакетов или 512 MiB.

### 5.4. Первичная запись сырого лога

| ID | Компонент | Требование |
|----|-----------|------------|
| WR-01 | `ego_raw_writer` | Отдельный writer-поток; запись только после RX+validation |
| WR-02 | `chunk_writer` | Ротация: 4 GiB или 60 мин; `ego_manifest.json` обновляется атомарно |
| WR-03 | `ego.index` | Append-only; запись не реже 1 записи на пакет (батчинг допустим) |
| WR-04 | `safe_flush_policy` | `fsync` каждые N пакетов (1000) или T сек (1); при `stop`/`emergency` — немедленный `fsync` |

Референс минимальной записи: `prod/arm_core/ego_bin_writer.hpp` (требуется доработка: persistent FD, flush, chunking).

### 5.5. Контроль хранилища

| ID | Компонент | Требование |
|----|-----------|------------|
| ST-01 | `storage_monitor` | `statvfs` каждые 5 с |
| ST-02 | `storage_alerts` | Warning: <15% или <50 GiB; Critical: <5% или <10 GiB |
| ST-03 | `emergency_stop` | При Critical — `Stopping` → finalize (§5.9), `stop_reason=storage_critical` |

### 5.6. Диагностика входящего потока

| ID | Компонент | Метрики |
|----|-----------|---------|
| IN-01 | `packet_loss_counter` | `seq_gaps`, `packets_lost` |
| IN-02 | `bad_packet_counter` | по `TransportRejectReason` |
| IN-03 | `time_gap_monitor` | если `ts_ns - last_ts > gap_threshold_ns` (default 50 ms) |
| IN-04 | `adsp_status_monitor` | разбор `kDiagnostics` (текст/структура — по согласованному payload с `prod`) |

Экспорт в `runtime_report.json` каждые 10 с.

### 5.7. Диагностика записи

| ID | Компонент | Требование |
|----|-----------|------------|
| RD-01 | `runtime_report.json` | `packets_received`, `packets_written`, `bad_packets`, `packet_loss`, `disk_free_gb`, `write_mbps`, `health` |
| RD-02 | `runtime_error_log` | UTC timestamp, уровень, ротация 10×50 MB |

### 5.8. Служебный интерфейс

| ID | Компонент | Требование |
|----|-----------|------------|
| UI-01 | `cli_status` | Команды: `ego-runtime start\|stop\|status\|stats`; коды выхода документированы |
| UI-02 | `diagnostic_panel` | `ego-runtime diagnostics` — сводка IN-* + RD-* + ST-* |

Веб-UI — вне MVP.

### 5.9. Завершение сессии

| ID | Компонент | Требование |
|----|-----------|------------|
| FN-01 | `finalize_session` | Закрытие FD, `stopped_at_utc`, последний `fsync` |
| FN-02 | `final_runtime_summary` | Итоговые счётчики, duration, `integrity` |
| FN-03 | `session_integrity_check` | Проверка manifest, index, отсутствие `.tmp`; результат: `ok` / `warning` / `failed` |

---

## 6. Архитектура процесса `ego-runtime`

```
┌──────────────────────────────────────────────────────────────┐
│ NetworkRxThread          │  ValidateThread (optional merge)   │
│  UDP recv → unwrap      │  TransportGuard + range_validation │
│  → raw reassembly       │  → packet_buffer                   │
├─────────────────────────┴──────────────────────────────────┤
│ WriterThread          │ StorageMonitorThread │ ReportThread  │
│  pop buffer → ego.bin │  statvfs, alerts     │ runtime_report│
│  chunk + index + fsync│  → emergency_stop    │               │
├───────────────────────┴──────────────────────┴───────────────┤
│ Main / CLI: SessionManager, recording_control, finalize      │
└──────────────────────────────────────────────────────────────┘
```

**Запрещено:** блокировать приём UDP синхронной записью на диск.

---

## 7. Конфигурация

Файл: `/etc/ego-runtime/config.yaml` (пример):

```yaml
network:
  bind_host: "0.0.0.0"
  bind_port: 19001
  allowed_sources: ["192.168.10.50"]
  unwrap_sc589: auto   # auto|on|off
  max_payload_bytes: 8192

storage:
  data_root: "/data/ego-runtime"
  chunk_max_bytes: 4294967296
  chunk_max_sec: 3600
  flush_packets: 1000
  flush_interval_sec: 1
  warn_free_gb: 50
  critical_free_gb: 10

session:
  auto_start_on_meta: false
  vehicle_id: "CAR-001"

diagnostics:
  report_interval_sec: 10
  time_gap_threshold_ms: 50
```

Переопределение через CLI и env `EGO_RUNTIME_*`.

---

## 8. Нефункциональные требования

| ID | Требование |
|----|------------|
| NFR-01 | Непрерывная запись ≥ 8 ч без утечек памяти |
| NFR-02 | При штатной нагрузке: потери < 0.01% пакетов/час (при исправной LAN) |
| NFR-03 | p99 задержка recv→fsync < 20 ms (профиль на целевом SSD) |
| NFR-04 | Автоперезапуск systemd: `Restart=on-failure`, не чаще 3/мин |
| NFR-05 | `--version` выводит semver и hash коммита `ego_protocol` |

---

## 9. Тестирование и приёмка

### 9.1. Unit-тесты

- Парсинг header/packet (эталон `sc589_uart_full_session_stream`);
- CRC, validation, `TransportGuard` gaps/reorder;
- chunk rotation, index consistency;
- `session_integrity_check` на синтетической сессии.

### 9.2. Интеграция с `prod`

| # | Сценарий | Ожидание |
|---|----------|----------|
| IT-01 | `prod` demo → UDP → RPi, 10 мин | `packets_written ≈ packets_received`, `integrity=ok` |
| IT-02 | Поток с намеренно битым CRC | `bad_packet_counter` растёт, битые не в `ego.bin` |
| IT-03 | Пропуск датаграмм (iptables DROP) | `packet_loss_counter` отражает gap |
| IT-04 | `stop` | полный комплект файлов §4 |
| IT-05 | Заполнение диска до Critical | `emergency_stop`, сессия закрыта |

### 9.3. Критерии приёмки релиза MVP

- [ ] **AC-01:** Валидные пакеты из `prod` в `ego.bin` без изменения байтов
- [ ] **AC-02:** Невалидные не записываются
- [ ] **AC-03:** `session_metadata.json` + `scenario_metadata.json`
- [ ] **AC-04:** `runtime_report.json` и `runtime_error.log`
- [ ] **AC-05:** CLI `start` / `stop` / `status` / `diagnostics`
- [ ] **AC-06:** `session_integrity_check` = `ok` после штатного stop

---

## 10. Структура репозитория (рекомендация)

```
ego-runtime/                 # корень в runtimepc/
├── CMakeLists.txt
├── src/
│   ├── network_receiver.cpp
│   ├── transport_guard.cpp    # или линковка ego_protocol
│   ├── session_manager.cpp
│   ├── ego_raw_writer.cpp
│   ├── chunk_writer.cpp
│   ├── storage_monitor.cpp
│   └── main.cpp
├── include/ego_runtime/
├── protocol/                  # копия/субмодуль из prod/common
├── config/config.yaml.example
├── systemd/ego-runtime.service
└── tests/
```

**Связь с `prod`:** `protocol/` синхронизируется с `Huawei/prod/common/`; при изменении контракта в `prod` — MR в оба репозитория.

---

## 11. Этапы разработки

| Этап | Срок (ориентир) | Состав |
|------|-----------------|--------|
| **M1** | 2 нед. | RX-01…04, DC-01…05, WR-01, SM-01…02, IT-01 |
| **M2** | 2 нед. | WR-02…04, ST-01…03, IN-01…03, RD-01…02, IT-02…03 |
| **M3** | 1 нед. | FN-01…03, UI-01…02, SM-03…04, IN-04, IT-04…05 |

---

## 12. Риски

| Риск | Действие |
|------|----------|
| UDP-потери | QoS/VLAN; согласовать с `prod` дублирование на SD SC589 |
| Расхождение CRC/seq с `prod` | Общая библиотека `ego_protocol`, общие тесты |
| Запись на microSD RPi | Запрет в эксплуатации; только внешний SSD |

---

## 13. Ссылки для разработчика

| Артефакт | Путь |
|----------|------|
| Типы и версии протокола | `../prod/common/types.hpp` |
| Сериализация | `../prod/common/serialization.hpp` |
| CRC | `../prod/common/checksum.hpp` |
| Валидация | `../prod/common/validation.hpp` |
| Проверка seq на стороне prod | `../prod/arm_core/transport_guard.hpp` |
| UDP-отправитель (контракт порта) | `../prod/arm_core/network_streamer.hpp`, `../prod/common/config.hpp` |
| Unwrap SC589 | `../prod/arm_core/uart_session_ingest.hpp`, `../data/sc589_uart_full_session_format.md` |
| Stand-поток | `../data/sc589_uart_stand_generator.py` |

---

## 14. Итог

Модуль на RPi5 — **приёмник и архиватор** потока, который формирует `prod`. Разработчик реализует всё из таблицы «Бортовой runtime-модуль мини-ПК», **кроме** генерации пакетов и сенсорной логики; вход — UDP-пакеты по контракту §3.
