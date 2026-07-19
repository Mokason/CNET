#!/usr/bin/env bash
# Status for single-process Qwythos ladder campaign.
cd "$(dirname "$0")/.."
echo "=== processes ==="
ps aux | grep -E 'cnet_spec_ladder|qwythos_ladder_campaign' | grep -v grep || echo "(none)"
echo
echo "=== pack ==="
ls -lh artifacts/qwythos_ladder*.ldtr 2>/dev/null || echo "(no ldtr yet)"
echo
echo "=== latest log ==="
LOG=logs/qwythos_ladder_campaign.log
if [[ -f "$LOG" ]]; then
  echo "--- stage / summary ---"
  grep -E 'stage |checkpoint|LADDER_SUMMARY|LADDER_PASS|LADDER_SOFT|open_ms|capture|gpu=|import|export' \
    "$LOG" | tail -40
  echo "--- tail ---"
  tail -15 "$LOG"
else
  echo "(no log)"
fi
echo
echo "=== GPU (snapshot) ==="
rocm-smi 2>/dev/null | sed -n '8,16p' || true
