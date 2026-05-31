#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
ARGS=(--config "$ROOT/etc/config.yaml")
if [[ -f "$ROOT/etc/s3.local.yaml" ]]; then
  ARGS+=(--s3-config "$ROOT/etc/s3.local.yaml")
fi
exec "$ROOT/bin/ego-offline" "${ARGS[@]}" "$@"
