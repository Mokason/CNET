#!/usr/bin/env bash
# A: baseline observation snapshot — call once now, again later to compare.
# Writes logs/personal_ai_observe_<ts>.json and prints a short summary.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${1:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
mkdir -p "$REPO/logs"
TS=$(date +%Y%m%dT%H%M%S)
OUT="$REPO/logs/personal_ai_observe_${TS}.json"
bash "$REPO/scripts/personal_ai_metrics.sh" "$BASE" >"$OUT"
echo "observe: wrote $OUT"
if command -v python3 >/dev/null 2>&1; then
  python3 - <<PY
import json
from pathlib import Path
p = Path("$OUT")
d = json.loads(p.read_text())
print(f"  units_journal={d.get('units_journal')} inbox={d.get('inbox_lines')} "
      f"ledger={d.get('ledger_lines')} learner={d.get('learner_active')} "
      f"cnb_MiB={d.get('cnb_bytes',0)/1024/1024:.1f}")
pl = d.get("placement") or {}
if isinstance(pl, dict):
    print(f"  dual_safe={pl.get('dual_safe')} mem_avail_GiB="
          f"{(pl.get('mem_available') or 0)/1024**3:.1f} "
          f"prefer_warm_only={pl.get('prefer_warm_only')}")
print("  re-run later: scripts/personal_ai_observe.sh")
print("PERSONAL_AI_OBSERVE_OK")
PY
else
  cat "$OUT"
  echo "PERSONAL_AI_OBSERVE_OK"
fi
