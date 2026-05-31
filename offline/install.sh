#!/usr/bin/env bash
# Local install: dirs + binary + config inside offline/ (no /usr/local, no /etc).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN="$ROOT/bin"
ETC="$ROOT/etc"

echo "[offline] install -> $ROOT"

mkdir -p "$BIN" "$ETC"

if [[ ! -x "$ROOT/build/ego-offline" ]]; then
  echo "error: build/ego-offline not found. Run:"
  echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"
  exit 1
fi

cp "$ROOT/build/ego-offline" "$BIN/ego-offline"
chmod +x "$BIN/ego-offline"

cp "$ROOT/config/board.yaml" "$ETC/config.yaml"

if [[ -f "$ROOT/config/s3.local.yaml" ]]; then
  cp "$ROOT/config/s3.local.yaml" "$ETC/s3.local.yaml"
  chmod 600 "$ETC/s3.local.yaml"
  echo "  s3:      $ETC/s3.local.yaml (from config/s3.local.yaml)"
else
  echo "note: config/s3.local.yaml not found — copy secrets to $ETC/s3.local.yaml for S3 upload"
  echo "      template: config/s3.yaml.example"
fi

chmod +x "$ROOT/run.sh" 2>/dev/null || true

echo "done."
echo "  config:  $ETC/config.yaml"
echo "  binary:  $BIN/ego-offline"
echo ""
echo "manual run (usually started by runtime after stop):"
echo "  ./run.sh process --session-dir /path/to/session"
