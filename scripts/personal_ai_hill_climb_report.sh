#!/usr/bin/env bash
# Weekly (or custom-window) hill-climb report for personal AI local EG.
#
# Local EG = baseline_cost_per_seal / cost_per_seal
#   cost_per_seal = teacher_work (examined) / max(seals, 1)
#   baseline default 32 (one min_evidence-ish teach batch per seal)
#
# Usage:
#   scripts/personal_ai_hill_climb_report.sh [base.cnb] [days=7]
# Env:
#   CNET_EG_BASELINE_COST=32
#   CNET_EG_LOG=<base>.hill_climb.jsonl
set -euo pipefail
# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
cnet_load_personal_env
BASE="${1:-$(cnet_default_base)}"
DAYS="${2:-7}"
LOG="${CNET_EG_LOG:-${BASE}.hill_climb.jsonl}"
BASELINE="${CNET_EG_BASELINE_COST:-32}"
NOW=$(date +%s)
T0=$((NOW - DAYS * 86400))
mkdir -p "$REPO/logs"

if [ ! -x "$REPO/bin/cnet_eg" ]; then
  make -C "$REPO" cnet_eg_cli -j"$(cnet_nproc)" >/dev/null 2>&1 || true
fi

echo "=== CNET hill-climb report (last ${DAYS}d) ==="
echo "base: $BASE"
echo "log:  $LOG"
echo "baseline_cost_per_seal: $BASELINE"

if [ ! -f "$LOG" ]; then
  echo "No EG log yet — learner writes on each did_work tick after rebuild."
  echo "Hint: leave lane running; or seed: scripts/personal_ai_grow_local.sh 8"
  if command -v journalctl >/dev/null 2>&1; then
    echo "--- journal fallback (approx) ---"
    mapfile -t JLINES < <(journalctl --user -u cnet-personal-ai-lane.service \
      --since "${DAYS} days ago" --no-pager 2>/dev/null || true)
    closed=0
    drained=0
    units=0
    for line in "${JLINES[@]:-}"; do
      if [[ "$line" =~ closed=([0-9]+) ]]; then
        closed=$((closed + BASH_REMATCH[1]))
      fi
      if [[ "$line" =~ drained=([0-9]+) ]]; then
        drained=$((drained + BASH_REMATCH[1]))
      fi
      if [[ "$line" =~ units=([0-9]+) ]]; then
        units=${BASH_REMATCH[1]}
      fi
    done
    echo "  journal_closed_sum=$closed journal_drained_sum=$drained last_units=$units"
    if [ "${closed:-0}" -gt 0 ] 2>/dev/null; then
      cps=$(awk -v d="$drained" -v c="$closed" 'BEGIN{den=(c>1?c:1); printf "%.4f", d/den}')
      eg=$(awk -v b="$BASELINE" -v d="$drained" -v c="$closed" 'BEGIN{
        den=(c>1?c:1); cps=d/den; if (cps<1e-9) cps=1e-9; printf "%.4f", b/cps
      }')
      echo "  approx_cost_per_seal=$cps  approx_local_eg=$eg"
      echo "  (EG>1 = better than baseline $BASELINE examined/seal)"
    fi
  fi
  echo "HILL_CLIMB_REPORT_OK partial=1"
  exit 0
fi

if [ -x "$REPO/bin/cnet_eg" ]; then
  "$REPO/bin/cnet_eg" report --log "$LOG" --since "$T0" --baseline "$BASELINE" | \
    tee "$REPO/logs/hill_climb_report.txt"
  if ! grep -q "HILL_CLIMB_REPORT_OK" "$REPO/logs/hill_climb_report.txt" 2>/dev/null; then
    echo "HILL_CLIMB_REPORT_OK"
  fi
  exit 0
fi

# jq fallback when bin/cnet_eg is unavailable
agg=$(jq -s --argjson t0 "$T0" --argjson t1 "$NOW" --argjson base "$BASELINE" --argjson days "$DAYS" '
  map(select((.ts|tonumber) >= $t0 and (.ts|tonumber) <= $t1)) as $rows |
  {
    teacher_work: ($rows | map(.examined//0) | add // 0),
    seals: ($rows | map(.closed//0) | add // 0),
    deferred: ($rows | map(.deferred//0) | add // 0),
    no_oracle: ($rows | map(.no_oracle//0) | add // 0),
    curiosity: ($rows | map(.curiosity//0) | add // 0),
    units: ($rows | map(.units//0) | max // 0),
    first: ($rows | map(.ts|tonumber) | min),
    last: ($rows | map(.ts|tonumber) | max),
    baseline_cost: $base,
    days: $days
  } |
  .hours = (if .first and .last then ((.last-.first)/3600.0) else 1e-9 end) |
  if .hours < 1e-9 then .hours = 1e-9 else . end |
  .cost_per_seal = (.teacher_work / (if .seals > 0 then .seals else 1 end)) |
  .seal_rate = (.seals / .hours) |
  .local_eg = ($base / (if .cost_per_seal > 1e-9 then .cost_per_seal else 1e-9 end)) |
  del(.first, .last)
' "$LOG")

jq -r '
  "  teacher_work(examined)=\(.teacher_work)",
  "  seals(closed)=\(.seals)",
  "  deferred=\(.deferred) no_oracle=\(.no_oracle) curiosity=\(.curiosity)",
  "  units_end=\(.units) hours=\((.hours*1000|floor)/1000)",
  "  cost_per_seal=\((.cost_per_seal*10000|floor)/10000)  (lower better)",
  "  seal_rate=\((.seal_rate*10000|floor)/10000)/h",
  "  local_eg=\((.local_eg*10000|floor)/10000)  (>1 better than baseline \(.baseline_cost))",
  (if .local_eg > 1 then
     "  trend: climbing — \((.local_eg*100|floor)/100)× more efficient than baseline"
   elif .seals == 0 then
     "  trend: no seals in window — feed teachable gaps or wait for curiosity"
   else
     "  trend: below baseline — each seal costs more teach work than \(.baseline_cost)"
   end)
' <<<"$agg"
printf '%s\n' "$agg" | jq . >"$REPO/logs/hill_climb_report.json"
echo "HILL_CLIMB_REPORT_OK"
