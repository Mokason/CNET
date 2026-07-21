#!/usr/bin/env bash
# Queue Unity / RPG research topics as structured CNET skills (research_*).
# Does not train here — personal-AI lane seals when teacher is up.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
BASE="${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
INBOX="${CNET_GAP_INBOX:-$BASE.inbox}"
K="${CNET_AUTO_LEARN_K:-3}"
W=256

queue() {
  local goal="$1"
  echo "NO_PLAN 1 $W 1 w_cur 1 $W $K $goal" >> "$INBOX"
  echo "queued $goal"
}

mkdir -p "$(dirname "$INBOX")"
# Unity craft
queue research_unity_core_loop
queue research_unity_game_feel_juice
queue research_unity_scriptable_objects
queue research_unity_scope_discipline
# RPG / storytelling (Memory-Witness lane)
queue research_rpg_place_as_testimony
queue research_rpg_oath_bearer_player
queue research_rpg_companion_dilemma
queue research_rpg_contradiction_world
# Code patterns
queue research_gamedev_command_pattern
queue research_gamedev_hit_pipeline
queue skill_unity_vertical_slice_checklist
queue skill_rpg_quest_prompt_craft

echo "RESEARCH_QUEUE_OK inbox=$INBOX lines=$(wc -l < "$INBOX")"
