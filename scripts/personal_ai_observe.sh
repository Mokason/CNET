#!/usr/bin/env bash
# A: baseline observation snapshot — call once now, again later to compare.
# Writes logs/personal_ai_observe_<ts>.json and prints a short summary.
set -euo pipefail
# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
BASE="${1:-$(cnet_default_base)}"
mkdir -p "$REPO/logs"
TS=$(date +%Y%m%dT%H%M%S)
OUT="$REPO/logs/personal_ai_observe_${TS}.json"
bash "$REPO/scripts/personal_ai_metrics.sh" "$BASE" >"$OUT"
echo "observe: wrote $OUT"
if [ -x "$REPO/scripts/personal_ai_hill_climb_report.sh" ]; then
  echo "--- hill-climb (7d) ---"
  bash "$REPO/scripts/personal_ai_hill_climb_report.sh" "$BASE" 7 2>/dev/null || true
fi
# Optional light Tier A sample (not a hard fail — observe stays cheap)
if [ -f "$BASE" ] && [ -x "$REPO/bin/serve_proof" ]; then
  echo "--- serve-proof sample (max 2) ---"
  "$REPO/bin/serve_proof" "$BASE" --max 2 2>/dev/null | \
    grep -E 'units_loaded=|SERVE_PROOF_|sample:' || true
fi
if command -v python3 >/dev/null 2>&1; then
  python3 - <<PY
import json
from pathlib import Path
p = Path("$OUT")
d = json.loads(p.read_text())
units = d.get("units") if d.get("units") is not None else d.get("units_journal")
print(f"  units={units} inbox={d.get('inbox_lines')} "
      f"ledger={d.get('ledger_lines')} learner={d.get('learner_active')} "
      f"cnb_MiB={d.get('cnb_bytes',0)/1024/1024:.1f}")
print(f"  json_toolcall={d.get('json_toolcall')} jtc_gaps={d.get('inbox_jtc_gaps')} "
      f"serve_mcp={d.get('serve_mcp')}")
if d.get("recycle_note"):
    print(f"  note: {d.get('recycle_note')}")
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
