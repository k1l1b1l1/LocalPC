#!/usr/bin/env bash
# Stand install: ego-offline with short-session config (min_valid_duration_s: 0.5).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BIN="$ROOT/bin"
ETC="$ROOT/etc"

echo "[offline] stand install -> $ROOT"

mkdir -p "$BIN" "$ETC"

if [[ ! -x "$ROOT/build/ego-offline" ]]; then
  echo "error: build/ego-offline not found. Run:"
  echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j\$(nproc)"
  exit 1
fi

cp "$ROOT/build/ego-offline" "$BIN/ego-offline"
chmod +x "$BIN/ego-offline"

if [[ -f "$ROOT/config/stand.pc.yaml" ]]; then
  cp "$ROOT/config/stand.pc.yaml" "$ETC/config.yaml"
else
  cp "$ROOT/config/board.yaml" "$ETC/config.yaml"
  sed -i "s/^  min_valid_duration_s:.*/  min_valid_duration_s: 0.5/" "$ETC/config.yaml"
  sed -i "s/^  fail_on_sync_error:.*/  fail_on_sync_error: false/" "$ETC/config.yaml"
  sed -i "s/^  fail_on_no_valid_ranges:.*/  fail_on_no_valid_ranges: false/" "$ETC/config.yaml"
fi

if [[ -f "$ROOT/config/s3.local.yaml" ]]; then
  cp "$ROOT/config/s3.local.yaml" "$ETC/s3.local.yaml"
  chmod 600 "$ETC/s3.local.yaml"
  echo "  s3:      $ETC/s3.local.yaml"
else
  echo "note: config/s3.local.yaml not found — copy secrets to $ETC/s3.local.yaml for S3"
fi

chmod +x "$ROOT/run.sh" 2>/dev/null || true

echo "done (stand config: min_valid_duration_s=0.5)."
