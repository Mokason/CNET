#!/usr/bin/env bash
# Content factory: Obsidian / research notes → structured CNET skill queue.
# Scans markdown for ## headings and queues skill/research tags (≤31 chars).
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
BASE="${CNET_BASE_PATH:-$REPO/soul_gemma4v2_final.cnb}"
INBOX="${CNET_GAP_INBOX:-$BASE.inbox}"
VAULT="${CNET_OBSIDIAN_VAULT:-$HOME/Obsidian}"
# Fallback search roots
ROOTS=()
[[ -d "$VAULT" ]] && ROOTS+=("$VAULT")
[[ -d "$HOME/AI/Obsidian" ]] && ROOTS+=("$HOME/AI/Obsidian")
[[ -d "$REPO/docs" ]] && ROOTS+=("$REPO/docs")
# Hermes memory research paths often under Obsidian Memory
for d in "$HOME/.hermes" "$HOME/marble" "$HOME"; do
  [[ -d "$d" ]] || continue
done

K="${CNET_AUTO_LEARN_K:-3}"
W=256
MAX="${CNET_CONTENT_FACTORY_MAX:-24}"
n=0

slug() {
  echo "$1" | tr '[:upper:]' '[:lower:]' | sed -E 's/[^a-z0-9]+/_/g;s/^_|_$//g' | cut -c1-22
}

queue() {
  local goal="$1"
  (( ${#goal} > 31 )) && goal="${goal:0:31}"
  echo "NO_PLAN 1 $W 1 w_cur 1 $W $K $goal" >> "$INBOX"
  echo "queued $goal"
  n=$((n+1))
}

mkdir -p "$(dirname "$INBOX")"
: > /tmp/cf_seen_$$

# Built-in quality exemplars (skill upgrade content)
queue chunk_unity_juice_hitstop
queue chunk_unity_core_verb
queue chunk_rpg_testimony_quest
queue chunk_gamedev_hit_pipeline
queue skill_obsidian_to_cnet

# Scan research markdown if vault exists
if ((${#ROOTS[@]})); then
  while IFS= read -r f; do
    [[ -f "$f" ]] || continue
    # title from filename
    bn=$(basename "$f" .md)
    case "$bn" in
      *Unity*|*unity*) queue "research_$(slug "$bn")" ;;
      *RPG*|*Skyrim*|*Witcher*|*Dragon*) queue "research_$(slug "$bn")" ;;
      *Game*|*game*) queue "research_$(slug "$bn")" ;;
    esac
    # first few ## headings
    rg -N '^## ' "$f" 2>/dev/null | head -5 | while read -r line; do
      h=$(echo "$line" | sed 's/^## //')
      s=$(slug "$h")
      [[ -z "$s" ]] && continue
      echo "research_$s" >> /tmp/cf_seen_$$
    done
    (( n >= MAX )) && break
  done < <(find "${ROOTS[@]}" -type f -name '*.md' 2>/dev/null | rg -i 'research|unity|rpg|skyrim|witcher|game' | head -40)
  if [[ -f /tmp/cf_seen_$$ ]]; then
    sort -u /tmp/cf_seen_$$ | head -n "$MAX" | while read -r g; do
      (( n >= MAX )) && break
      queue "$g"
    done
  fi
fi
rm -f /tmp/cf_seen_$$

# Skill quality notes (human-readable) → learn_from_chat style text file
NOTE=artifacts/janitor/skill_quality_seeds.md
mkdir -p artifacts/janitor
cat > "$NOTE" <<'EOF'
# Skill quality seeds (attach via learn skill=)

## unity juice
Hitstop 2–4 frames on connect; camera kick small; layered SFX; particles on sweet spot only.

## unity core loop
One hero verb fun in 30s; systems orbit the verb; death teaches; retry < 2s.

## rpg testimony
Player is oath-bearer / place-reader. Quests bury lies that feed someone. Freeing truth costs.

## hit pipeline
HitRequest → filters → modifiers → apply → OnDamaged events (numbers + juice + audio).
EOF

echo "CONTENT_FACTORY_OK queued=$n inbox=$INBOX note=$NOTE"
