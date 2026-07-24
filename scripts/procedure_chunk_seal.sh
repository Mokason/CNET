#!/usr/bin/env bash
# Queue structured procedure skills (no tk*q*).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
INBOX="${CNET_GAP_INBOX:-$ROOT/soul_gemma4v2_final.cnb.gaps.txt}"
SEEDS="${1:-$ROOT/references/procedure_seeds.md}"
Q="${INBOX}.procedure_queue"
: > "$Q"
n=0
if [[ -f "$SEEDS" ]]; then
  while IFS= read -r skill; do
    [[ -z "$skill" ]] && continue
    echo "PROCEDURE_SEED skill=$skill" >> "$Q"
    n=$((n+1))
  done < <(awk -F'|' '/chunk_|skill_|research_/ {
    gsub(/^[ \t]+|[ \t]+$/,"",$2);
    if ($2 != "" && $2 != "skill") print $2
  }' "$SEEDS" || true)
fi
if [[ "$n" -eq 0 ]]; then
  for skill in chunk_unity_hit_pipeline chunk_rpg_testimony_quest \
    chunk_unity_core_verb skill_unity_vertical_slice_chec \
    research_rpg_place_as_testimony research_unity_core_loop; do
    echo "PROCEDURE_SEED skill=$skill" >> "$Q"
    n=$((n+1))
  done
fi
echo "PROCEDURE_CHUNK_SEAL_OK count=$n queue=$Q"
