#!/usr/bin/env bash
# Measure the personal-AI loop: library size, inbox backlog, residual env, lane.
# Usage: scripts/personal_ai_loop_report.sh [base.cnb]
set -euo pipefail
# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
BASE="${1:-$(cnet_default_base)}"
INBOX="${BASE}.inbox"
LEDGER="${BASE}.gaps.txt"
ENVF="$REPO/config/personal-ai.env"

echo "=== Personal AI loop report ($(date -Iseconds)) ==="
if [ -x "$REPO/bin/cnet_plan" ]; then
  echo "--- placement (cnet_plan doctor) ---"
  (cd "$REPO" && CNET_BASE_PATH="$BASE" ./bin/cnet_plan doctor 2>/dev/null) || true
  echo "---"
fi
echo "base:   $BASE$([ -f "$BASE" ] && echo ' [ok]' || echo ' [MISSING]')"
if [ -f "$BASE" ]; then
  echo "size:   $(cnet_file_bytes "$BASE") bytes"
  units=$(cnet_unit_count_fast "$BASE")
  if [ -n "$units" ]; then
    echo "units:  $units (cnb)"
  elif command -v journalctl >/dev/null 2>&1; then
    # Cheap fallback — journal last units= (no SoulHost load)
    u=$(journalctl --user -u cnet-personal-ai-lane.service -n 20 --no-pager 2>/dev/null | \
      grep -oE 'units=[0-9]+' | tail -1 | cut -d= -f2 || true)
    [ -n "${u:-}" ] && echo "units:  $u (journal)"
  fi
fi
if [ -f "$INBOX" ]; then
  notes=$(cnet_count_lines "$INBOX" '.')
  noplan=$(cnet_count_lines "$INBOX" '^NO_PLAN')
  echo "inbox:  $notes lines ($noplan NO_PLAN)"
else
  echo "inbox:  absent"
fi
if [ -f "$LEDGER" ]; then
  echo "ledger: $(cnet_count_lines "$LEDGER" '.') lines"
fi
if [ -f "$ENVF" ]; then
  res=$(grep -E '^CNET_RESIDUAL_GGUF=' "$ENVF" | head -1 | cut -d= -f2- || true)
  echo "residual env: ${res:-unset}"
fi
if cnet_learner_active; then
  echo "learner: active"
  systemctl --user show cnet-personal-ai-lane.service -p MemoryCurrent --value 2>/dev/null | \
    awk '{printf "learner_rss_hint: %s\n", $0}'
else
  echo "learner: inactive"
fi
if cnet_serve_active; then
  echo "serve:  CnetMcpServer running"
else
  echo "serve:  not running"
fi
if command -v journalctl >/dev/null 2>&1; then
  echo "--- recent residual/structure (journal, last 2h) ---"
  journalctl --user -u cnet-personal-ai-lane.service --since "2 hours ago" --no-pager 2>/dev/null | \
    grep -E 'residual|structure-mined|teacher sleep|BATCHED|units=|local_eg|closed=' | tail -20 || true
fi
if [ -f "${BASE}.hill_climb.jsonl" ]; then
  echo "eg_log: ${BASE}.hill_climb.jsonl ($(cnet_count_lines "${BASE}.hill_climb.jsonl" '.') lines)"
fi
echo "=== end report ==="
