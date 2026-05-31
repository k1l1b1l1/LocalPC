#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
ARGS=()
if [[ -f "$ROOT/etc/config.yaml" ]]; then
  ARGS+=(--config "$ROOT/etc/config.yaml")
fi
if [[ -f "$ROOT/etc/s3.local.yaml" ]]; then
  ARGS+=(--s3-config "$ROOT/etc/s3.local.yaml")
fi
exec "$ROOT/bin/ego-offline" "$@" "${ARGS[@]}"
