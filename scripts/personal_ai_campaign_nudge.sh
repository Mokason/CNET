#!/usr/bin/env bash
# B: nudge local library growth — LANE-TEACHABLE inbox seeds + optional v2-fast.
# Does NOT lower cert bars. Seeds use w_cur → tk*q* top-k (matches gap_lane).
set -euo pipefail
# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
N="${1:-16}"
bash "$REPO/scripts/personal_ai_grow_local.sh" "$N"
if [ -x "$REPO/tools/campaign_v2_fast.sh" ]; then
  echo "campaign_nudge: campaign_v2_fast available — prepare only (no long run)"
  bash "$REPO/tools/campaign_v2_fast.sh" prepare 2>/dev/null || \
    echo "campaign_nudge: prepare skipped (ok if not configured)"
fi
echo "PERSONAL_AI_CAMPAIGN_NUDGE_OK"
