#!/usr/bin/env bash
# Raise local hit rate: seed the gap inbox with window-token signatures so the
# learner (gap_lane) teaches/seals more units into the personal library.
#
# Usage:
#   scripts/personal_ai_grow_local.sh [n_seeds]
# Env:
#   BASE_PATH, WINDOW_FILE (default english_window_256.txt)
#   TOKEN_BASE (default 1000) — inbox port encoding convention for lane
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${BASE_PATH:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
INBOX="${BASE}.inbox"
WINDOW="${WINDOW_FILE:-${CNET_WINDOW_FILE:-$REPO/english_window_256.txt}}"
N="${1:-32}"
# Port family ONEHOT=1; width follows window size for residual alignment.
# Gap lane teaches token units from its own context; inbox NO_PLAN lines with
# onehot ports matching residual window encourage residual+learner overlap.

if [ ! -f "$BASE" ]; then
  echo "grow_local: missing base $BASE" >&2
  exit 2
fi
if [ ! -f "$WINDOW" ]; then
  echo "grow_local: missing window $WINDOW" >&2
  exit 2
fi

mapfile -t IDS < <(grep -E '^[0-9]+' "$WINDOW")
W=${#IDS[@]}
if [ "$W" -lt 4 ]; then
  echo "grow_local: window too small" >&2
  exit 2
fi

: >>"$INBOX"
# Residual GGUF answers only when ONEHOT width == residual window (typically 256).
# Seed residual-shaped signatures so serve can Tier-C answer while the lane learns.
added=0
# One residual-shaped signature (res_tok → res_next) — enough for residual path.
printf 'NO_PLAN 1 %d 1 res_tok 1 %d 1 res_next\n' "$W" "$W" >>"$INBOX"
added=$((added + 1))
# Additional novel goal tags (same width) for lane variety
for i in $(seq 0 $((N - 1))); do
  printf 'NO_PLAN 1 %d 1 grow_in 1 %d 1 grow_tok_%d\n' "$W" "$W" "$i" >>"$INBOX"
  added=$((added + 1))
done

echo "grow_local: appended seeds to $INBOX (window_n=$W seeds=$added)"
echo "grow_local: learner will drain on next ticks (cnet-personal-ai-lane)"
if systemctl --user is-active --quiet cnet-personal-ai-lane.service 2>/dev/null; then
  echo "grow_local: learner active — backlog will process at interval"
else
  echo "grow_local: learner inactive — start with scripts/personal_ai_auto.sh start"
fi
echo "PERSONAL_AI_GROW_LOCAL_OK seeds=$added window_n=$W"
