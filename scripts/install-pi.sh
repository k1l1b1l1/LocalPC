#!/usr/bin/env bash
# Build + local install both modules on Raspberry Pi.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

echo "== runtimepc =="
cd "$ROOT/runtimepc"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
bash install.sh

echo ""
echo "== offline =="
cd "$ROOT/offline"
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
bash install.sh

echo ""
echo "All installed under module directories."
echo "Start: cd $ROOT/runtimepc && ./run.sh run &"
echo "       ./run.sh start"
