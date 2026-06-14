# RPi5

```
SC589 / PC  →  runtimepc  →  offline  →  S3
```

## runtimepc

TCP-клиент `:5001`, пишет сессию в `runtimepc/var/sessions/`. После stop запускает offline.

Для нового контура `board + SourceSiren` в репозитории есть helper:

- `runtimepc/bin/localpc-finalize`

Он собирается вместе с `runtimepc` и запускается удалённо по SSH на самом `LocalPC`:

- забирает `raw` у `mic_web_control`
- забирает `source.bin` у `SirenSource`
- собирает `session-*`
- запускает `offline`
- дожидается итогового S3 статуса

Если `LocalPC` забирает `source.bin` у `SirenSource` по паролю, на Pi нужен
`sshpass`. Если используется SSH-ключ, `sshpass` не требуется.

## offline

MDF4 + отчёты + S3 из каталога сессии.

---

## Установка на Pi

```bash
git clone https://github.com/k1l1b1l1/LocalPC ~/LocalPC
cd ~/LocalPC

sudo apt install -y cmake g++ curl openssl nlohmann-json3-dev
chmod +x runtimepc/install.sh offline/install.sh runtimepc/run.sh offline/run.sh scripts/install-pi.sh

# оба модуля: сборка + install
./scripts/install-pi.sh
```

## Wi-Fi autoconnect

If Pi must always come back to the stand Wi-Fi after disconnect/reboot, configure
OS-level Wi-Fi autoconnect once on the device:

```bash
cd ~/LocalPC
chmod +x scripts/setup-wifi.sh
sudo ./scripts/setup-wifi.sh --ssid 'YOUR_WIFI_SSID' --psk 'YOUR_WIFI_PASSWORD' --hostname LocalPC
```

For fixed stand addressing:

```bash
sudo ./scripts/setup-wifi.sh \
  --ssid 'YOUR_WIFI_SSID' \
  --psk 'YOUR_WIFI_PASSWORD' \
  --hostname LocalPC \
  --static-ip 192.168.1.111/24 \
  --gateway 192.168.1.1 \
  --dns 192.168.1.1,8.8.8.8
```

Recovery notes: see `WIFI_RECOVERY.md`.

## Tailscale reconnect

Wi-Fi reconnect and Tailscale reconnect are separate layers. After Tailscale is
installed and the Pi has been authorized once, enable the watchdog:

```bash
cd ~/LocalPC
chmod +x scripts/setup-tailscale-watchdog.sh
sudo ./scripts/setup-tailscale-watchdog.sh --hostname localpc
```

Check:

```bash
systemctl status tailscaled tailscale-reconnect.timer
journalctl -u tailscale-reconnect.service -n 50 --no-pager
tailscale status
```

If `tailscale status --json` shows `BackendState=NeedsLogin`, run one manual
authorization on the Pi:

```bash
sudo tailscale up --hostname localpc
```

Then the watchdog will handle normal reconnects after network/power
interruptions. Details: see `TAILSCALE_RECOVERY.md`.

Или по отдельности:

```bash
cd runtimepc
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
./install.sh

cd ../offline
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
./install.sh
# S3: положить config/s3.local.yaml и снова ./install.sh
```

`install.sh` создаёт внутри каждого модуля:

| runtimepc | offline |
|-----------|---------|
| `bin/ego-runtime` | `bin/ego-offline` |
| `etc/config.yaml` | `etc/config.yaml` |
| `var/sessions/` | `etc/s3.local.yaml` |

---

## Запуск

```bash
cd ~/LocalPC/runtimepc
./run.sh status         # systemd-демон должен быть already running
./run.sh start          # запись
./run.sh stop           # стоп → finalize → offline (detached) → S3
```

После `./run.sh stop` runtime **синхронно** завершает сессию и запускает `ego-offline process` на Pi; загрузка в S3 идёт в фоне на устройстве (GUI на PC не ждёт S3).

Autostart (штатный режим):

```bash
sudo cp var/ego-runtime.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now ego-runtime
```

---

## Данные

| Путь | Что |
|------|-----|
| `runtimepc/var/sessions/session-.../` | сырой лог |
| `.../offline/` | MDF4 |
| S3 | после stop, автоматически |

Перед первым запуском требуется изменить `runtimepc/etc/config.yaml` — параметр `network.ego_host` (IP отправителя).
