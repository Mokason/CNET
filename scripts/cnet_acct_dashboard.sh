#!/usr/bin/env bash
# Accounting dashboard — aggregate CNET_ACCT_LOG + fault log + store into a brief.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACCT="${CNET_ACCT_LOG:-$ROOT/logs/cnet_acct.jsonl}"
FAULT="${CNET_FAULT_LOG:-$ROOT/logs/cnet_faults.jsonl}"
STORE="${CNET_LORA_STORE_DIR:-$ROOT/logs/lora_store}"
OUT="${1:-$ROOT/artifacts/janitor/ACCT_DASHBOARD.md}"
mkdir -p "$(dirname "$OUT")" "$ROOT/logs"

{
  echo "# CNET accounting dashboard"
  echo
  echo "- when: $(date -Iseconds)"
  echo "- acct: \`$ACCT\`"
  echo "- fault: \`$FAULT\`"
  echo "- store: \`$STORE\`"
  echo

  echo "## Latest acct snapshot"
  if [[ -f "$ACCT" ]]; then
    tail -1 "$ACCT" | python3 -c '
import sys,json
try:
  d=json.loads(sys.stdin.read().strip() or "{}")
except Exception as e:
  print("- (parse error)", e); sys.exit(0)
keys=["tier_a","hard","tier_b","tier_c","teacher","gaps","abstain","err","steps","adapter_pass","adapter_reject","teacher_fwd","dedup_skip","peft_train"]
print("| metric | value |")
print("|---|---:|")
for k in keys:
  print(f"| {k} | {d.get(k,0)} |")
a=float(d.get("tier_a",0)+d.get("hard",0)); c=float(d.get("tier_c",0)+d.get("teacher",0)); t=a+c+float(d.get("tier_b",0))
if t>0:
  print()
  print(f"- certified-ish share (a+hard)/all_serves ≈ {100*(a)/max(t,1):.1f}%")
  print(f"- residual/teacher share ≈ {100*c/max(t,1):.1f}%")
' || echo "- (empty or unreadable)"
    echo
    echo "- lines: $(wc -l < "$ACCT")"
  else
    echo "- no acct log yet (run serves with CNET_ACCT_LOG set)"
  fi

  echo
  echo "## Fault bus"
  if [[ -f "$FAULT" ]]; then
    echo "- lines: $(wc -l < "$FAULT")"
    echo "- labeled vectors (approx): $(rg -c '"in":\[' "$FAULT" 2>/dev/null || echo 0)"
  else
    echo "- no fault log yet"
  fi

  echo
  echo "## LoRA store"
  if [[ -d "$STORE" ]]; then
    n=$(find "$STORE" -name '*.lora' 2>/dev/null | wc -l)
    echo "- adapters on disk: $n"
    find "$STORE" -name '*.lora' 2>/dev/null | head -20 | sed 's/^/- /'
  else
    echo "- store dir missing"
  fi

  echo
  # Budgets / alerts
  echo "## Alerts"
  if [[ -f "$ACCT" ]]; then
    python3 - <<'PY' "$ACCT"
import json,sys
path=sys.argv[1]
try:
  lines=open(path).read().strip().splitlines()
  d=json.loads(lines[-1]) if lines else {}
except Exception:
  d={}
alerts=[]
a=d.get("tier_a",0)+d.get("hard",0)
c=d.get("tier_c",0)+d.get("teacher",0)
g=d.get("gaps",0)
if c>a*3 and (a+c)>10:
  alerts.append("HIGH residual/teacher vs certified — library under-serving")
if g>50 and a<5:
  alerts.append("MANY gaps, few certified hits — intake without seals")
if d.get("adapter_reject",0)>d.get("adapter_pass",0) and d.get("adapter_reject",0)>3:
  alerts.append("Adapter cert rejects dominate — check fault quality / policy")
if not alerts:
  print("- none")
else:
  for x in alerts: print("- ⚠", x)
PY
  else
    echo "- none (no data)"
  fi

  echo
  echo "CNET_ACCT_DASHBOARD_PASS"
} | tee "$OUT"
