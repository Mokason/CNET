#!/usr/bin/env bash
# Emit JSON metrics for the personal-AI loop (C: product/ops).
# Usage: scripts/personal_ai_metrics.sh [base.cnb] > logs/personal_ai_metrics.json
set -euo pipefail
# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
cnet_load_personal_env
BASE="${1:-$(cnet_default_base)}"
INBOX="${BASE}.inbox"
LEDGER="${BASE}.gaps.txt"
TS=$(date -Iseconds)

if cnet_learner_active; then
  learner=1
  mem=$(systemctl --user show cnet-personal-ai-lane.service -p MemoryCurrent --value 2>/dev/null || echo 0)
else
  learner=0
  mem=0
fi
serve=0
cnet_serve_active && serve=1

inbox_n=$(cnet_count_lines "$INBOX" '.')
noplan=$(cnet_count_lines "$INBOX" '^NO_PLAN')
ledger_n=$(cnet_count_lines "$LEDGER" '.')
cnb_b=$(cnet_file_bytes "$BASE")

plan_json="{}"
if [ -x "$REPO/bin/cnet_plan" ]; then
  plan_json=$(CNET_BASE_PATH="$BASE" "$REPO/bin/cnet_plan" json 2>/dev/null || echo '{}')
fi

# Prefer sealed CNB unit count; fall back to journal (can lag).
units_cnb=$(cnet_unit_count_fast "$BASE")
last_units="$units_cnb"
if [ -z "$last_units" ] && command -v journalctl >/dev/null 2>&1; then
  last_units=$(journalctl --user -u cnet-personal-ai-lane.service -n 30 --no-pager 2>/dev/null | \
    grep -oE 'units=[0-9]+' | tail -1 | cut -d= -f2 || true)
fi

cur_count=0
if [ -f "${BASE}.curiosity" ]; then
  cur_count=$(grep -E '^count=' "${BASE}.curiosity" 2>/dev/null | head -1 | cut -d= -f2 || true)
  cur_count=${cur_count:-0}
fi

# Sanitize numeric fields for JSON (no newlines from bad greps).
mem=${mem//$'\n'/}
mem=${mem:-0}
last_units=${last_units//$'\n'/}

printf '%s\n' "{
  \"ts\": \"$TS\",
  \"base\": \"$BASE\",
  \"cnb_bytes\": $cnb_b,
  \"inbox_lines\": $inbox_n,
  \"inbox_no_plan\": $noplan,
  \"ledger_lines\": $ledger_n,
  \"learner_active\": $learner,
  \"learner_memory_bytes\": $mem,
  \"serve_mcp\": $serve,
  \"units\": ${last_units:-null},
  \"units_journal\": ${last_units:-null},
  \"curiosity_hour_count\": ${cur_count:-0},
  \"placement\": $plan_json
}"
