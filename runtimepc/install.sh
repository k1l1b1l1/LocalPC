#!/usr/bin/env bash
# Local install: dirs + binary + config inside runtimepc/ (no /usr/local, no /etc).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$ROOT/.." && pwd)"
OFFLINE_ROOT="$REPO_ROOT/offline"

BIN="$ROOT/bin"
ETC="$ROOT/etc"
VAR="$ROOT/var"
SESSIONS="$VAR/sessions"
RUN="$VAR/run"

echo "[runtimepc] install -> $ROOT"

mkdir -p "$BIN" "$ETC" "$SESSIONS" "$RUN" "$VAR/logs"

if [[ ! -x "$ROOT/build/ego-runtime" ]]; then
  echo "error: build/ego-runtime not found. Run:"
  echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"
  exit 1
fi

pkill -f '[/]bin/ego-runtime' 2>/dev/null || true
sleep 0.3

cp "$ROOT/build/ego-runtime" "$BIN/ego-runtime"
chmod +x "$BIN/ego-runtime"

cp "$ROOT/config/board.yaml" "$ETC/config.yaml"

# Absolute paths for this deployment tree
sed -i "s|^  data_root:.*|  data_root: \"${SESSIONS}\"|" "$ETC/config.yaml"
sed -i "s|^  binary:.*|  binary: \"${OFFLINE_ROOT}/bin/ego-offline\"|" "$ETC/config.yaml"
sed -i "s|^  config:.*|  config: \"${OFFLINE_ROOT}/etc/config.yaml\"|" "$ETC/config.yaml"
sed -i "s|^  s3_config:.*|  s3_config: \"${OFFLINE_ROOT}/etc/s3.local.yaml\"|" "$ETC/config.yaml"

chmod +x "$ROOT/run.sh" 2>/dev/null || true

# systemd unit with local paths (optional)
cat > "$VAR/ego-runtime.service" <<EOF
[Unit]
Description=EGO runtime recorder (Raspberry Pi 5)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
WorkingDirectory=${ROOT}
Environment=EGO_RUNTIME_ROOT=${ROOT}
ExecStart=${BIN}/ego-runtime run --config ${ETC}/config.yaml
Restart=on-failure
RestartSec=5
StartLimitIntervalSec=60
StartLimitBurst=3

[Install]
WantedBy=multi-user.target
EOF

if [[ ! -x "$OFFLINE_ROOT/bin/ego-offline" ]]; then
  echo "note: offline not installed yet — run ../offline/install.sh after building offline"
fi

echo "done."
echo "  config:  $ETC/config.yaml"
echo "  data:    $SESSIONS"
echo "  binary:  $BIN/ego-runtime"
echo ""
echo "run:"
echo "  cd $ROOT && ./run.sh run &"
echo "  ./run.sh start"
echo "  ./run.sh stop"
echo ""
echo "systemd (optional):"
echo "  sudo cp $VAR/ego-runtime.service /etc/systemd/system/"
echo "  sudo systemctl daemon-reload && sudo systemctl enable --now ego-runtime"
