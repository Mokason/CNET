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
REPO="$(cd "$(dirname "$0")/.." && pwd)"
if [ -f "$REPO/config/personal-ai.env" ]; then
  set -a
  # shellcheck source=/dev/null
  . "$REPO/config/personal-ai.env" || true
  set +a
fi
BASE="${1:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
DAYS="${2:-7}"
LOG="${CNET_EG_LOG:-${BASE}.hill_climb.jsonl}"
BASELINE="${CNET_EG_BASELINE_COST:-32}"
NOW=$(date +%s)
T0=$((NOW - DAYS * 86400))
mkdir -p "$REPO/logs"

# Ensure bin/cnet_eg if possible
if [ ! -x "$REPO/bin/cnet_eg" ]; then
  make -C "$REPO" cnet_eg_cli -j"$(nproc 2>/dev/null || echo 2)" >/dev/null 2>&1 || true
fi

echo "=== CNET hill-climb report (last ${DAYS}d) ==="
echo "base: $BASE"
echo "log:  $LOG"
echo "baseline_cost_per_seal: $BASELINE"

if [ ! -f "$LOG" ]; then
  echo "No EG log yet — learner writes on each did_work tick after rebuild."
  echo "Hint: leave lane running; or seed: scripts/personal_ai_grow_local.sh 8"
  # Fallback: scrape journal if present
  if command -v journalctl >/dev/null 2>&1; then
    echo "--- journal fallback (approx) ---"
    closed=$(journalctl --user -u cnet-personal-ai-lane.service --since "${DAYS} days ago" --no-pager 2>/dev/null | \
      grep -oE 'closed=[0-9]+' | awk -F= '{s+=$2} END{print s+0}')
    drained=$(journalctl --user -u cnet-personal-ai-lane.service --since "${DAYS} days ago" --no-pager 2>/dev/null | \
      grep -oE 'drained=[0-9]+' | awk -F= '{s+=$2} END{print s+0}')
    units=$(journalctl --user -u cnet-personal-ai-lane.service -n 5 --no-pager 2>/dev/null | \
      grep -oE 'units=[0-9]+' | tail -1 | cut -d= -f2 || echo 0)
    echo "  journal_closed_sum=$closed journal_drained_sum=$drained last_units=$units"
    if [ "${closed:-0}" -gt 0 ] 2>/dev/null; then
      cps=$(python3 -c "print(round($drained/max($closed,1), 4))")
      eg=$(python3 -c "print(round($BASELINE/max($drained/max($closed,1),1e-9), 4))")
      echo "  approx_cost_per_seal=$cps  approx_local_eg=$eg"
      echo "  (EG>1 = better than baseline $BASELINE examined/seal)"
    fi
  fi
  echo "HILL_CLIMB_REPORT_OK partial=1"
  exit 0
fi

if [ -x "$REPO/bin/cnet_eg" ]; then
  "$REPO/bin/cnet_eg" report --log "$LOG" --since "$T0" --baseline "$BASELINE" | tee "$REPO/logs/hill_climb_report.txt"
else
  python3 - <<PY
import json, time
from pathlib import Path
log = Path("$LOG")
t0, t1 = $T0, $NOW
base = float("$BASELINE")
work=seals=deferred=no_oracle=curiosity=0
units=0
first=last=None
for line in log.read_text().splitlines():
    try:
        o=json.loads(line)
    except Exception:
        continue
    ts=int(o.get("ts",0))
    if ts<t0 or ts>t1: continue
    first = ts if first is None or ts<first else first
    last = ts if last is None or ts>last else last
    work += int(o.get("examined",0))
    seals += int(o.get("closed",0))
    deferred += int(o.get("deferred",0))
    no_oracle += int(o.get("no_oracle",0))
    curiosity += int(o.get("curiosity",0))
    units = max(units, int(o.get("units",0)))
hours = max((last-first)/3600.0, 1e-9) if first and last else 1e-9
cost = work / max(seals, 1)
eg = base / max(cost, 1e-9)
rate = seals / hours
print(f"  teacher_work(examined)={work}")
print(f"  seals(closed)={seals}")
print(f"  deferred={deferred} no_oracle={no_oracle} curiosity={curiosity}")
print(f"  units_end={units} hours={hours:.3f}")
print(f"  cost_per_seal={cost:.4f}  (lower better)")
print(f"  seal_rate={rate:.4f}/h")
print(f"  local_eg={eg:.4f}  (>1 better than baseline {base})")
if eg > 1:
    print(f"  trend: climbing — {eg:.2f}× more efficient than baseline")
elif seals == 0:
    print("  trend: no seals in window — feed teachable gaps or wait for curiosity")
else:
    print(f"  trend: below baseline — each seal costs more teach work than {base}")
Path("$REPO/logs/hill_climb_report.json").write_text(json.dumps({
  "teacher_work": work, "seals": seals, "deferred": deferred,
  "no_oracle": no_oracle, "curiosity": curiosity, "units": units,
  "hours": hours, "cost_per_seal": cost, "seal_rate": rate,
  "local_eg": eg, "baseline_cost": base, "days": $DAYS
}, indent=2)+"\n")
print("HILL_CLIMB_REPORT_OK")
PY
fi
echo "HILL_CLIMB_REPORT_OK"
