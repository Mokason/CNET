#!/usr/bin/env bash
# Raise local hit rate with LANE-TEACHABLE inbox seeds (not residual-only shapes).
#
# The gap_lane teacher only binds ONEHOT[W]×ONEHOT[W×k] with W = window size
# (english_window_256). Historical sealed units use:
#   in:  family=1 width=256 count=1 tag=w_cur
#   out: family=1 width=256 count=3 tag=tk{token}q{token}   (top-k=3)
#
# Residual tags (res_tok/res_next) are for personal_ai serve; they often show
# up as skipped_no_oracle on the lane. This script seeds the teachable shape.
#
# Usage:
#   scripts/personal_ai_grow_local.sh [n_seeds]
# Env: BASE_PATH / CNET_BASE_PATH, WINDOW_FILE, K (default 3), ALLOWLIST
set -euo pipefail
# shellcheck source=personal_ai_common.sh
. "$(cd "$(dirname "$0")" && pwd)/personal_ai_common.sh"
BASE="$(cnet_default_base)"
INBOX="${BASE}.inbox"
WINDOW="${WINDOW_FILE:-${CNET_WINDOW_FILE:-$REPO/english_window_256.txt}}"
N="${1:-16}"
K="${K:-3}"
ALLOW="${ALLOWLIST:-$REPO/artifacts/qwythos_v2_minable.ids}"

if [ ! -f "$BASE" ]; then
  echo "grow_local: missing base $BASE" >&2
  exit 2
fi
if [ ! -f "$WINDOW" ]; then
  echo "grow_local: missing window $WINDOW" >&2
  exit 2
fi

mapfile -t WIN < <(grep -E '^[0-9]+' "$WINDOW")
W=${#WIN[@]}
if [ "$W" -lt 4 ]; then
  echo "grow_local: window too small" >&2
  exit 2
fi

# O(1) membership instead of nested scan over window × allowlist.
declare -A IN_WIN=()
for w in "${WIN[@]}"; do
  IN_WIN["$w"]=1
done

TOKENS=()
if [ -f "$ALLOW" ]; then
  while read -r tid; do
    [[ "$tid" =~ ^[0-9]+$ ]] || continue
    if [ -n "${IN_WIN[$tid]:-}" ]; then
      TOKENS+=("$tid")
      [ "${#TOKENS[@]}" -ge "$N" ] && break
    fi
  done < <(grep -E '^[0-9]+' "$ALLOW" | head -n 2000)
fi
if [ "${#TOKENS[@]}" -eq 0 ]; then
  local_n=$((N < W ? N : W))
  for ((i = 0; i < local_n; i++)); do
    TOKENS+=("${WIN[$i]}")
  done
fi

: >>"$INBOX"
# Batch-append (one open) instead of N appends
added=0
{
  for tid in "${TOKENS[@]}"; do
    printf 'NO_PLAN 1 %d 1 w_cur 1 %d %d tk%sq%s\n' "$W" "$W" "$K" "$tid" "$tid"
    added=$((added + 1))
    [ "$added" -ge "$N" ] && break
  done
} >>"$INBOX"

echo "grow_local: appended $added teachable seeds (W=$W K=$K) → $INBOX"
echo "grow_local: shape=w_cur → tk*q* top-$K (lane teacher can bind)"
if cnet_learner_active; then
  echo "grow_local: learner active — will drain on next ticks"
else
  echo "grow_local: learner inactive — start with: scripts/personal_ai_auto.sh start"
fi
echo "PERSONAL_AI_GROW_LOCAL_OK seeds=$added window_n=$W k=$K"
