#!/usr/bin/env bash
set -euo pipefail
report=$(mktemp /tmp/cnet-socket-contract-XXXXXX.jsonl)
CNET_SCALE_COUNTS=2 CNET_SCALE_REPEATS=1 bash scripts/cnet_capsule_scale_bench.sh > "$report"
root=$(jq -sr '.[-1].artifact_root' "$report")
CNET_SOCKET_REQUESTS=16 CNET_SOCKET_CLIENTS='1 4' bash scripts/cnet_capsule_socket_bench.sh "$root/capsules" 1 > "$root/socket.jsonl"
jq -es 'length == 2 and all(.[]; .status == "pass" and .completed == 16 and .correct == 14 and .refused == 2)' "$root/socket.jsonl" >/dev/null
if CNET_SOCKET_CLIENTS='1 1' bash scripts/cnet_capsule_socket_bench.sh "$root/capsules" 1; then exit 1; fi
echo CAPSULE_SOCKET_CONTRACT_PASS
