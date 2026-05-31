# ego-runtime — готовность к production

**Дата:** 30.05.2026  
**Версия:** 1.0.0  
**ТЗ:** [TZ_ego_runtime_rpi5.md](TZ_ego_runtime_rpi5.md)

---

## Вердикт

| Режим | Статус |
|-------|--------|
| **Production RPi5 + prod UDP** | **Готов к пилоту** — после IT на целевом железе |
| **Production Windows / stand_pc** | **Готов** — file-ingest, UDP localhost, IPC |
| **Полная NFR-приёмка (8 ч soak, p99)** | **Не выполнена** — требует замеров на RPi5 + NVMe |

Runtime можно выпускать в продуктив как **приёмник и архиватор** потока от `prod` по контракту §3 ТЗ.

---

## Критерии приёмки MVP (§9.3)

| ID | Критерий | Статус |
|----|----------|--------|
| AC-01 | Валидные пакеты в `ego_*.bin` byte-for-byte | ✅ |
| AC-02 | Невалидные не записываются | ✅ |
| AC-03 | `session_metadata.json` + `scenario_metadata.json` | ✅ |
| AC-04 | `runtime_report.json` + `runtime_error.log` | ✅ |
| AC-05 | CLI `start` / `stop` / `status` / `diagnostics` | ✅ (IPC) |
| AC-06 | `session_integrity_check` = ok после stop | ✅ unit + e2e file |

---

## Сборка и тесты

```powershell
cd runtimepc
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Кросс-сборка RPi5:

```bash
cmake -B build-arm -DCMAKE_TOOLCHAIN_FILE=cmake/aarch64-linux-gnu.cmake
cmake --build build-arm
```

---

## Деплой (RPi5)

1. Сборка `aarch64`, установка бинарника и `config/board.yaml` → `/etc/ego-runtime/config.yaml`.
2. Каталог данных на NVMe: `storage.data_root`.
3. `systemd`: `systemd/ego-runtime.service` (`Restart=on-failure`, `StartLimitBurst=3`).
4. Whitelist: `network.allowed_sources` (пустой + `allow_all_sources: false` → deny all).

---

## Конфигурация

| Файл | Назначение |
|------|------------|
| `config/board.yaml` | production → `/etc/ego-runtime/config.yaml` |
| `config/dev/stand.pc.yaml` | PC lab, TCP/UDP |
| `config/dev/windows-test.yaml` | Windows dev |

```powershell
ego-runtime run --config config/dev/stand.pc.yaml
ego-runtime start --config config/dev/stand.pc.yaml
ego-runtime run --config config/dev/stand.pc.yaml --input path.bin
```

---

## Модули ТЗ (§5)

| Блок | Статус | Примечание |
|------|--------|------------|
| DC-01…05 Контракт | ✅ | `protocol/` = копия `prod/common` |
| SM-01…04 Сессия | ✅ | IPC, metadata |
| RX-01…04 Приём | ✅ | epoll Linux; poll Windows |
| WR-01…04 Запись | ✅ | periodic `fsync` Linux (WR-04) |
| ST-01…03 Диск | ✅ | emergency_stop |
| IN-01…03 Диагностика | ✅ | |
| IN-04 adsp_status | ⚠️ | текстовый payload |
| RD-01…02 Отчёты | ✅ | |
| UI-01…02 CLI | ✅ | |
| FN-01…03 Finalize | ✅ | `duration_sec`, integrity |

---

## Выходные артефакты (§4)

| Файл | Статус |
|------|--------|
| `ego_0.bin`, … | ✅ |
| `ego_manifest.json` | ✅ |
| `ego.index` | ✅ |
| `session_metadata.json` | ✅ |
| `scenario_metadata.json` | ✅ |
| `runtime_report.json` | ✅ |
| `final_runtime_summary.json` | ✅ |
| `logs/runtime_error.log` | ✅ |

---

## Рекомендуемая приёмка перед боевым RPi5

| # | Сценарий | Ожидание |
|---|----------|----------|
| IT-01 | prod → UDP → RPi, ≥ 10 мин | `packets_written ≈ packets_received`, integrity ok |
| IT-02 | Битый CRC | рост `bad_packets`, не в bin |
| IT-03 | DROP datagrams | `packets_lost` отражает gap |
| IT-04 | stop | полный комплект §4 |
| IT-05 | Critical disk | emergency_stop |

E2e file (dev): каталог `e2e_file_test/sessions/session-*` — эталон для `pipeline` IT-03.

---

## Известные ограничения

1. **Windows** — без `fsync` (ОС); для prod только Linux/RPi5.
2. **IN-04** — структурный разбор `kDiagnostics` отложен до согласования payload с prod.
3. **protocol/** — ручная синхронизация с `prod/common` (C-01 backlog).
4. **NFR soak / p99** — не измерялись на целевом SSD.

---

## Связанные документы

| Документ | Путь |
|----------|------|
| Архитектурный аудит | [arch.md](arch.md) |
| README | [README.md](README.md) |
| Downstream | `pipeline/documentation/PRODUCTION_READY.md` |
