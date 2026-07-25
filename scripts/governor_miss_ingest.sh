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

waiting = 0
open_n = 0
if gaps.exists():
    for i,line in enumerate(gaps.read_text(errors="replace").splitlines()):
        if i < 2: continue
        if "waiting_oracle" in line or "waiting_charter" in line:
            waiting += 1
        parts=line.split()
        if len(parts)>1 and parts[1]=="1":
            open_n += 1

# real_miss_rate proxy: waiting / (waiting+closed-ish) capped
denom = max(1, waiting + open_n + 1)
real_miss_rate = min(1.0, waiting / denom)
top = units.most_common(12)
rep = {
  "ts": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
  "fault_lines": total,
  "jtc_faults": jtc,
  "waiting_oracle": waiting,
  "open_gaps": open_n,
  "real_miss_rate": round(real_miss_rate, 4),
  "top_units": [{"unit": u, "n": n} for u,n in top],
}
out.write_text(json.dumps(rep, indent=2)+"\n")
print("MISS_BUS_OK", json.dumps({"fault_lines": total, "waiting": waiting, "real_miss_rate": rep["real_miss_rate"]}))
PY
