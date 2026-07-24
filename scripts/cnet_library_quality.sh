#!/usr/bin/env bash
# Library quality raise: structured procedure queue + gh_ prune list + pin map.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BASE="${CNET_BASE_PATH:-$ROOT/soul_gemma4v2_final.cnb}"
OUT_DIR="${CNET_JANITOR_OUT:-$ROOT/artifacts/janitor}"
mkdir -p "$OUT_DIR" logs

echo "# Library quality report" > "$OUT_DIR/LIBRARY_QUALITY.md"
echo "- when: $(date -Iseconds)" >> "$OUT_DIR/LIBRARY_QUALITY.md"
echo "- base: \`$BASE\`" >> "$OUT_DIR/LIBRARY_QUALITY.md"
echo >> "$OUT_DIR/LIBRARY_QUALITY.md"

# 1) Procedure seeds → queue
bash scripts/procedure_chunk_seal.sh
echo "## Procedure queue" >> "$OUT_DIR/LIBRARY_QUALITY.md"
if [[ -f "${CNET_GAP_INBOX:-$BASE.gaps.txt}.procedure_queue" ]]; then
  wc -l < "${CNET_GAP_INBOX:-$BASE.gaps.txt}.procedure_queue" | awk '{print "- seeds:", $1}' >> "$OUT_DIR/LIBRARY_QUALITY.md"
  # Promote seeds into learn-friendly lines for operators / MCP skill=
  : > "$OUT_DIR/procedure_learn_batch.txt"
  while read -r line; do
    skill="${line#PROCEDURE_SEED skill=}"
    [[ -z "$skill" ]] && continue
    echo "cnet_learn_from_chat skill=$skill" >> "$OUT_DIR/procedure_learn_batch.txt"
  done < "${CNET_GAP_INBOX:-$BASE.gaps.txt}.procedure_queue"
  echo "- learn batch: \`$OUT_DIR/procedure_learn_batch.txt\`" >> "$OUT_DIR/LIBRARY_QUALITY.md"
fi

# 2) Skill census from base strings
echo >> "$OUT_DIR/LIBRARY_QUALITY.md"
echo "## Skill census (string scan)" >> "$OUT_DIR/LIBRARY_QUALITY.md"
if [[ -f "$BASE" ]]; then
  total=$(strings "$BASE" 2>/dev/null | rg -o 'acq_(skill|research|chunk)_[A-Za-z0-9_]+' | sort -u | wc -l || echo 0)
  gh=$(strings "$BASE" 2>/dev/null | rg -o 'acq_skill_gh_[A-Za-z0-9_]+' | sort -u | wc -l || echo 0)
  good=$(strings "$BASE" 2>/dev/null | rg -o 'acq_(research|chunk|skill_(unity|rpg|obsidian))[A-Za-z0-9_]*' | sort -u | wc -l || echo 0)
  echo "- named skill-like units: $total" >> "$OUT_DIR/LIBRARY_QUALITY.md"
  echo "- gh_ noise: $gh" >> "$OUT_DIR/LIBRARY_QUALITY.md"
  echo "- research/chunk/unity/rpg-ish: $good" >> "$OUT_DIR/LIBRARY_QUALITY.md"
  strings "$BASE" 2>/dev/null | rg -o 'acq_skill_gh_[A-Za-z0-9_]+' | sort -u > "$OUT_DIR/gh_prune_candidates.txt" || true
  strings "$BASE" 2>/dev/null | rg -o 'acq_(research|chunk)_[A-Za-z0-9_]+' | sort -u > "$OUT_DIR/pins_recommended.txt" || true
  echo "- wrote gh_prune_candidates.txt + pins_recommended.txt" >> "$OUT_DIR/LIBRARY_QUALITY.md"
fi

# 3) Pin file for janitor
mkdir -p "$OUT_DIR/pins"
if [[ -f "$OUT_DIR/pins_recommended.txt" ]]; then
  head -40 "$OUT_DIR/pins_recommended.txt" > "$OUT_DIR/pins/PINNED_SKILLS.txt"
fi

# 4) Serve map refresh if possible
if [[ -x scripts/write_skill_map.sh ]]; then
  bash scripts/write_skill_map.sh >> "$OUT_DIR/LIBRARY_QUALITY.md" 2>&1 || true
elif [[ -x scripts/serve_named_skills.sh ]]; then
  bash scripts/serve_named_skills.sh "$OUT_DIR/serve_skills_REPORT.md" 2>&1 || true
fi

echo >> "$OUT_DIR/LIBRARY_QUALITY.md"
echo "CNET_LIBRARY_QUALITY_PASS" >> "$OUT_DIR/LIBRARY_QUALITY.md"
echo "CNET_LIBRARY_QUALITY_PASS report=$OUT_DIR/LIBRARY_QUALITY.md"
