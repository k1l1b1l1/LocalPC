#!/usr/bin/env bash
# Build + local install both modules on Raspberry Pi.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TAILSCALE_WATCHDOG="$ROOT/scripts/setup-tailscale-watchdog.sh"

if ! command -v protoc >/dev/null 2>&1; then
  echo "error: protoc not found. Install: sudo apt install -y libprotobuf-dev protobuf-compiler"
  exit 1
fi

if command -v systemctl >/dev/null 2>&1; then
  echo "== ego-runtime stop =="
  if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    systemctl stop ego-runtime 2>/dev/null || true
  elif sudo -n true >/dev/null 2>&1; then
    sudo systemctl stop ego-runtime 2>/dev/null || true
  else
    echo "systemd detected, but stop needs sudo:"
    echo "  sudo systemctl stop ego-runtime"
  fi
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
echo "runtime daemon control:"
echo "  cd $ROOT/runtimepc && ./run.sh status"
echo "  ./run.sh start"
echo "  ./run.sh stop"
echo ""

if command -v systemctl >/dev/null 2>&1; then
  echo "== ego-runtime systemd =="
  if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    install -m 0644 "$ROOT/runtimepc/var/ego-runtime.service" /etc/systemd/system/ego-runtime.service
    systemctl daemon-reload
    systemctl enable --now ego-runtime
    systemctl status ego-runtime --no-pager || true
  elif sudo -n true >/dev/null 2>&1; then
    sudo install -m 0644 "$ROOT/runtimepc/var/ego-runtime.service" /etc/systemd/system/ego-runtime.service
    sudo systemctl daemon-reload
    sudo systemctl enable --now ego-runtime
    sudo systemctl status ego-runtime --no-pager || true
  else
    echo "systemd detected, but enabling ego-runtime needs sudo:"
    echo "  sudo install -m 0644 $ROOT/runtimepc/var/ego-runtime.service /etc/systemd/system/ego-runtime.service"
    echo "  sudo systemctl daemon-reload"
    echo "  sudo systemctl enable --now ego-runtime"
  fi
else
  echo "systemctl not found; skipping ego-runtime autostart setup."
fi

if command -v tailscale >/dev/null 2>&1; then
  echo "== tailscale watchdog =="
  if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    bash "$TAILSCALE_WATCHDOG" --hostname localpc
  elif sudo -n true >/dev/null 2>&1; then
    sudo bash "$TAILSCALE_WATCHDOG" --hostname localpc
  else
    echo "Tailscale detected, but watchdog install needs sudo:"
    echo "  sudo $TAILSCALE_WATCHDOG --hostname localpc"
  fi
else
  echo "Tailscale not installed; skipping reconnect watchdog."
  echo "After Tailscale install, run:"
  echo "  sudo $TAILSCALE_WATCHDOG --hostname localpc"
fi
