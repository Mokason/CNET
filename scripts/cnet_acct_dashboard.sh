#!/usr/bin/env bash
# Accounting dashboard — aggregate CNET_ACCT_LOG + fault log + store into a brief.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACCT="${CNET_ACCT_LOG:-$ROOT/logs/cnet_acct.jsonl}"
FAULT="${CNET_FAULT_LOG:-$ROOT/logs/cnet_faults.jsonl}"
STORE="${CNET_LORA_STORE_DIR:-$ROOT/logs/lora_store}"
OUT="${1:-$ROOT/docs/janitor/ACCT_DASHBOARD.md}"
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
    last=$(tail -1 "$ACCT")
    if ! jq -e . >/dev/null 2>&1 <<<"$last"; then
      echo "- (parse error or empty)"
    else
      echo "| metric | value |"
      echo "|---|---:|"
      for k in tier_a hard tier_b tier_c teacher gaps abstain err steps adapter_pass adapter_reject teacher_fwd dedup_skip peft_train; do
        v=$(jq -r --arg k "$k" '.[$k] // 0' <<<"$last")
        echo "| $k | $v |"
      done
      echo
      jq -r '
        (((.tier_a//0)+(.hard//0)) as $a |
         ((.tier_c//0)+(.teacher//0)) as $c |
         ($a + $c + (.tier_b//0)) as $t |
         if $t > 0 then
           "- certified-ish share (a+hard)/all_serves ≈ \(((100.0*$a/$t)*10|floor)/10)%\n- residual/teacher share ≈ \(((100.0*$c/$t)*10|floor)/10)%"
         else empty end)
      ' <<<"$last"
    fi
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
  echo "## Alerts"
  if [[ -f "$ACCT" ]]; then
    jq -r '
      (((.tier_a//0)+(.hard//0)) as $a |
       ((.tier_c//0)+(.teacher//0)) as $c |
       (.gaps//0) as $g |
       [ if ($c > $a*3 and ($a+$c) > 10) then "HIGH residual/teacher vs certified — library under-serving" else empty end,
         if ($g > 50 and $a < 5) then "MANY gaps, few certified hits — intake without seals" else empty end,
         if ((.adapter_reject//0) > (.adapter_pass//0) and (.adapter_reject//0) > 3)
           then "Adapter cert rejects dominate — check fault quality / policy" else empty end
       ] | if length==0 then "- none" else map("- ⚠ " + .) | join("\n") end)
    ' <<<"$(tail -1 "$ACCT")" 2>/dev/null || echo "- none"
  else
    echo "- none (no data)"
  fi

  echo
  echo "CNET_ACCT_DASHBOARD_PASS"
} | tee "$OUT"
