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
if command -v jq >/dev/null 2>&1 && jq -e . "$OUT" >/dev/null 2>&1; then
  jq -r '
    (.units // .units_journal) as $u |
    "  units=\($u) inbox=\(.inbox_lines) ledger=\(.ledger_lines) learner=\(.learner_active) cnb_MiB=\(((.cnb_bytes//0)/1048576*10|floor)/10)",
    "  json_toolcall=\(.json_toolcall) jtc_gaps=\(.inbox_jtc_gaps) serve_mcp=\(.serve_mcp)",
    (if .recycle_note then "  note: \(.recycle_note)" else empty end),
    ((.placement // {}) as $pl |
      if ($pl|type)=="object" then
        "  dual_safe=\($pl.dual_safe) mem_avail_GiB=\(((($pl.mem_available//0)/1073741824)*10|floor)/10) prefer_warm_only=\($pl.prefer_warm_only)"
      else empty end),
    "  re-run later: scripts/personal_ai_observe.sh",
    "PERSONAL_AI_OBSERVE_OK"
  ' "$OUT"
else
  cat "$OUT"
  echo "PERSONAL_AI_OBSERVE_OK"
fi
