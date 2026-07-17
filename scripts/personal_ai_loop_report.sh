#!/usr/bin/env bash
# Measure the personal-AI loop: library size, inbox backlog, residual env, lane.
# Usage: scripts/personal_ai_loop_report.sh [base.cnb]
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${1:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
INBOX="${BASE}.inbox"
LEDGER="${BASE}.gaps.txt"
ENVF="$REPO/config/personal-ai.env"

echo "=== Personal AI loop report ($(date -Iseconds)) ==="
echo "base:   $BASE$([ -f "$BASE" ] && echo ' [ok]' || echo ' [MISSING]')"
if [ -f "$BASE" ]; then
  bytes=$(wc -c <"$BASE" | tr -d ' ')
  echo "size:   $bytes bytes"
fi
if [ -x "$REPO/bin/cnb_audit" ] || [ -x "$REPO/bin/base" ]; then
  :
fi
# Unit count via soul_host if built
if [ -x "$REPO/bin/test_soul_residual" ] || [ -f "$REPO/cnet.so" ]; then
  :
fi
if [ -f "$INBOX" ]; then
  notes=$(grep -c . "$INBOX" 2>/dev/null || echo 0)
  noplan=$(grep -c '^NO_PLAN' "$INBOX" 2>/dev/null || echo 0)
  echo "inbox:  $notes lines ($noplan NO_PLAN)"
else
  echo "inbox:  absent"
fi
if [ -f "$LEDGER" ]; then
  echo "ledger: $(wc -l <"$LEDGER" | tr -d ' ') lines"
fi
if [ -f "$ENVF" ]; then
  res=$(grep -E '^CNET_RESIDUAL_GGUF=' "$ENVF" | head -1 | cut -d= -f2- || true)
  echo "residual env: ${res:-unset}"
fi
if systemctl --user is-active --quiet cnet-personal-ai-lane.service 2>/dev/null; then
  echo "learner: active"
  systemctl --user show cnet-personal-ai-lane.service -p MemoryCurrent --value 2>/dev/null | \
    awk '{printf "learner_rss_hint: %s\n", $0}'
else
  echo "learner: inactive"
fi
if pgrep -x CnetMcpServer >/dev/null 2>&1; then
  echo "serve:  CnetMcpServer running"
else
  echo "serve:  not running"
fi
# Recent residual / structure-mine lines from journal
if command -v journalctl >/dev/null 2>&1; then
  echo "--- recent residual/structure (journal, last 2h) ---"
  journalctl --user -u cnet-personal-ai-lane.service --since "2 hours ago" --no-pager 2>/dev/null | \
    grep -E 'residual|structure-mined|teacher sleep|BATCHED|units=' | tail -20 || true
fi
echo "=== end report ==="
