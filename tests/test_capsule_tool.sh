#!/usr/bin/env bash
set -euo pipefail
trap 'rc=$?; (( !rc )) || echo CAPSULE_TOOL_RED' EXIT
tool=./bin/cnet_capsule_tool
[[ $($tool mul 8 12) == 96 ]]
[[ $($tool xor 255 7) == 248 ]]
[[ $($tool mul 1 65535) == 65535 ]]
for args in 'mul 65535 2' 'mul -1 2' 'mul 1.5 2' 'mul 0x10 2' 'div 2 4' 'mul 1 2 extra'; do
    read -ra argv <<< "$args"
    if "$tool" "${argv[@]}"; then exit 1; fi
done
echo CAPSULE_TOOL_PASS
