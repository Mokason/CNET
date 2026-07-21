#!/usr/bin/env bash
# Restore a CNB pin created by the G2 janitor.
# Stops personal-AI lane, restores pin → base, restarts lane.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

BASE="${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
PIN_DIR="${CNET_GOV_PIN_DIR:-$REPO/artifacts/janitor/pins}"
PIN="${1:-}"

if [[ -z "$PIN" ]]; then
  if [[ -f "$PIN_DIR/CURRENT" ]]; then
    PIN="$(cat "$PIN_DIR/CURRENT")"
  else
    echo "usage: $0 <pin.cnb>   (or set pins/CURRENT)"
    exit 2
  fi
fi

if [[ ! -f "$PIN" ]]; then
  echo "pin missing: $PIN"
  exit 3
fi

echo "Restoring:"
echo "  pin:  $PIN"
echo "  base: $BASE"

# Stop learner so it doesn't write mid-copy
systemctl --user stop cnet-personal-ai-lane.service 2>/dev/null || true
touch "${BASE}.stop"
sleep 1

# Safety copy of current base
TS=$(date +%Y%m%d_%H%M%S)
mkdir -p "$PIN_DIR"
if [[ -f "$BASE" ]]; then
  cp -f -- "$BASE" "$PIN_DIR/pre_restore_${TS}_$(basename "$BASE")"
fi

cp -f -- "$PIN" "$BASE"
echo "$(date -Iseconds) restored_from $PIN" >> "${BASE}.restore.log"
rm -f "${BASE}.stop"

systemctl --user start cnet-personal-ai-lane.service 2>/dev/null || true
echo "RESTORE_OK base=$BASE from=$PIN"
