#!/usr/bin/env bash
# Reply with C chain-of-thought panel + answer. No Python.
#   scripts/roe_reply.sh "who are you"
#   scripts/roe_reply.sh "who are you" --teacher
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
Q="${1:-}"
TEACHER=0
if [[ "${2:-}" == "--teacher" ]]; then TEACHER=1; fi
if [[ -z "$Q" ]]; then
  echo "usage: $0 \"query\" [--teacher]" >&2
  exit 2
fi
if [[ ! -x bin/roe_chain_think ]]; then
  gcc -std=c11 -Wall -Wextra -O2 -D_POSIX_C_SOURCE=200809L -Iinclude \
    -o bin/roe_chain_think src/cnet_roe_cot.c tools/roe_chain_think.c
fi
if [[ ! -x bin/roe_front_door ]]; then
  gcc -std=c11 -Wall -O2 -D_POSIX_C_SOURCE=200809L -Iinclude -o bin/roe_front_door \
    src/cnet_roe_asi.c src/cnet_roe_net.c src/cnet_asi_improve.c tools/roe_front_door.c -lm -lcurl
fi
export ROE_NO_THOUGHT=1
if [[ "$TEACHER" -eq 1 ]]; then
  exec ./bin/roe_chain_think --teacher "$Q"
else
  exec ./bin/roe_chain_think "$Q"
fi
