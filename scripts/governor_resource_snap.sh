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

# day/night: hour
hour=$(date +%H)
night=0
if [[ "$hour" -ge 23 || "$hour" -lt 7 ]]; then night=1; fi

# busy if load high or mem low
busy=0
python3 - <<PY
import json, time
from pathlib import Path
load=float("$load")
mem_kb=int("$mem_avail_kb")
gpu=int("$gpu0" or 0)
busy = 1 if (load >= 12.0 or mem_kb < 2_000_000 or gpu >= 85) else 0
night=int("$night")
# heavy teach preferred at night or when not busy
allow_heavy = 1 if (night==1 or busy==0) else 0
rep={
  "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
  "load1": load,
  "mem_avail_kb": mem_kb,
  "gpu0_use": gpu,
  "hermes": "$hermes",
  "bonsai": "$bonsai",
  "lane": "$lane",
  "night": night,
  "busy": busy,
  "allow_heavy": allow_heavy,
}
Path("$OUT").write_text(json.dumps(rep, indent=2)+"\n")
print("RESOURCE_SNAP_OK", json.dumps(rep))
PY
