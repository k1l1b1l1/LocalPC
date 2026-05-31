#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
export EGO_RUNTIME_ROOT="$ROOT"
exec "$ROOT/bin/ego-runtime" --config "$ROOT/etc/config.yaml" "$@"
