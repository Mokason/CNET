#!/usr/bin/env bash
# Resource snapshot for governor (Hermes/Bonsai contention).
set -euo pipefail
ROOT="${CNET_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
DIR="${CNET_GOVERNOR_DIR:-$ROOT/logs/governor}"
OUT="$DIR/resource_snap.json"
mkdir -p "$DIR"

mem_avail_kb=$(awk '/MemAvailable:/ {print $2}' /proc/meminfo)
load=$(cut -d' ' -f1 /proc/loadavg)
hermes=$(systemctl --user is-active hermes-gateway.service 2>/dev/null || echo inactive)
bonsai=$(systemctl --user is-active bonsai-server.service 2>/dev/null || echo inactive)
lane=$(systemctl --user is-active cnet-personal-ai-lane.service 2>/dev/null || echo inactive)

# GPU busy proxy: rocm-smi if present
gpu0=0
if command -v rocm-smi >/dev/null 2>&1; then
  gpu0=$(rocm-smi --showuse 2>/dev/null | awk '/GPU.use/ {print $NF; exit}' | tr -dc '0-9' || echo 0)
fi
[[ -z "$gpu0" ]] && gpu0=0

hour=$(date +%H)
night=0
if [[ "$hour" -ge 23 || "$hour" -lt 7 ]]; then night=1; fi

busy=0
awk -v load="$load" -v mem_kb="$mem_avail_kb" -v gpu="$gpu0" 'BEGIN {
  if (load+0 >= 12.0 || mem_kb+0 < 2000000 || gpu+0 >= 85) exit 0
  exit 1
}' && busy=1

allow_heavy=0
if [[ "$night" -eq 1 || "$busy" -eq 0 ]]; then allow_heavy=1; fi

ts=$(date +%Y-%m-%dT%H:%M:%S%z)
jq -n \
  --arg ts "$ts" \
  --argjson load1 "$load" \
  --argjson mem_avail_kb "$mem_avail_kb" \
  --argjson gpu0_use "$gpu0" \
  --arg hermes "$hermes" \
  --arg bonsai "$bonsai" \
  --arg lane "$lane" \
  --argjson night "$night" \
  --argjson busy "$busy" \
  --argjson allow_heavy "$allow_heavy" \
  '{
    ts: $ts,
    load1: $load1,
    mem_avail_kb: $mem_avail_kb,
    gpu0_use: $gpu0_use,
    hermes: $hermes,
    bonsai: $bonsai,
    lane: $lane,
    night: $night,
    busy: $busy,
    allow_heavy: $allow_heavy
  }' >"$OUT"
echo "RESOURCE_SNAP_OK $(cat "$OUT" | jq -c .)"
