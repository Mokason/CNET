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
# Env: BASE_PATH, WINDOW_FILE, K (default 3), TOKEN_BASE unused in corpus mode
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
BASE="${BASE_PATH:-${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}}"
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

# Prefer minable allowlist token ids that appear in the window; else first N window ids.
TOKENS=()
if [ -f "$ALLOW" ]; then
  while read -r tid; do
    [[ "$tid" =~ ^[0-9]+$ ]] || continue
    for w in "${WIN[@]}"; do
      if [ "$w" = "$tid" ]; then TOKENS+=("$tid"); break; fi
    done
    [ "${#TOKENS[@]}" -ge "$N" ] && break
  done < <(grep -E '^[0-9]+' "$ALLOW" | head -n 500)
fi
if [ "${#TOKENS[@]}" -eq 0 ]; then
  for i in $(seq 0 $((N < W ? N - 1 : W - 1))); do
    TOKENS+=("${WIN[$i]}")
  done
fi

: >>"$INBOX"
added=0
for tid in "${TOKENS[@]}"; do
  # Lane-native NO_PLAN line (port_write format)
  printf 'NO_PLAN 1 %d 1 w_cur 1 %d %d tk%sq%s\n' "$W" "$W" "$K" "$tid" "$tid" >>"$INBOX"
  added=$((added + 1))
  [ "$added" -ge "$N" ] && break
done

echo "grow_local: appended $added teachable seeds (W=$W K=$K) → $INBOX"
echo "grow_local: shape=w_cur → tk*q* top-$K (lane teacher can bind)"
if systemctl --user is-active --quiet cnet-personal-ai-lane.service 2>/dev/null; then
  echo "grow_local: learner active — will drain on next ticks"
else
  echo "grow_local: learner inactive — start with: scripts/personal_ai_auto.sh start"
fi
echo "PERSONAL_AI_GROW_LOCAL_OK seeds=$added window_n=$W k=$K"
