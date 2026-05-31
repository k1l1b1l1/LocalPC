# Модуль `runtimepc` (ego-runtime) — обзор по файлам

> Краткое объяснение для разбора архитектуры.  
> Дата: 30.05.2026

---

## Зачем нужен этот модуль

**`runtimepc`** — программа **`ego-runtime`**: «чёрный ящик» на мини-ПК (целевая платформа — Raspberry Pi 5), который **принимает поток пакетов от модуля `prod` по LAN (UDP)** и **записывает их на диск** в файлы `ego_0.bin`, `ego_1.bin` и т.д.

Runtime **не выполняет глубокую обработку** данных — только приём, валидацию по контракту и запись сырого лога. Дальше каталог сессии забирает **offline pipeline** (`pipeline/offline`).

---

## Откуда приходят данные: LAN / UDP

Да, в штатном режиме данные приходят **по локальной сети (LAN)**:

| Параметр | Значение по умолчанию |
|----------|----------------------|
| Транспорт | **UDP** (датаграммы, без установки соединения) |
| Порт | **19001** (`network.bind_port` в конфиге) |
| Интерфейс | `0.0.0.0` — слушает все сетевые интерфейсы |
| Отправитель | модуль **`prod`** на бортовом/стендовом ПК |
| Фильтр IP | `network.allowed_sources` — whitelist адресов prod |

Типичная схема на стенде:

```
[prod]  ──UDP :19001, LAN──►  [мини-ПК / RPi5: ego-runtime]
                                    │
                                    └── запись в NVMe/SSD
                                        sessions/{session_id}/ego_*.bin
```

**Dev-режим без сети:** `ego-runtime start --input file.bin` — читает готовый бинарный файл локально (для тестов на Windows без UDP).

**Управление демоном** (start/stop/status) — отдельный канал **IPC** (не поток данных):
- Linux: Unix socket `{data_root}/ego-runtime.sock`
- Windows: TCP `127.0.0.1:19002`

---

## Общая схема потоков внутри процесса

```
┌─────────────────────────────────────────────────────────────────┐
│  NetworkReceiver (UDP :19001, LAN)  │  ProcessFileInput (dev)   │
├─────────────────────────────────────┴───────────────────────────┤
│  Sc589Unwrap — снятие обёртки ADSP-SC589 (если есть)            │
├─────────────────────────────────────────────────────────────────┤
│  TransportGuard — magic, CRC, seq, порядок пакетов              │
├─────────────────────────────────────────────────────────────────┤
│  PacketBuffer — очередь между быстрым UDP и медленным диском    │
├─────────────────────────────────────────────────────────────────┤
│  WriterLoop → ChunkWriter → ego_0.bin, ego.index, manifest      │
├─────────────────────────────────────────────────────────────────┤
│  StorageMonitor — место на диске → emergency stop               │
│  ReportLoop — runtime_report.json                               │
│  ControlServer — IPC start/stop/status                          │
│  SessionManager — жизненный цикл сессии, metadata JSON          │
└─────────────────────────────────────────────────────────────────┘
```

Всё координирует класс **`RuntimeService`**.

---

## Структура каталога

```
runtimepc/
├── src/                    ← реализация (.cpp)
├── include/ego_runtime/    ← интерфейсы классов (.hpp)
├── protocol/               ← контракт пакетов (синхронизирован с prod/common)
├── config/                 ← примеры YAML-конфигов
├── tests/                  ← unit-тесты
├── systemd/                ← unit для автозапуска на Linux (RPi5)
├── e2e_file_test/          ← тестовые сессии с реальными ego_*.bin
├── CMakeLists.txt          ← сборка
├── README.md               ← команды CLI и сборка
├── arch.md                 ← архитектурный аудит (для разработчиков)
└── RUNTIMEPC_GUIDE.md      ← этот файл
```

---

## Файлы по назначению

### Точка входа

| Файл | Назначение |
|------|------------|
| `src/main.cpp` | CLI: `run`, `start`, `stop`, `status`, `diagnostics`, `--input`, `--version` |

**Команды:**

| Команда | Что делает |
|---------|------------|
| `ego-runtime run` | Долгоживущий демон: UDP + запись, ждёт SIGTERM/SIGINT |
| `ego-runtime start` | Через IPC: начать запись в работающем демоне |
| `ego-runtime stop` | Через IPC: остановить запись |
| `ego-runtime status` | Статус сессии |
| `ego-runtime diagnostics` | Метрики (пакеты, ошибки, скорость записи) |
| `--input file.bin` | Dev: чтение из файла вместо LAN/UDP |

**Коды выхода:** `0` ok, `1` session_busy, `2` not_recording, `3` storage, `4` config, `5` internal.

---

### Главный оркестратор

| Файл | Назначение |
|------|------------|
| `include/ego_runtime/runtime_service.hpp` | Интерфейс `RuntimeService` |
| `src/runtime_service.cpp` | Связывает приём, валидацию, буфер, запись, отчёты, IPC |

Основные методы:
- `StartDaemon()` — UDP-приёмник, storage monitor, writer/report потоки
- `StartRecording()` — создаёт каталог сессии, открывает `ChunkWriter`
- `OnDatagram()` → `OnUnwrappedPacket()` — путь одного пакета
- `StopRecording()` — drain буфера, finalize, integrity check
- `EmergencyStop()` — при критическом заполнении диска

**Фоновые потоки:**
- Writer thread — `PacketBuffer` → `ChunkWriter`
- Report thread — периодический `runtime_report.json`
- Network thread — внутри `NetworkReceiver`
- Storage thread — внутри `StorageMonitor`
- Control thread — внутри `ControlServer`

---

### Управление сессией

| Файл | Назначение |
|------|------------|
| `include/ego_runtime/session_manager.hpp` | Состояния сессии, metadata |
| `src/session_manager.cpp` | Создание `{data_root}/sessions/{session_id}/` |

**Состояния:**

| Состояние | Смысл |
|-----------|-------|
| `kIdle` | Запись не идёт |
| `kRecording` | Идёт запись |
| `kStopping` | Остановка, слив буфера |
| `kClosed` | Сессия завершена |
| `kError` | Ошибка (диск, конфиг…) |

**JSON-артефакты сессии:**
- `session_metadata.json` — session_id, время, vehicle_id, IP источника (LAN)
- `scenario_metadata.json` — scenario_id, operator, notes

---

### Приём по LAN

| Файл | Назначение |
|------|------------|
| `include/ego_runtime/network_receiver.hpp` | UDP-сокет, callback на датаграмму |
| `src/network_receiver.cpp` | Поток recv; Linux: epoll; Windows: poll |

- Порт по умолчанию: **19001**
- Whitelist IP: `allowed_sources` в конфиге
- `allow_all_sources: false` + пустой whitelist = **все источники отклоняются**

---

### Разбор обёртки SC589

| Файл | Назначение |
|------|------------|
| `include/ego_runtime/sc589_unwrap.hpp` | Снятие транспортной обёртки ADSP-SC589 |

Prod может слать пакеты в обёртке: `[SOF 0xA5 0x5A][len][inner packet][CRC]`.  
Режим: `unwrap_sc589: auto | on | off` в конфиге.

---

### Контракт пакетов (protocol/)

Копия из `prod/common/`. Runtime не задаёт формат сам — он согласован с prod.

| Файл | Назначение |
|------|------------|
| `protocol/types.hpp` | `PacketHeader` (34 байта), типы пакетов 1–8, magic `0x45574F48` |
| `protocol/serialization.hpp` | Serialize/Deserialize header и packet |
| `protocol/checksum.hpp` | CRC32 payload |
| `protocol/validation.hpp` | magic, version, размер |
| `protocol/transport_guard.hpp` | seq, gap, out-of-order, checksum |

**Формат одного пакета на диске:**

```
[PacketHeader 34 байта LE][payload N байт]
```

Подробнее о типах payload — в `pipeline/offline/include/ego_offline/protocol.hpp` и `prod/common/types.hpp`.

---

### Буфер и запись на диск

| Файл | Назначение |
|------|------------|
| `include/ego_runtime/packet_buffer.hpp` | Очередь между UDP и диском (~4096 пакетов / 512 MB) |
| `include/ego_runtime/ego_raw_writer.hpp` | Обёртка: bytes → ChunkWriter |
| `include/ego_runtime/chunk_writer.hpp` | Интерфейс записи чанков |
| `src/chunk_writer.cpp` | `ego_0.bin`, `ego.index`, `ego_manifest.json` |

**Выходные файлы записи:**

| Файл | Содержимое |
|------|------------|
| `ego_0.bin`, `ego_1.bin`, … | Сырые пакеты байт-в-байт (как приняты после unwrap) |
| `ego.index` | CSV: `offset, ts_ns, type, seq, chunk_id` |
| `ego_manifest.json` | Список чанков: bytes, packet_count, first/last ts |

Ротация чанка: по размеру (4 GB) или времени (1 час).

---

### Мониторинг и диагностика

| Файл | Назначение |
|------|------------|
| `include/ego_runtime/diagnostics.hpp` | Счётчики: received, written, bad, gaps, reject reasons |
| `include/ego_runtime/storage_monitor.hpp` | Свободное место на диске |
| `src/storage_monitor.cpp` | Warning / critical → emergency stop |
| `include/ego_runtime/error_log.hpp` | Текстовый лог ошибок |
| `src/error_log.cpp` | `logs/runtime_error.log`, ротация |

**Отчёты:**
- `runtime_report.json` — периодически во время записи
- `final_runtime_summary.json` — итог после stop (duration, integrity)

---

### IPC и завершение

| Файл | Назначение |
|------|------------|
| `include/ego_runtime/control_ipc.hpp` | ControlServer / ControlClient, pid-файл |
| `src/control_ipc.cpp` | Unix socket (Linux) или TCP :19002 (Windows) |
| `include/ego_runtime/shutdown.hpp` | SIGTERM/SIGINT |
| `src/shutdown.cpp` | Graceful shutdown для systemd |
| `include/ego_runtime/session_integrity.hpp` | Проверка целостности сессии |
| `src/session_integrity.cpp` | Сверка manifest ↔ index ↔ файлы |

---

### Конфигурация и сборка

| Файл | Назначение |
|------|------------|
| `include/ego_runtime/config.hpp` | Структура `RuntimeConfig` |
| `src/config.cpp` | YAML + CLI + env (`EGO_RUNTIME_*`) |
| `config/config.yaml.example` | Пример для RPi5 / production |
| `config/config.windows-test.yaml` | Пример для dev на Windows |
| `CMakeLists.txt` | `ego_runtime_lib`, `ego-runtime`, тесты |
| `systemd/ego-runtime.service` | Autostart на Linux |
| `cmake/aarch64-linux-gnu.cmake` | Кросс-сборка под ARM |

---

### Тесты и примеры

| Файл / каталог | Назначение |
|----------------|------------|
| `tests/test_main.cpp` | Unit-тесты: protocol, transport, chunk, integrity |
| `e2e_file_test/sessions/...` | Реальная тестовая сессия (133 пакета в `ego_0.bin`) |

Пример выхода сессии:

```
sessions/session-1779998724656-4271/
  ego_0.bin
  ego.index
  ego_manifest.json
  session_metadata.json
  scenario_metadata.json
  runtime_report.json
  final_runtime_summary.json
  logs/runtime_error.log
  offline/                  ← результат pipeline (не пишет runtime)
```

---

## Связь с другими модулями проекта

```
prod          → формирует пакеты, шлёт UDP по LAN :19001
     ↓
runtimepc     → принимает LAN, пишет ego_*.bin + metadata
     ↓
pipeline      → читает ego + source.bin, sync, MDF4, upload
micro         → пишет source.bin (параллельно на стенде)
stand         → оркестратор полной цепочки на ПК-стенде
```

**Важно:** `source.bin` runtime **не создаёт** — это модуль `micro`.

---

## Порядок чтения кода (рекомендуемый)

1. `README.md` — сборка и CLI
2. `src/main.cpp` — точка входа
3. `src/runtime_service.cpp` — полный пайплайн данных
4. `src/network_receiver.cpp` — приём LAN/UDP
5. `src/chunk_writer.cpp` — что пишется на диск
6. `protocol/types.hpp` — формат одного пакета
7. `e2e_file_test/sessions/.../ego_0.bin` + `ego.index` — живой пример выхода

---

## Частые вопросы

**Данные приходят по LAN?**  
Да. Штатный путь — **UDP по локальной сети** от `prod` на порт **19001**. Альтернатива для разработки — `--input file.bin` без сети.

**Runtime декодирует GPS/IMU/аудио?**  
Нет. Только сохраняет сырые байты пакетов. Разбор — в `pipeline/offline`.

**Почему `ego_0.bin`, а не один `ego.bin`?**  
Runtime режет лог на **чанки** (размер/время). Формат пакетов внутри — тот же, что в `ego.bin` у prod.

**IPC-команды не работают?**  
Нужен запущенный демон: `ego-runtime run` (или systemd). `start`/`stop` в отдельном процессе общаются через socket/TCP, а не через LAN с prod.

---

## Ссылки

| Документ | Путь |
|----------|------|
| ТЗ модуля | `runtimepc/TZ_ego_runtime_rpi5.md` |
| Архитектурный аудит | `runtimepc/arch.md` |
| Контракт prod | `prod/common/types.hpp`, `prod/common/serialization.hpp` |
| Контракт pipeline | `pipeline/offline/include/ego_offline/protocol.hpp` |
| Интеграция стенда | `STAND.md`, `TZ_pc_stand_system_integration.md` |
