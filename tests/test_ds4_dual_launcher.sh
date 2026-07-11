#!/usr/bin/env bash
set -euo pipefail

# The Hermes terminal intentionally persists exported variables across calls;
# make this test hermetic when a real DS4 staging profile was sourced earlier.
unset CNET_DS4_SERVER CNET_DS4_MODEL CNET_DS4_IDENTITY_FILE \
      CNET_DS4_ALLOW_SHARED_GPU CNET_DS4_API_PORT CNET_DS4_DIST_PORT \
      CNET_DS4_BOUNDED_LAYER_MAP || true

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
LAUNCHER="$ROOT/scripts/run_cnet_ds4_dual.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

bash -n "$LAUNCHER"

CNET_DS4_STATE_DIR="$TMP/state" "$LAUNCHER" status > "$TMP/status.json"
grep -q '"ready":false' "$TMP/status.json"
grep -q '"api_ready":false' "$TMP/status.json"

if CNET_DS4_STATE_DIR="$TMP/state" "$LAUNCHER" verify \
    > "$TMP/verify.out" 2> "$TMP/verify.err"; then
    echo "inactive endpoint unexpectedly verified" >&2
    exit 1
fi
grep -q 'DS4 API is not ready' "$TMP/verify.err"

CNET_DS4_ROOT="$TMP/missing" \
CNET_DS4_STATE_DIR="$TMP/state" \
    "$LAUNCHER" print-plan > "$TMP/plan.txt"
grep -q 'HIP_VISIBLE_DEVICES=1' "$TMP/plan.txt"
grep -q -- '--role worker' "$TMP/plan.txt"
grep -q 'HIP_VISIBLE_DEVICES=0' "$TMP/plan.txt"
grep -q -- '--role coordinator' "$TMP/plan.txt"
grep -q 'DS4_METAL_DISABLE_STREAMING_STATIC_DECODE_MAP=1' "$TMP/plan.txt"
if grep -q 'HIP_VISIBLE_DEVICES=2' "$TMP/plan.txt"; then
    echo "iGPU leaked into launch plan" >&2
    exit 1
fi

if CNET_DS4_ROOT="$TMP/missing" \
   CNET_DS4_STATE_DIR="$TMP/state" \
   CNET_DS4_READY_TIMEOUT_SEC=1 \
       "$LAUNCHER" start > "$TMP/start.out" 2> "$TMP/start.err"; then
    echo "missing DS4 root unexpectedly started" >&2
    exit 1
fi
grep -q 'DS4 server binary not executable' "$TMP/start.err"

mkdir -p "$TMP/fake/gguf"
printf '#!/bin/sh\nexit 0\n' > "$TMP/fake/ds4-server"
chmod +x "$TMP/fake/ds4-server"
printf 'abcdefghijklmnopqrstuvwxyz' > "$TMP/fake/gguf/model.gguf"
CNET_DS4_ROOT="$TMP/fake" \
CNET_DS4_MODEL="$TMP/fake/gguf/model.gguf" \
CNET_DS4_IDENTITY_FILE="$TMP/model.identity" \
CNET_DS4_STATE_DIR="$TMP/state" \
CNET_HASH_CHUNK_BYTES=8 \
    "$LAUNCHER" identity > "$TMP/identity.out"
grep -q '^sha256-tree-v1:' "$TMP/model.identity"
first=$(<"$TMP/model.identity")
CNET_DS4_ROOT="$TMP/fake" \
CNET_DS4_MODEL="$TMP/fake/gguf/model.gguf" \
CNET_DS4_IDENTITY_FILE="$TMP/model.identity" \
CNET_DS4_STATE_DIR="$TMP/state" \
CNET_HASH_CHUNK_BYTES=8 \
    "$LAUNCHER" identity > "$TMP/identity-resume.out"
grep -q 'reused=4 hashed=0' "$TMP/identity-resume.out"
printf '!' >> "$TMP/fake/gguf/model.gguf"
CNET_DS4_ROOT="$TMP/fake" \
CNET_DS4_MODEL="$TMP/fake/gguf/model.gguf" \
CNET_DS4_IDENTITY_FILE="$TMP/model.identity" \
CNET_DS4_STATE_DIR="$TMP/state" \
CNET_HASH_CHUNK_BYTES=8 \
    "$LAUNCHER" identity > "$TMP/identity-changed.out"
second=$(<"$TMP/model.identity")
test "$first" != "$second"

echo "DS4_DUAL_LAUNCHER_PASS"
