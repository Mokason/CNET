#!/usr/bin/env bash
# Write skill map for Hermes: sealed skills + how to call them.
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
BASE="${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
OUT="${1:-artifacts/janitor/SKILL_MAP.md}"
mkdir -p "$(dirname "$OUT")"
{
  echo "# CNET skill map (Hermes)"
  echo
  echo "_Generated $(date -Iseconds)_"
  echo
  echo "## How to use"
  echo
  echo '```'
  echo 'cnet_list_skills'
  echo 'cnet_use_skill skill=<name> query="<context>"'
  echo 'cnet_learn_from_chat text="..." skill=<name>   # teach/update structured'
  echo '```'
  echo
  echo "## When to call"
  echo
  echo "| Intent | Skill tag |"
  echo "|---|---|"
  echo "| Unity core loop / one verb | \`research_unity_core_loop\` |"
  echo "| Game feel / juice | \`research_unity_game_feel_juice\` |"
  echo "| ScriptableObject architecture | \`research_unity_scriptable_objec\` |"
  echo "| Scope discipline | \`research_unity_scope_discipline\` |"
  echo "| Place as testimony / memory-witness | \`research_rpg_place_as_testimony\` |"
  echo "| Oath-bearer player fantasy | \`research_rpg_oath_bearer_player\` |"
  echo "| Companion dilemma | \`research_rpg_companion_dilemma\` |"
  echo "| Hit pipeline / combat juice code | \`research_gamedev_hit_pipeline\` |"
  echo "| Quest prompt craft | \`skill_rpg_quest_prompt_craft\` |"
  echo "| Vertical slice checklist | \`skill_unity_vertical_slice_chec\` |"
  echo
  echo "## Sealed units in base"
  echo
  strings "$BASE" 2>/dev/null | rg -o 'acq_(skill|research|chunk)_[A-Za-z0-9_]+' | sort -u | sed 's/^/- `/' | sed 's/$/`/'
} > "$OUT"
echo "SKILL_MAP $OUT"
