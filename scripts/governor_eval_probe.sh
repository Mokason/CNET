#!/usr/bin/env bash
# Bounded eval probe for governor scoreboard (independent of teacher labels).
# Writes logs/governor/eval_probe.json
set -euo pipefail
ROOT="${CNET_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
DIR="${CNET_GOVERNOR_DIR:-$ROOT/logs/governor}"
OUT="$DIR/eval_probe.json"
mkdir -p "$DIR"
cd "$ROOT"

jtc_delta="null"
jtc_ok=0
# The bench synthesises ~211 mirror faults per run via registry_record_fault().
# Left on the shared bus those writes land in logs/cnet_faults.jsonl — the very
# file miss_ingest measures — so the instrument was contaminating its own input
# (1411 -> 4385 lines in two hours, 100% synthetic). Give it a scratch bus.
EVAL_FAULTS="$DIR/eval_faults.jsonl"
: >"$EVAL_FAULTS"
if [[ -x bin/jtc_adapter_bench ]]; then
  # short timeout; bench is hermetic
  if timeout 180 env CNET_PROMOTE_EVAL_DELTA="$DIR/ghost_eval_delta.txt" \
      CNET_FAULT_LOG="$EVAL_FAULTS" \
      ./bin/jtc_adapter_bench >"$DIR/jtc_probe.log" 2>&1; then
    jtc_ok=1
    # parse Δ from log (awk/grep — was python3)
    jtc_delta=$(
      sed -nE 's/.*[Dd]elta[=:[:space:]]+([+-]?[0-9]+\.?[0-9]*).*/\1/p' "$DIR/jtc_probe.log" | head -1
    )
    if [[ -z "${jtc_delta:-}" ]]; then
      jtc_delta=$(
        sed -nE 's/.*Δ[[:space:]]*=[[:space:]]*([+-]?[0-9]+\.?[0-9]*).*/\1/p' "$DIR/jtc_probe.log" | head -1
      )
    fi
    if [[ -z "${jtc_delta:-}" ]]; then
      on=$(sed -nE 's/.*acc_on[^0-9]*([0-9]+\.[0-9]+).*/\1/p' "$DIR/jtc_probe.log" | head -1)
      off=$(sed -nE 's/.*acc_off[^0-9]*([0-9]+\.[0-9]+).*/\1/p' "$DIR/jtc_probe.log" | head -1)
      if [[ -n "$on" && -n "$off" ]]; then
        jtc_delta=$(awk -v a="$on" -v b="$off" 'BEGIN{printf "%.4f", a-b}')
      fi
    fi
    [[ -z "${jtc_delta:-}" ]] && jtc_delta="null"
  fi
fi

# procedure chunk presence (byte markers in base)
proc_n=0
if [[ -f soul_gemma4v2_final.cnb ]]; then
  # grep -a -o counts overlapping poorly; use tr|grep for marker hits
  c1=$(grep -ao 'chunk_' soul_gemma4v2_final.cnb 2>/dev/null | wc -l | tr -d ' ')
  c2=$(grep -ao 'procedure' soul_gemma4v2_final.cnb 2>/dev/null | wc -l | tr -d ' ')
  proc_n=$(( ${c1:-0} + ${c2:-0} ))
fi

# Seal rejects SINCE THE LAST PROBE. Grepping the whole append-only muscle.log
# returned a cumulative total (8 -> 9 -> 10 -> 11 ...) that was reported as a
# current level, so it could only ever rise. Track a byte offset instead.
reject=0
reject_total=0
MUSCLE="$DIR/muscle.log"
OFFT="$DIR/muscle_offset"
if [[ -f "$MUSCLE" ]]; then
  size=$(wc -c <"$MUSCLE" | tr -d ' ')
  off=0
  [[ -f "$OFFT" ]] && off=$(cat "$OFFT" 2>/dev/null || echo 0)
  # log rotated/truncated → restart from 0
  [[ "$off" -gt "$size" ]] && off=0
  reject=$(tail -c "+$((off + 1))" "$MUSCLE" 2>/dev/null \
    | grep -cE "reject|REJECT|certified=0" || true)
  reject_total=$(grep -cE "reject|REJECT|certified=0" "$MUSCLE" 2>/dev/null || true)
  echo "$size" >"$OFFT"
fi
reject="${reject:-0}"
reject_total="${reject_total:-0}"

# Bench detail: cert_regress is the signal that actually moves when behaviour
# changes; eval_jtc_delta is fixed by construction (hermetic synthetic set).
cert_fixes=0
cert_regress=0
acc_on=0
acc_off=0
if [[ -f "$DIR/jtc_probe.log" ]]; then
  line=$(grep -m1 "JTC_ADAPTER_BENCH_PASS" "$DIR/jtc_probe.log" 2>/dev/null || true)
  if [[ -n "$line" ]]; then
    cert_fixes=$(sed -n 's/.*cert_fixes=\([0-9-]*\).*/\1/p' <<<"$line")
    cert_regress=$(sed -n 's/.*cert_regress=\([0-9-]*\).*/\1/p' <<<"$line")
    acc_on=$(sed -n 's/.*acc_on=\([0-9.]*\).*/\1/p' <<<"$line")
    acc_off=$(sed -n 's/.*acc_off=\([0-9.]*\).*/\1/p' <<<"$line")
  fi
fi
cert_fixes=${cert_fixes:-0}
cert_regress=${cert_regress:-0}
acc_on=${acc_on:-0}
acc_off=${acc_off:-0}

ts=$(date +%Y-%m-%dT%H:%M:%S%z)
prev_delta="null"
if [[ -f "$OUT" ]]; then
  prev_delta=$(jq -r '.eval_jtc_delta // "null"' "$OUT" 2>/dev/null || echo null)
fi

if [[ "$jtc_delta" != "null" && "$prev_delta" != "null" && -n "$prev_delta" ]]; then
  d_eval=$(awk -v a="$jtc_delta" -v b="$prev_delta" 'BEGIN { printf "%.4f", a-b }')
  d_eval_json="$d_eval"
else
  d_eval_json="null"
fi

delta_json="$jtc_delta"

jq -n \
  --arg ts "$ts" \
  --argjson jtc_probe_ok "$jtc_ok" \
  --argjson eval_jtc_delta "$delta_json" \
  --argjson acc_on "$acc_on" \
  --argjson acc_off "$acc_off" \
  --argjson cert_fixes "$cert_fixes" \
  --argjson cert_regress "$cert_regress" \
  --argjson procedure_markers "$proc_n" \
  --argjson seal_reject_new "$reject" \
  --argjson seal_reject_total "$reject_total" \
  --argjson d_eval_jtc "$d_eval_json" \
  '{
    ts: $ts,
    jtc_probe_ok: $jtc_probe_ok,
    eval_jtc_delta: $eval_jtc_delta,
    eval_source: "synthetic_fixed_set",
    acc_on: $acc_on,
    acc_off: $acc_off,
    cert_fixes: $cert_fixes,
    cert_regress: $cert_regress,
    procedure_markers: $procedure_markers,
    seal_reject_new: $seal_reject_new,
    seal_reject_total: $seal_reject_total,
    d_eval_jtc: $d_eval_jtc
  }' >"$OUT"

if [[ "$jtc_delta" != "null" ]]; then
  printf '%s\n' "$jtc_delta" >"$DIR/ghost_eval_delta.txt"
fi
echo "EVAL_PROBE_OK $(jq -c . "$OUT")"
