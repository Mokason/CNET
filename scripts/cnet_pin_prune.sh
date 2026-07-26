#!/usr/bin/env bash
# Prune old CNB pins; keep N newest (default 2). Never deletes CURRENT target without replacement.
#
# KEEP.txt (one pin basename per line, # comments allowed) pins a snapshot
# permanently. Rotation alone is not enough when a pin is the ONLY copy of
# dropped units: the CURRENT guard protects just the latest, so two further
# consolidate applies would age such a pin out and destroy the restore option.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
PIN_DIR="${CNET_GOV_PIN_DIR:-$REPO/artifacts/janitor/pins}"
KEEP="${CNET_GOV_MAX_SNAPSHOTS:-2}"
KEEPLIST="$PIN_DIR/KEEP.txt"
mkdir -p "$PIN_DIR"

# --dry-run lists what would go without touching anything. Use it for any
# verification against the real pin directory.
DRY=0
[[ "${1:-}" == "--dry-run" ]] && DRY=1

# A pin can be the only copy of units that are in no live base. KEEP<1 would
# put every unprotected pin on the block, so it needs deliberate intent.
if (( KEEP < 1 )) && [[ "${CNET_PIN_PRUNE_FORCE:-0}" != "1" ]]; then
  echo "REFUSE: CNET_GOV_MAX_SNAPSHOTS=$KEEP would prune all unprotected pins." >&2
  echo "        Set CNET_PIN_PRUNE_FORCE=1 to mean it, or --dry-run to preview." >&2
  exit 2
fi

is_protected() {  # is_protected <path>
  local b; b="$(basename "$1")"
  [[ -f "$KEEPLIST" ]] || return 1
  grep -vE '^\s*(#|$)' "$KEEPLIST" 2>/dev/null | sed 's/[[:space:]]*$//' \
    | grep -qxF "$b"
}
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
  if is_protected "$f"; then
    echo "skip KEEP.txt $f"
    continue
  fi
  if (( DRY )); then
    echo "would rm $f"
    continue
  fi
  echo "rm $f"
  rm -f -- "$f"
done
echo "PRUNE_OK"
