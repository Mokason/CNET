#!/usr/bin/env bash
# Install/sync CORE .lut bricks into the live bank (CNET_CORE_BUS_BRICKS_DIR).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SEED="${CNET_CORE_BRICKS_SEED:-$REPO/config/core_bricks_seed}"
LIVE="${CNET_CORE_BUS_BRICKS_DIR:-$HOME/.local/share/cnet-bricks}"
DIR_CONF_SRC="${CNET_EVOLVE_DIRECTION_SRC:-$REPO/config/cnet_evolve_direction.conf}"

mkdir -p "$LIVE"
n=0
shopt -s nullglob
for f in "$SEED"/*.lut; do
  base="$(basename "$f")"
  cp -f "$f" "$LIVE/$base"
  n=$((n+1))
done
if [[ -f "$DIR_CONF_SRC" && ! -f "$LIVE/evolve_direction.conf" ]]; then
  cp -f "$DIR_CONF_SRC" "$LIVE/evolve_direction.conf"
  echo "cnet_core_bricks_wire: installed evolve_direction.conf"
elif [[ -f "$DIR_CONF_SRC" && "${CNET_CORE_BRICKS_FORCE_DIRCONF:-0}" == "1" ]]; then
  cp -f "$DIR_CONF_SRC" "$LIVE/evolve_direction.conf"
  echo "cnet_core_bricks_wire: refreshed evolve_direction.conf"
fi
# If live still has no direction conf but repo config exists under bricks-shaped name
if [[ ! -f "$LIVE/evolve_direction.conf" && -f "$REPO/config/cnet_evolve_direction.conf" ]]; then
  cp -f "$REPO/config/cnet_evolve_direction.conf" "$LIVE/evolve_direction.conf"
fi
echo "cnet_core_bricks_wire: seed=$SEED live=$LIVE luts=$n"
ls -la "$LIVE"
# quick parse check
python3 - <<PY
from pathlib import Path
live=Path("$LIVE")
luts=sorted(live.glob("*.lut"))
ok=0
for p in luts:
    t=p.read_text()
    if "tag=" in t and "lut=" in t:
        ok+=1
    else:
        print("BAD", p)
print(f"parse_ok={ok}/{len(luts)}")
raise SystemExit(0 if ok==len(luts) and ok>0 else 1)
PY
echo "CNET_CORE_BRICKS_WIRE_OK"
