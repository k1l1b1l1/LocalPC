#!/usr/bin/env bash
# Build + local install both modules on Raspberry Pi.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"

if ! command -v protoc >/dev/null 2>&1; then
  echo "error: protoc not found. Install: sudo apt install -y libprotobuf-dev protobuf-compiler"
  exit 1
fi

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
