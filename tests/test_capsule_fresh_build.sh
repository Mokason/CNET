#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d /tmp/cnet-fresh-build-XXXXXX)
trap 'rc=$?; rm -rf -- "$scratch"; (( !rc )) || echo CAPSULE_FRESH_BUILD_RED' EXIT
# Resolve the daemon as a file dependency without relying on an old binary.
make -s -C "$root" -n BIN_DIR="$scratch/absent-bin" capsule_socket_bench_contract >"$scratch/graph.log"
make -s -C "$root" -np flagship >"$scratch/flagship.log"
grep -Eq '^flagship: .*libcce[.]a' "$scratch/flagship.log"
# Exercise the standalone tool with neither bin/ nor logs/ present.
cp "$root/Makefile" "$scratch/"
cp -a "$root/mk" "$root/include" "$root/tools" "$root/tests" "$scratch/"
make -s -C "$scratch" capsule_tool
echo CAPSULE_FRESH_BUILD_PASS
