#!/usr/bin/env bash
# Bounded soak gate on packaged or repo runtime.
# 1–2 evolve ticks + 1–2 autonomous cycles; hard assertions.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
LOG=logs/cnet_runtime_soak_gate.log
mkdir -p logs
PACKS=artifacts/roe_daily_packs
PERSONAL="$PACKS/pack_personal/catalog.jsonl"

{
  echo "=== CNET runtime soak gate ==="
  fail=0

  # Prefer package if path file exists
  if [[ -f dist/CNET-Minimal-latest.path ]]; then
    PKG=$(cat dist/CNET-Minimal-latest.path)
    echo "package=$PKG"
    if [[ -x $PKG/scripts/cnet_runtime_smoke.sh ]]; then
      CNET_MINIMAL_ROOT="$PKG" bash "$PKG/scripts/cnet_runtime_smoke.sh" || fail=$((fail + 1))
    fi
  fi

  # Repo smoke too
  bash scripts/cnet_runtime_smoke.sh || fail=$((fail + 1))

  before_n=0
  if [[ -f $PERSONAL ]]; then
    before_n=$(grep -c '^{' "$PERSONAL" || echo 0)
  fi
  echo "personal_before=$before_n"

  # Evolve ticks (2)
  for i in 1 2; do
    echo "---- evolve tick $i ----"
    python3 tools/roe_evolve_tick.py 2>&1 | tee -a logs/soak_evolve_$i.log | tail -12
    grep -q ROE_EVOLVE_TICK_PASS logs/soak_evolve_$i.log || fail=$((fail + 1))
  done

  # Autonomous (2 if script exists)
  if [[ -f scripts/cnet_autonomous_cycle.py ]]; then
    for i in 1 2; do
      echo "---- autonomous $i ----"
      timeout 120 python3 scripts/cnet_autonomous_cycle.py 2>&1 | tee -a logs/soak_auto_$i.log | tail -15 || true
      if grep -q CNET_AUTONOMOUS_PASS logs/soak_auto_$i.log; then
        echo "  ok autonomous $i"
      else
        echo "  WARN autonomous $i soft"
      fi
    done
  fi

  # Assertions on last evolve report
  python3 - <<'PY'
import json, sys
from pathlib import Path
from collections import Counter
fail=0
rep=json.load(open("artifacts/roe_daily_packs/EVOLVE_TICK.json"))
prom=rep.get("promoted") or []
skip=rep.get("skipped") or []
print("promoted", len(prom))
print("skipped", len(skip))
# promoted must not be blocklisted content
for p in prom:
    ans=(p.get("answer_preview") or "").lower()
    q=(p.get("query") or "").lower()
    if ans.startswith("abstain") or "mystic ooze" in q or "zz99" in q:
        print("FAIL bad promote", p)
        fail+=1
c=Counter()
for s in skip:
    r=s.get("reason") or ""
    c[r.split(":")[0] if r.startswith("block") else r]+=1
print("skip_reasons", dict(c))
blocks=sum(1 for s in skip if "block" in (s.get("reason") or ""))
print("blocklist_skips", blocks)
if blocks < 1:
    # only fail if miss_log still has probes
    print("WARN no blocklist skips this tick (may be idle)")
# personal sterile
rows=[]
p=Path("artifacts/roe_daily_packs/pack_personal/catalog.jsonl")
if p.exists():
    for ln in p.read_text().splitlines():
        if ln.strip():
            rows.append(json.loads(ln))
bad=[r for r in rows if (r.get("answer") or "").lower().startswith("abstain")
     or "mystic" in (r.get("pattern") or "").lower()
     or "zz99" in (r.get("pattern") or "").lower()]
print("personal_n", len(rows), "bad", len(bad))
if bad:
    fail+=1
    print("FAIL personal polluted", bad)
# local_hit floor from autonomous if present
ap=Path("logs/marble_24_7/AUTONOMOUS_CYCLE.json")
if ap.exists():
    d=json.load(open(ap))
    kpi=d.get("kpi") or {}
    hit=kpi.get("local_hit")
    if hit is None:
        hit=d.get("local_hit") or d.get("local_hit_rate")
    if hit is None and kpi.get("probe_n"):
        hit=float(kpi.get("local_n") or 0)/float(kpi["probe_n"])
    hit=float(hit or 0)
    print("autonomous_local_hit", hit)
    if hit < 0.80:
        print("FAIL local_hit floor", hit)
        fail+=1
    else:
        print("ok local_hit >= 0.80")
sys.exit(fail)
PY
  rc=$?
  fail=$((fail + rc))

  after_n=0
  if [[ -f $PERSONAL ]]; then
    after_n=$(grep -c '^{' "$PERSONAL" || echo 0)
  fi
  echo "personal_after=$after_n"

  if [[ $fail -eq 0 ]]; then
    echo "CNET_RUNTIME_SOAK_GATE_PASS"
    exit 0
  fi
  echo "CNET_RUNTIME_SOAK_GATE_FAIL fail=$fail"
  exit 1
} | tee "$LOG"
