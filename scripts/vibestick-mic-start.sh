#!/usr/bin/env bash
# systemd 入口：先确保 VibeStick-AP 热点在跑，再前台运行 vibestick-mic.sh。
# 热点带重试——user 服务可能比 NetworkManager 就绪更早启动。
set -uo pipefail

AP="VibeStick-AP"

for i in $(seq 1 10); do
  active="$(nmcli -t -f NAME,DEVICE connection show --active 2>/dev/null | grep "^${AP}:" || true)"
  if [ -n "$active" ]; then
    break
  fi
  nmcli connection up "$AP" >/dev/null 2>&1 || true
  sleep 3
done

if ! nmcli -t -f NAME,DEVICE connection show --active 2>/dev/null | grep -q "^${AP}:"; then
  echo "WARN: hotspot ${AP} not active; receiver will still start" >&2
fi

exec bash "$(cd -- "$(dirname -- "$0")" && pwd)/vibestick-mic.sh"
