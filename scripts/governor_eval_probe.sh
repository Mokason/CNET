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
if [[ -x bin/jtc_adapter_bench ]]; then
  # short timeout; bench is hermetic
  if timeout 180 env CNET_PROMOTE_EVAL_DELTA="$DIR/ghost_eval_delta.txt" \
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

# seal reject proxy from last muscle log
reject=0
if [[ -f "$DIR/muscle.log" ]]; then
  reject=$(rg -c "reject|REJECT|certified=0" "$DIR/muscle.log" 2>/dev/null | tail -1 || echo 0)
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
rep={
  "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
  "jtc_probe_ok": int("$jtc_ok"),
  "eval_jtc_delta": delta,
  "procedure_markers": int("$proc_n"),
  "seal_reject_hits": int("$reject" or 0),
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
