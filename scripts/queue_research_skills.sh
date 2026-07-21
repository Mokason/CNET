#!/usr/bin/env bash
# Queue Unity / RPG research as structured CNET skills.
# Tags must fit PORT_TAG_MAX=32 (nul incl.) → keep names ≤31 chars.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
BASE="${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
INBOX="${CNET_GAP_INBOX:-$BASE.inbox}"
K="${CNET_AUTO_LEARN_K:-3}"
W=256

queue() {
  local goal="$1"
  if (( ${#goal} > 31 )); then
    echo "WARN: tag too long (${#goal}): $goal" >&2
    goal="${goal:0:31}"
  fi
  echo "NO_PLAN 1 $W 1 w_cur 1 $W $K $goal" >> "$INBOX"
  echo "queued $goal"
}

mkdir -p "$(dirname "$INBOX")"
queue research_unity_core_loop
queue research_unity_juice
queue research_unity_so_arch
queue research_unity_scope
queue research_rpg_testimony
queue research_rpg_oath_bearer
queue research_rpg_companion
queue research_rpg_contradiction
queue research_gamedev_command
queue research_gamedev_hit
queue skill_unity_vslice
queue skill_rpg_quest_craft

echo "RESEARCH_QUEUE_OK inbox=$INBOX lines=$(wc -l < "$INBOX")"
