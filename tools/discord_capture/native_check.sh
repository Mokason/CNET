#!/bin/sh
# Disposable native fixtures only; no live collection or model installation.
set -eu
umask 077
capture_source=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
capture_build=$(mktemp -d /tmp/cnet-evidence-native-XXXXXX)
cc -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror -fPIC -shared \
  -I"$capture_source/include" "$capture_source/src/memory/cnet_semantic_cortex.c" \
  "$capture_source/src/memory/cnet_shared_workspace.c" -o "$capture_build/libcapture_intent.so"
cc -std=c11 -D_POSIX_C_SOURCE=200809L -O2 -Wall -Wextra -Werror \
  -I"$capture_source/include" "$capture_source/tools/cnet_capsule_tool.c" \
  "$capture_source/tools/cnet_capsule_tool_plan.c" -o "$capture_build/cnet_capsule_tool"
chmod 500 "$capture_build/libcapture_intent.so" "$capture_build/cnet_capsule_tool"
CNET_EVIDENCE_RUNTIME="$capture_build" "${DISCORD_PYTHON:-python3}" \
  -m unittest discover -s "$capture_source/tools/discord_capture" -p test_native_evidence.py -v
printf 'NATIVE_MAPPING_PASS independently_checked=512 negative=4 fixture_only=1 runtime=%s\n' "$capture_build"
