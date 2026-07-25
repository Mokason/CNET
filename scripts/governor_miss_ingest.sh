#!/usr/bin/env bash
# Ingest real misses from fault bus + gap ledger → logs/governor/miss_bus.json
set -euo pipefail
ROOT="${CNET_ROOT:-$(cd "$(dirname "$0")/.." && pwd)}"
BASE="${CNET_BASE_PATH:-$ROOT/soul_gemma4v2_final.cnb}"
FAULT="${CNET_FAULT_LOG:-$ROOT/logs/cnet_faults.jsonl}"
GAPS="${BASE}.gaps.txt"
OUT="${CNET_GOVERNOR_DIR:-$ROOT/logs/governor}/miss_bus.json"
mkdir -p "$(dirname "$OUT")"

python3 - <<PY
import json, time, re
from collections import Counter
from pathlib import Path

fault = Path("$FAULT")
gaps = Path("$GAPS")
out = Path("$OUT")

units = Counter()
total = 0
jtc = 0
if fault.exists():
    for line in fault.open(errors="replace"):
        line=line.strip()
        if not line: continue
        try:
            d=json.loads(line)
        except Exception:
            continue
        total += 1
        u = d.get("unit") or d.get("tag") or "unknown"
        units[u] += 1
        if "jtc" in u.lower() or "json_tool" in u.lower():
            jtc += 1

# A waiting_oracle row is an OPEN row carrying an annotation, not a third
# state (verified against the live ledger: every waiting row has state 1).
# Counting both independently double-counted them; governor_autonomous.collect()
# instead partitions outstanding rows into waiting XOR open, so mirror that or
# backlog_pressure and real_miss_rate silently disagree about the same ledger.
waiting = 0
open_n = 0
closed_n = 0
if gaps.exists():
    for i,line in enumerate(gaps.read_text(errors="replace").splitlines()):
        if i < 2: continue
        parts=line.split()
        state = parts[1] if len(parts)>1 else ""
        if "waiting_oracle" in line or "waiting_charter" in line:
            waiting += 1
        elif state=="1":
            open_n += 1
        elif state=="2":
            closed_n += 1

# real_miss_rate: share of the ledger still outstanding.
# The old denominator was (waiting + open + 1) — it excluded every closed gap,
# so the rate could never fall below ~0.3 no matter how well the lane drained,
# and real_miss_cut permanently dominated project ranking. Successes must be in
# the denominator for this to be a rate at which 0 means "healthy".
outstanding = waiting + open_n
denom = max(1, outstanding + closed_n)
real_miss_rate = min(1.0, outstanding / denom)
top = units.most_common(12)
rep = {
  "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
  "fault_lines": total,
  "jtc_faults": jtc,
  "waiting_oracle": waiting,
  "open_gaps": open_n,
  "closed_gaps": closed_n,
  "real_miss_rate": round(real_miss_rate, 4),
  "real_miss_formula": "(waiting_oracle + open_gaps) / (waiting_oracle + open_gaps + closed_gaps); waiting XOR open partition the outstanding set",
  "top_units": [{"unit": u, "n": n} for u,n in top],
}
out.write_text(json.dumps(rep, indent=2)+"\n")
print("MISS_BUS_OK", json.dumps({"fault_lines": total, "waiting": waiting, "real_miss_rate": rep["real_miss_rate"]}))
PY
