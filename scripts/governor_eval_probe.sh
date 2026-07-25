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
    # parse Δ from log
    jtc_delta=$(python3 - <<'PY'
import re
from pathlib import Path
t=Path("logs/governor/jtc_probe.log").read_text(errors="replace")
# common patterns
m=re.search(r"delta[=:\s]+([+-]?\d+\.?\d*)", t, re.I)
if not m:
    m=re.search(r"Δ\s*=\s*([+-]?\d+\.?\d*)", t)
if not m:
    m=re.search(r"acc_on[^\d]+(\d+\.\d+).*acc_off[^\d]+(\d+\.\d+)", t, re.S)
    if m:
        print(round(float(m.group(1))-float(m.group(2)), 4))
    else:
        print("null")
else:
    print(m.group(1))
PY
)
  fi
fi

# procedure chunk presence
proc_n=$(python3 - <<'PY'
from pathlib import Path
b=Path("soul_gemma4v2_final.cnb")
if not b.exists():
    print(0)
else:
    data=b.read_bytes()
    print(data.count(b"chunk_")+data.count(b"procedure"))
PY
)

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

python3 - <<PY
import json, time
from pathlib import Path
out=Path("$OUT")
delta=$jtc_delta if "$jtc_delta" != "null" else None
try:
    delta=float("$jtc_delta") if "$jtc_delta"!="null" else None
except Exception:
    delta=None
def _i(s, d=0):
    try:
        return int(str(s).strip() or d)
    except Exception:
        return d

def _f(s, d=0.0):
    try:
        return float(str(s).strip() or d)
    except Exception:
        return d

rep={
  "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
  "jtc_probe_ok": int("$jtc_ok"),
  "eval_jtc_delta": delta,
  # Hermetic bench over a FIXED synthetic fault set: the delta is a constant by
  # construction, so it measures adapter lift on that set, never progress.
  "eval_source": "synthetic_fixed_set",
  "acc_on": _f("$acc_on"),
  "acc_off": _f("$acc_off"),
  "cert_fixes": _i("$cert_fixes"),
  "cert_regress": _i("$cert_regress"),
  "procedure_markers": int("$proc_n"),
  "seal_reject_new": _i("$reject"),
  "seal_reject_total": _i("$reject_total"),
}
# load previous for d_eval
prev_p=out
prev_delta=None
if prev_p.exists():
    try:
        prev=json.loads(prev_p.read_text())
        prev_delta=prev.get("eval_jtc_delta")
    except Exception:
        pass
if delta is not None and prev_delta is not None:
    rep["d_eval_jtc"]=round(delta-float(prev_delta),4)
else:
    rep["d_eval_jtc"]=None
out.write_text(json.dumps(rep, indent=2)+"\n")
# also write promote delta file if we have a number
if delta is not None:
    Path("$DIR/ghost_eval_delta.txt").write_text(f"{delta}\\n")
print("EVAL_PROBE_OK", json.dumps(rep))
PY
