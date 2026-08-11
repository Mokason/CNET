#!/usr/bin/env bash
# Reply with visible 0-token thinking + answer (front_door).
#   scripts/roe_reply.sh "who are you"
#   scripts/roe_reply.sh "..." --md
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
Q="${1:-}"
STYLE=panel
if [[ "${2:-}" == "--md" ]]; then STYLE=md; fi
if [[ -z "$Q" ]]; then
  echo "usage: $0 \"query\" [--md]" >&2
  exit 2
fi
export ROE_NO_THOUGHT=1
export ROE_REPLY_QUERY="$Q"
if [[ ! -x bin/roe_front_door ]]; then
  gcc -std=c11 -Wall -O2 -D_POSIX_C_SOURCE=200809L -Iinclude -o bin/roe_front_door \
    src/cnet_roe_asi.c src/cnet_roe_net.c src/cnet_asi_improve.c tools/roe_front_door.c -lm -lcurl
fi
# Capture answer from front door, render thinking panel
TMP=$(mktemp)
./bin/roe_front_door ask "$Q" >"$TMP" 2>&1 || true
python3 scripts/cnet_reply_think.py --query "$Q" --wrap-stdin --style "$STYLE" <"$TMP"
rm -f "$TMP"
