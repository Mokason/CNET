#!/usr/bin/env bash
# B: nudge local library growth — inbox seeds + optional v2-fast prepare.
# Does NOT lower cert bars. Safe to run while learner is active.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
N="${1:-16}"
bash "$REPO/scripts/personal_ai_grow_local.sh" "$N"
if [ -x "$REPO/tools/campaign_v2_fast.sh" ]; then
  echo "campaign_nudge: campaign_v2_fast available — prepare only (no long run)"
  # prepare is cheap if already built; ignore failures if artifacts missing
  bash "$REPO/tools/campaign_v2_fast.sh" prepare 2>/dev/null || \
    echo "campaign_nudge: prepare skipped (ok if not configured)"
fi
echo "PERSONAL_AI_CAMPAIGN_NUDGE_OK"
