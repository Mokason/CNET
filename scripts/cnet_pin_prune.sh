#!/usr/bin/env bash
# Prune old CNB pins; keep N newest (default 2). Never deletes CURRENT target without replacement.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PIN_DIR="${CNET_GOV_PIN_DIR:-$REPO/artifacts/janitor/pins}"
KEEP="${CNET_GOV_MAX_SNAPSHOTS:-2}"
mkdir -p "$PIN_DIR"
mapfile -t pins < <(ls -1t "$PIN_DIR"/pin_*.cnb 2>/dev/null || true)
n=${#pins[@]}
echo "pins=$n keep=$KEEP"
if (( n <= KEEP )); then
  echo "PRUNE_OK nothing"
  exit 0
fi
for ((i=KEEP; i<n; i++)); do
  f="${pins[$i]}"
  # skip if CURRENT points here
  if [[ -f "$PIN_DIR/CURRENT" ]] && grep -qxF "$f" "$PIN_DIR/CURRENT" 2>/dev/null; then
    echo "skip CURRENT $f"
    continue
  fi
  echo "rm $f"
  rm -f -- "$f"
done
echo "PRUNE_OK"
