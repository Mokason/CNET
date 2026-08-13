#!/usr/bin/env bash
# Ingest real misses from fault bus + gap ledger → logs/governor/miss_bus.json
set -euo pipefail
ROOT="${CNET_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
BASE="${CNET_BASE_PATH:-$ROOT/soul_gemma4v2_final.cnb}"
FAULT="${CNET_FAULT_LOG:-$ROOT/logs/cnet_faults.jsonl}"
GAPS="${BASE}.gaps.txt"
OUT="${CNET_GOVERNOR_DIR:-$ROOT/logs/governor}/miss_bus.json"
mkdir -p "$(dirname "$OUT")"

total=0
jtc=0
units_tmp=$(mktemp)
trap 'rm -f "$units_tmp"' EXIT

if [[ -f "$FAULT" ]]; then
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -z "$line" ]] && continue
    u=$(jq -r '.unit // .tag // "unknown"' <<<"$line" 2>/dev/null) || continue
    total=$((total + 1))
    echo "$u" >>"$units_tmp"
    ul=$(printf '%s' "$u" | tr '[:upper:]' '[:lower:]')
    if [[ "$ul" == *jtc* || "$ul" == *json_tool* ]]; then
      jtc=$((jtc + 1))
    fi
  done <"$FAULT"
fi

# A waiting_oracle row is an OPEN row carrying an annotation, not a third
# state (verified against the live ledger: every waiting row has state 1).
# Counting both independently double-counted them; bin/governor_autonomous
# instead partitions outstanding rows into waiting XOR open, so mirror that or
# backlog_pressure and real_miss_rate silently disagree about the same ledger.
waiting=0
open_n=0
closed_n=0
if [[ -f "$GAPS" ]]; then
  lineno=0
  while IFS= read -r line || [[ -n "$line" ]]; do
    lineno=$((lineno + 1))
    [[ "$lineno" -lt 3 ]] && continue
    # shellcheck disable=SC2206
    parts=($line)
    state="${parts[1]:-}"
    if [[ "$line" == *waiting_oracle* || "$line" == *waiting_charter* ]]; then
      waiting=$((waiting + 1))
    elif [[ "$state" == "1" ]]; then
      open_n=$((open_n + 1))
    elif [[ "$state" == "2" ]]; then
      closed_n=$((closed_n + 1))
    fi
  done <"$GAPS"
fi

# real_miss_rate: share of the ledger still outstanding.
# The old denominator was (waiting + open + 1) — it excluded every closed gap,
# so the rate could never fall below ~0.3 no matter how well the lane drained,
# and real_miss_cut permanently dominated project ranking. Successes must be in
# the denominator for this to be a rate at which 0 means "healthy".
outstanding=$((waiting + open_n))
denom=$((outstanding + closed_n))
[[ "$denom" -lt 1 ]] && denom=1
real_miss_rate=$(awk -v o="$outstanding" -v d="$denom" 'BEGIN {
  r = o / d; if (r > 1) r = 1; printf "%.4f", r
}')

top_json='[]'
if [[ -s "$units_tmp" ]]; then
  top_json=$(
    sort "$units_tmp" | uniq -c | sort -nr | head -12 \
      | while read -r count unit; do
          jq -n --arg unit "$unit" --argjson n "$count" '{unit: $unit, n: $n}'
        done | jq -s .
  )
fi

ts=$(date +%Y-%m-%dT%H:%M:%S%z)
formula='(waiting_oracle + open_gaps) / (waiting_oracle + open_gaps + closed_gaps); waiting XOR open partition the outstanding set'
jq -n \
  --arg ts "$ts" \
  --argjson fault_lines "$total" \
  --argjson jtc_faults "$jtc" \
  --argjson waiting_oracle "$waiting" \
  --argjson open_gaps "$open_n" \
  --argjson closed_gaps "$closed_n" \
  --argjson real_miss_rate "$real_miss_rate" \
  --arg real_miss_formula "$formula" \
  --argjson top_units "$top_json" \
  '{
    ts: $ts,
    fault_lines: $fault_lines,
    jtc_faults: $jtc_faults,
    waiting_oracle: $waiting_oracle,
    open_gaps: $open_gaps,
    closed_gaps: $closed_gaps,
    real_miss_rate: $real_miss_rate,
    real_miss_formula: $real_miss_formula,
    top_units: $top_units
  }' >"$OUT"

echo "MISS_BUS_OK $(jq -c '{fault_lines, waiting: .waiting_oracle, real_miss_rate}' "$OUT")"
