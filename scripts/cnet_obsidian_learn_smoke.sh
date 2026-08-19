#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
gcc -std=c11 -Wall -O2 -D_DEFAULT_SOURCE -Iinclude -o bin/test_cnet_obsidian_learn \
  tests/test_cnet_obsidian_learn.c src/cnet_obsidian_learn.c src/cnet_live_miss.c -lm
./bin/test_cnet_obsidian_learn | tee /tmp/obs_unit.out
grep -q CNET_OBSIDIAN_LEARN_PASS /tmp/obs_unit.out
echo CNET_OBSIDIAN_LEARN_PASS
