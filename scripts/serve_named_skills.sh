#!/usr/bin/env bash
# Exercise sealed named skills via soul_host through a tiny C driver OR MCP.
# Writes artifacts/janitor/serve_skills_REPORT.md
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
BASE="${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
OUT="${1:-artifacts/janitor/serve_skills_REPORT.md}"
mkdir -p "$(dirname "$OUT")"

{
  echo "# Named skill serve report"
  echo
  echo "- When: $(date -Iseconds)"
  echo "- Base: \`$BASE\`"
  echo
  echo "## Sealed skill units"
  strings "$BASE" | rg -o 'acq_(skill|research|chunk)_[A-Za-z0-9_]+' | sort -u | while read -r u; do
    echo "- \`$u\`"
  done
  echo
  echo "## Serve exercise (native request probe via bin if present)"
} > "$OUT"

# Prefer a quick Python/ctypes if we have tools - else list only + MCP note
if [[ -x bin/serve_named_skills ]]; then
  ./bin/serve_named_skills "$BASE" | tee -a "$OUT"
else
  {
    echo
    echo "Use MCP after deploy:"
    echo '```'
    echo 'cnet_list_skills'
    echo 'cnet_use_skill skill=research_unity_core_loop query="one verb core loop"'
    echo 'cnet_use_skill skill=research_rpg_place_as_testimony query="place as testimony"'
    echo '```'
  } >> "$OUT"
fi

echo "SERVE_SKILLS_REPORT $OUT"
