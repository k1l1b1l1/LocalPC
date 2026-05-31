# Модуль `pipeline/offline` (ego-offline) — обзор по файлам

> Краткое объяснение для разбора архитектуры.  
> Дата: 30.05.2026

---

## Зачем нужен этот модуль

**`pipeline/offline`** — программа **`ego-offline`**: post-session конвейер на Local PC, который **читает каталог сессии** (выход `runtimepc` + опционально `source.bin` от `micro`), **валидирует, синхронизирует, выравнивает**, строит **MDF4 и JSON-отчёты**, и **опционально** выгружает результат в **S3**.

Runtime и micro **не обрабатывают** данные глубокo — это делает offline pipeline.

---

## Вход: что нужно на входе

Не пара файлов «ego.bin + source.bin», а **каталог сессии** `{session_dir}/`:

| Файл | Обязательность | Откуда |
|------|----------------|--------|
| `ego_0.bin`, `ego_1.bin`, … | **Да** | `runtimepc` |
| `ego_manifest.json` | **Да** | `runtimepc` |
| `session_metadata.json` | **Да** | `runtimepc` |
| `scenario_metadata.json` | рекомендуется | `runtimepc` |
| `ego.index` | опционально | `runtimepc` (offline пока не читает — P2) |
| `source.bin` | **опционально** | `micro` |
| `source_metadata.json` | рекомендуется | `micro` |
| `final_runtime_summary.json` | опционально | `runtimepc` |

**Без `source.bin`:** pipeline **не падает** — sync пропускается, в `sync_report.json` будет note «source.bin absent».

**Legacy:** один файл `ego.bin` — reader может объединить логически (через manifest).

---

## Выход: где лежит результат

```
{session_dir}/
  ego_0.bin                 ← вход (runtime, не трогаем)
  source.bin                ← вход (micro, опционально)
  session_metadata.json     ← вход
  offline/                  ← ВЫХОД pipeline
    session.mf4
    session.mf4.sha256
    validation_report.json
    sync_report.json
    scene_events.json
    scene_segments.json
    session_report.json
    data_quality_report.json
    events_report.json
    upload_report.json      ← если был S3 upload
    upload_manifest.json
```

Пример e2e-сессии:

```
runtimepc/e2e_file_test/sessions/session-1779998724656-4271/offline/
```

---

## S3: автоматически ли улетает выход?

**Нет, не автоматически.**

Upload в S3 происходит **только если одновременно**:

1. Запуск с флагом **`--upload-s3`** (или отдельная команда `ego-offline upload`).
2. В конфиге **`s3.enabled: true`** (по умолчанию **`false`**).
3. Настроены credentials (env `EGO_S3_ACCESS_KEY` / `EGO_S3_SECRET_KEY` или локальный `config/s3.local.yaml`, gitignored).

Код в `pipeline.cpp`:

```cpp
if (opts.upload_s3 && cfg_.s3.enabled) {
    uploader.upload_session(opts.session_dir, out, ...);
}
```

**Оркестратор стенда** (`stand/run_session.py`) **не вызывает** S3 upload — только `ego-offline process` без `--upload-s3`.

### Что именно уходит на S3

**Не вся папка сессии**, а только файлы из **`{session_dir}/offline/`** (обработанный результат):

| Upload | Не upload (по умолчанию) |
|--------|--------------------------|
| `session.mf4`, `session.mf4.sha256` | `ego_0.bin`, `ego_1.bin` |
| `session_report.json` | `source.bin` |
| `validation_report.json` | `ego.index`, metadata runtime |
| `sync_report.json` | |
| `scene_events.json`, `scene_segments.json` | |
| `data_quality_report.json`, `events_report.json` | |

Ключ объекта:

```
s3://{bucket}/{prefix}/{session_id}/offline/{filename}
```

Пример: `ego-sessions/session-1779998724656-4271/offline/session.mf4`

### Gate перед upload

- `require_real_mdf4: true` — блокирует upload, если MDF4 stub/placeholder.
- `require_pipeline_success: false` по умолчанию — upload возможен даже при `pipeline_status: partial/failed` (если gate не ужесточён).

---

## CLI

```powershell
cd pipeline/offline

# Только валидация
ego-offline validate --session-dir <path> --config config/stand.pc.yaml

# Полный process (локально, без S3)
ego-offline process --session-dir <path> --config config/stand.pc.yaml

# Process + S3
ego-offline process --session-dir <path> --config config/stand.pc.yaml --upload-s3 --s3-config config/s3.local.yaml

# Только upload уже готового offline/
ego-offline upload --session-dir <path> --s3-config config/s3.local.yaml

# Только до этапа
ego-offline process --session-dir <path> --only validate
ego-offline process --session-dir <path> --only sync
ego-offline process --session-dir <path> --only mdf4
ego-offline process --session-dir <path> --only scenes
```

**Exit codes:** `0` ok, `1` validation, `2` sync (если `fail_on_sync_error`), `3` internal, `4` upload_failed, `5` upload_blocked.

---

## Пошаговый пайплайн (10 этапов)

```
Каталог сессии
  ego_*.bin + ego_manifest.json
  source.bin (если есть)
  session_metadata.json
        │
        ▼
┌─────────────────────────────────────────────────────────┐
│ 1. Load metadata     metadata_loader                    │
│ 2. Load + parse ego  ego_log_reader → binary_parser     │
│                      → packet_demuxer                   │
│ 3. Load + parse src  source_log_reader (если есть)      │
│ 4. Validate          log_validator → validation_report   │
│ 5. Sync              time_sync (ego ↔ source)           │
│                      → sync_report.json                 │
│ 6. Align             time_aligner (сетка 10 ms)         │
│ 7. Scene geometry    range, azimuth, closing speed      │
│ 8. MDF4              session.mf4 + .sha256              │
│ 9. Scene finalize    scene_events, scene_segments       │
│10. Reports           session_report, data_quality,      │
│                      events_report                      │
│11. S3 (опционально)  upload offline/ → bucket           │
└─────────────────────────────────────────────────────────┘
```

---

## Файлы по назначению

### Точка входа

| Файл | Назначение |
|------|------------|
| `src/main.cpp` | CLI: `process`, `validate`, `upload`, `export-mdf4` |

### Оркестратор

| Файл | Назначение |
|------|------------|
| `include/ego_offline/pipeline/pipeline.hpp` | `Pipeline`, `RunOptions` |
| `src/pipeline/pipeline.cpp` | Все этапы `run_full` / `run_validate` |

### Загрузка (LD-*)

| Файл | Назначение |
|------|------------|
| `src/load/ego_log_reader.cpp` | Чтение `ego_*.bin` по manifest |
| `src/load/source_log_reader.cpp` | Чтение `source.bin` |
| `src/load/metadata_loader.cpp` | `session_metadata.json`, scenario |

### Парсинг (PR-*)

| Файл | Назначение |
|------|------------|
| `src/parse/binary_parser.cpp` | magic, CRC, size |
| `src/parse/packet_demuxer.cpp` | ego types 1–8 |
| `src/parse/source_demuxer.cpp` | source types 101–105 |
| `include/ego_offline/protocol.hpp` | контракт пакетов |

### Валидация (VL-*)

| Файл | Назначение |
|------|------------|
| `src/validate/log_validator.cpp` | seq, gaps, valid_ranges → `validation_report.json` |

### Синхронизация (SY-*)

| Файл | Назначение |
|------|------------|
| `src/sync/time_sync.cpp` | offset ego↔source → `sync_report.json` |

### Align (AL-*)

| Файл | Назначение |
|------|------------|
| `src/align/time_aligner.cpp` | общая сетка 10 ms, интерполяция |

### Сцена (SC-*)

| Файл | Назначение |
|------|------------|
| `src/scene/scene_geometry.cpp` | range, azimuth, closing speed |

### MDF4 (MF-*, EX-*)

| Файл | Назначение |
|------|------------|
| `src/mdf4/mdf4_writer.cpp` | подготовка channel groups |
| `src/mdf4/mdf4_lib_adapter.cpp` | запись MDF4 v4.1 + SHA256 sidecar |
| `src/mdf4/mdf4_channel_builder.cpp` | заполнение данных каналов |

### Финализация (FN-*)

| Файл | Назначение |
|------|------------|
| `src/finalize/scene_builder.cpp` | `scene_events.json`, `scene_segments.json` |

### Отчёты (RP-*)

| Файл | Назначение |
|------|------------|
| `src/reports/report_builder.cpp` | session/data_quality/events reports |

### S3 upload

| Файл | Назначение |
|------|------------|
| `src/upload/s3_uploader.cpp` | сбор файлов, gate, manifest |
| `src/upload/s3_client.cpp` | HTTP PUT в S3-compatible API |

### Конфигурация

| Файл | Назначение |
|------|------------|
| `include/ego_offline/config.hpp` | `Config`, defaults (`s3.enabled=false`) |
| `src/config.cpp` | YAML parser |
| `config/stand.pc.yaml` | стенд: короткие сессии, мягкий sync |
| `config/s3.yaml.example` | пример S3 (без secrets) |
| `config/s3.local.yaml` | локальные credentials (**gitignore**, не коммитить) |

---

## Связь с другими модулями

```
runtimepc  → ego_*.bin, manifest, metadata
micro      → source.bin (опционально)
     ↓
pipeline/offline  → offline/*.json, session.mf4
     ↓ (если --upload-s3 + s3.enabled)
S3 bucket  → {prefix}/{session_id}/offline/...
```

---

## Соответствие ТЗ (сводка)

| Блок ТЗ | Статус |
|---------|--------|
| Загрузка ego/source | ✅ |
| Парсинг, валидация | ✅ |
| Синхронизация ego↔source | ✅ (нужен source.bin) |
| Align, геометрия сцены | ✅ |
| MDF4 export | ⚠️ свой writer, не ASAM SDK |
| Scene events/segments | ✅ |
| JSON-отчёты | ✅ |
| S3 upload | ✅ (ручное включение) |
| Beamforming | ❌ |
| index_loader (ego.index) | ⏳ P2 |
| local_cleaner после S3 | ❌ |
| Multipart upload больших MF4 | ⏳ |

Подробнее: `pipeline/documentation/PRODUCTION_READY.md`, `TZ_ego_offline_pipeline.md`, `TZ_s3_upload.md`.

---

## Частые вопросы

**После `process` всё само улетает в S3?**  
Нет. Нужны `--upload-s3` и `s3.enabled: true` + credentials.

**Upload по папке сессии?**  
По **session_id** из metadata, но файлы берутся только из **`offline/`**, не сырой ego/source.

**Можно проверить без S3?**  
Да. `ego-offline process` без `--upload-s3` — результат только локально в `offline/`.

**Порядок чтения кода:**  
`main.cpp` → `pipeline.cpp` → `ego_log_reader` → `time_sync` → `mdf4_writer` → `s3_uploader.cpp`.

---

## Ссылки

| Документ | Путь |
|----------|------|
| ТЗ offline pipeline | `pipeline/documentation/TZ_ego_offline_pipeline.md` |
| ТЗ S3 | `pipeline/documentation/TZ_s3_upload.md` |
| Готовность | `pipeline/documentation/PRODUCTION_READY.md` |
| Контракт source | `pipeline/documentation/contracts/source_bin_v1.md` |
| Upstream runtime | `runtimepc/RUNTIMEPC_GUIDE.md` |
