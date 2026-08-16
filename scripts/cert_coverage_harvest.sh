#!/usr/bin/env bash
# CERT coverage harvest gate — miss clusters that must LOCAL-hit after seed.
# Probe junk (zz*/mystic ooze/teacher-only) must still ABSTAIN / non-LOCAL.
# Law: day-0 seed CERT only; live promote still gold/reviewer — no self-mint here.
set -euo pipefail
ROOT="${1:-artifacts/roe_daily_packs}"
BIN="${BIN_DIR:-bin}"
LOG=logs/cert_coverage_harvest.log
mkdir -p logs "$BIN"

echo "=== CERT coverage harvest gate ===" | tee "$LOG"

# Reseed packs (preserves pack_personal outside PACKS list)
python3 tools/roe_daily_packs_seed.py --root "$ROOT" | tee -a "$LOG"
grep -q ROE_DAILY_PACKS_SEED_OK "$LOG"

# Ensure personal pack has queries_train for gate (garden pack)
if [[ -d $ROOT/pack_personal/skills ]]; then
  python3 - <<'PY' | tee -a "$LOG"
from pathlib import Path
import json
root = Path("artifacts/roe_daily_packs/pack_personal")
qs = []
for sk in sorted((root/"skills").glob("*/SKILL.roe")):
    for ln in sk.read_text().splitlines():
        if ln.startswith("pattern "):
            qs.append(ln[8:].strip())
if qs:
    qs.append("quantum personal junk zz_ood")
    (root/"queries_train.txt").write_text("\n".join(qs)+"\n", encoding="utf-8")
    print(f"pack_personal queries_train n={len(qs)}")
PY
fi

make -s domain_route 2>&1 | tee -a "$LOG"
grep -q DOMAIN_ROUTE_PASS "$LOG"

# Optional full packs gate (includes personal if queries present)
if [[ -x $BIN/roe_daily_packs_gate ]]; then
  "$BIN/roe_daily_packs_gate" 2>&1 | tee -a "$LOG" || true
  grep -q ROE_DAILY_PACKS_PASS "$LOG" || echo "WARN daily_packs_gate not green" | tee -a "$LOG"
fi

# Build front door if needed
if [[ ! -x $BIN/roe_front_door ]]; then
  make roe_front_door 2>&1 | tee -a "$LOG"
fi

must_local=(
  "cnet never lowers floors for brain floats"
  "roe front door selective load"
  "pack personal evolve tick"
  "teacher rate under ten percent warm"
  "roe evolve tick demo query alpha"
  "roe reviewer multi stable demo beta"
  "CNET proposes Unity disposes"
  "who are you"
  "format-truncation werror"
)

must_not_local=(
  "brand new teacher only query zz99"
  "zz unknown mystic ooze 99"
  "totally unknown zzqq mystic ooze"
  "autonomous cycle probe novel fact beta-nine"
)

fail=0
for q in "${must_local[@]}"; do
  out=$("$BIN/roe_front_door" ask "$q" 2>/dev/null || true)
  echo "---- LOCAL? $q" | tee -a "$LOG"
  echo "$out" | head -20 | tee -a "$LOG"
  if echo "$out" | grep -q 'source=LOCAL'; then
    echo "  OK LOCAL" | tee -a "$LOG"
  else
    echo "  FAIL expected LOCAL" | tee -a "$LOG"
    fail=$((fail + 1))
  fi
  # domain route should CERT for harvested phrases (not ABSTAIN)
  dr=$("$BIN/roe_domain_route" "$q" 2>/dev/null || true)
  if echo "$dr" | grep -q 'DISPATCH CERT'; then
    echo "  OK domain CERT" | tee -a "$LOG"
  else
    echo "  WARN domain_route: $dr" | tee -a "$LOG"
  fi
done

for q in "${must_not_local[@]}"; do
  out=$("$BIN/roe_front_door" ask "$q" 2>/dev/null || true)
  echo "---- OOD stay non-LOCAL? $q" | tee -a "$LOG"
  if echo "$out" | grep -q 'source=LOCAL'; then
    echo "  FAIL probe should not LOCAL" | tee -a "$LOG"
    fail=$((fail + 1))
  else
    echo "  OK non-LOCAL (abstain/teacher path)" | tee -a "$LOG"
  fi
done

# Gold files for evolve (gold_file skips reviewer) — answers only, not auto-CERT
mkdir -p "$ROOT/gold"
printf '%s\n' \
  "CNET floors never lowered for Brain floats: Brain is cheap host of same CNU1 packs; pieces.bin opt-in; never drop CERT floors for float convenience." \
  >"$ROOT/gold/cc_brain_floors.txt"
printf '%s\n' \
  "Unattended evolve: tools/roe_evolve_tick.py under autonomy_charter; promote only gold_file or multi_stable+reviewer APPROVE — never self-CERT." \
  >"$ROOT/gold/meta_evolve_tick.txt"
printf '%s\n' \
  "Teacher ≠ reviewer. multi_stable needs reviewer APPROVE; gold_file skips reviewer." \
  >"$ROOT/gold/meta_reviewer.txt"

if [[ $fail -eq 0 ]]; then
  echo "CERT_COVERAGE_HARVEST_PASS fail=0" | tee -a "$LOG"
  exit 0
fi
echo "CERT_COVERAGE_HARVEST_FAIL fail=$fail" | tee -a "$LOG"
exit 1
