# Верификация reconnect / resume на SC589 + RPi5

Проверка сценария: плата продолжает писать на SD при обрыве Pi; LocalPC догоняет поток в **той же** session dir без нового этапа.

## Предусловия

- RPi5: `ego-runtime` собран, `./install.sh`, `etc/config.yaml` с IP SC589
- SC589: Control `:5000`, Data `:5001`, активная сессия по ego-contract v1.3
- `network.reconnect_enabled: true`, `checkpoint_packets: 200`

## IT-04a — обрыв Data 5–10 с, автоматический reconnect

```bash
cd ~/runtimepc   # или путь install
./run.sh run &   # или systemd ego-runtime
./run.sh start
# записать 30 с
./run.sh status  # session_state=Recording data_link=up last_seq=N
```

1. Отключить Ethernet на Pi на 10 с (или `iptables` drop к IP платы).
2. Плата продолжает запись на SD (проверить на плате / логах).
3. Восстановить сеть.
4. В течение `reconnect_interval_ms` × N:

```bash
./run.sh status
```

Ожидание:

- `session_state=Recording` (тот же `session_id`)
- `data_link=up`
- `reconnect_count` ≥ 1
- `packets_written` растёт после восстановления
- `ego.index`: без дублей `seq` (offline integrity `ok`)

## IT-04b — replay и dedup

После reconnect:

- `packets_replayed` > 0 (кадры из replay-окна платы)
- Прирост строк в `ego.index` ≈ только новые `seq` (не весь replay)

## IT-04c — kill ego-runtime, resume

```bash
./run.sh start
sleep 20
kill $(cat var/ego-runtime.pid)   # или pkill ego-runtime
./run.sh run &
./run.sh resume
./run.sh status
```

Ожидание:

- тот же `session_dir` / `session_id`
- `last_seq` ≥ значения до kill (из `logs/session_checkpoint.json` или `ego.index`)
- запись продолжается после resume

Опционально: `session.auto_resume_on_run: true` — `./run.sh run` без отдельного `resume`.

## IT-04d — start при незавершённой сессии

```bash
# после обрыва без stop:
./run.sh start
```

Ожидание: `ERR session_busy use RESUME`

## Финализация — один session_id до stop

```bash
./run.sh stop
```

- `final_runtime_summary.json` в той же dir
- offline/S3: **одна** сессия, один `session_id`
- В `runtime_report.json`: `reconnect_count`, `packets_replayed`; при gap beyond replay — `seq_gaps` > 0

## Критерии приёмки

| ID | Критерий |
|----|----------|
| IT-04a | Тот же `session_id`, dedup seq, `data_link=up` после восстановления |
| IT-04b | `packets_replayed` > 0, на диск только новые seq |
| IT-04c | `resume` восстанавливает dir и `last_seq` |
| IT-04d | `start` → `session_busy` или auto-resume по конфигу |

## Unit-тесты (без платы)

На Pi / Linux:

```bash
cd build && cmake .. && cmake --build . && ctest -R ego_runtime_tests -V
```

Тест `TestIT04ReplayReconnectDedup`: mock replay server, disconnect/reconnect, dedup seq.

## Известные ограничения

- Replay-окно RAM на плате ограничено; длинный обрыв → gap в Pi-архиве (SD платы полный).
- `session.backfill_enabled` — фаза 3 (Control `log_get`), не в MVP.
