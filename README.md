# RPi5

Два модуля для Raspberry Pi 5.

```
SC589 / PC-симулятор  →  runtimepc  →  offline  →  S3
     TCP :5001              запись         MDF4
```

## runtimepc (`ego-runtime`)

Подключается к отправителю по TCP Data `:5001`, принимает EgoFrame, пишет сессию в `/data/ego-sessions/sessions/session-.../` (`ego_*.bin`, manifest, metadata).

После stop сам запускает `ego-offline` в фоне.

## offline (`ego-offline`)

Читает каталог сессии, строит `offline/session.mf4` и JSON-отчёты, загружает в S3 файлы сессии и `offline/`.

## common / scripts

`common/ego_v1/` — заголовки контракта для сборки runtimepc.  
`scripts/sync_ego_contract.sh` — обновить их с ПК (на Pi не нужен).

---

## Установка на Pi (один раз)

```bash
sudo apt install -y cmake g++ curl openssl nlohmann-json3-dev

# runtime
cd runtimepc && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
sudo cp build/ego-runtime /usr/local/bin/
sudo cp config/board.yaml /etc/ego-runtime/config.yaml
sudo cp systemd/ego-runtime.service /etc/systemd/system/
sudo mkdir -p /data/ego-sessions

# offline
cd ../offline && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
sudo cp build/ego-offline /usr/local/bin/
sudo cp config/board.yaml /etc/ego-offline/config.yaml

# s3 credentials — отдельно, не из git (шаблон: offline/config/s3.yaml.example)
sudo cp s3.local.yaml /etc/ego-offline/s3.local.yaml
sudo chmod 600 /etc/ego-offline/s3.local.yaml

sudo systemctl daemon-reload
sudo systemctl enable --now ego-runtime
```

В `/etc/ego-runtime/config.yaml` укажи `network.ego_host` — IP SC589 или PC.

---

## Запуск

```bash
sudo systemctl start ego-runtime
ego-runtime start          # начать запись
ego-runtime stop           # стоп → offline → S3 автоматически
```

Данные:

| Где | Что |
|-----|-----|
| `/data/ego-sessions/sessions/session-.../` | сырой лог от runtime |
| `.../offline/` | MDF4 и отчёты |
| `s3://{bucket}/{prefix}/{session_id}/` | сессия + offline/ в облаке |

Лог автозапуска offline: `{session_dir}/logs/offline_trigger.json`.
