#!/usr/bin/env bash
# Emit JSON metrics for the personal-AI loop (C: product/ops).
# Usage: scripts/personal_ai_metrics.sh [base.cnb] > logs/personal_ai_metrics.json
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# Load deploy profile so residual/teacher paths enter placement JSON.
if [ -f "$REPO/config/personal-ai.env" ]; then
  set -a
  # shellcheck source=/dev/null
  . "$REPO/config/personal-ai.env" || true
  set +a
fi
BASE="${1:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
INBOX="${BASE}.inbox"
LEDGER="${BASE}.gaps.txt"
TS=$(date -Iseconds)
units=""
if systemctl --user is-active --quiet cnet-personal-ai-lane.service 2>/dev/null; then
  learner=1
  mem=$(systemctl --user show cnet-personal-ai-lane.service -p MemoryCurrent --value 2>/dev/null || echo 0)
else
  learner=0
  mem=0
fi
serve=0
pgrep -x CnetMcpServer >/dev/null 2>&1 && serve=1
inbox_n=0; noplan=0
[ -f "$INBOX" ] && inbox_n=$(grep -c . "$INBOX" 2>/dev/null || echo 0)
[ -f "$INBOX" ] && noplan=$(grep -c '^NO_PLAN' "$INBOX" 2>/dev/null || echo 0)
ledger_n=0
[ -f "$LEDGER" ] && ledger_n=$(wc -l <"$LEDGER" | tr -d ' ')
cnb_b=0
[ -f "$BASE" ] && cnb_b=$(wc -c <"$BASE" | tr -d ' ')
plan_json="{}"
if [ -x "$REPO/bin/cnet_plan" ]; then
  plan_json=$(CNET_BASE_PATH="$BASE" "$REPO/bin/cnet_plan" json 2>/dev/null || echo '{}')
fi
# Extract last units= from journal if possible
last_units=""
if command -v journalctl >/dev/null 2>&1; then
  last_units=$(journalctl --user -u cnet-personal-ai-lane.service -n 30 --no-pager 2>/dev/null | \
    grep -oE 'units=[0-9]+' | tail -1 | cut -d= -f2 || true)
fi
cur_count=0
if [ -f "${BASE}.curiosity" ]; then
  cur_count=$(grep -E '^count=' "${BASE}.curiosity" 2>/dev/null | head -1 | cut -d= -f2 || echo 0)
fi
printf '%s\n' "{
  \"ts\": \"$TS\",
  \"base\": \"$BASE\",
  \"cnb_bytes\": $cnb_b,
  \"inbox_lines\": $inbox_n,
  \"inbox_no_plan\": $noplan,
  \"ledger_lines\": $ledger_n,
  \"learner_active\": $learner,
  \"learner_memory_bytes\": ${mem:-0},
  \"serve_mcp\": $serve,
  \"units_journal\": ${last_units:-null},
  \"curiosity_hour_count\": ${cur_count:-0},
  \"placement\": $plan_json
}"
