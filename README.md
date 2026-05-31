# RPi5

```
SC589 / PC  →  runtimepc  →  offline  →  S3
```

## runtimepc

TCP-клиент `:5001`, пишет сессию в `runtimepc/var/sessions/`. После stop запускает offline.

## offline

MDF4 + отчёты + S3 из каталога сессии.

---

## Установка на Pi

```bash
git clone https://github.com/k1l1b1l1/LocalPC/tree/main ~/LocalPC
cd ~/LocalPC

sudo apt install -y cmake g++ curl openssl nlohmann-json3-dev
chmod +x runtimepc/install.sh offline/install.sh runtimepc/run.sh offline/run.sh scripts/install-pi.sh

# оба модуля: сборка + install
./scripts/install-pi.sh
```

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
./run.sh run &          # демон
./run.sh start          # запись
./run.sh stop           # стоп → offline → S3
```

Autostart (опционально):

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
