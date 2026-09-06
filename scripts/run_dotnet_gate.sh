#!/bin/bash
# A zero-exit test command with no executed tests is not evidence.
set -eu
marker=$1
shift
output=$(mktemp)
trap 'rm -f "$output"' EXIT
rc=0
LC_ALL=C "$@" > "$output" 2>&1 || rc=$?
cat "$output"
[ "$rc" -eq 0 ] || exit "$rc"
if ! grep -Eq 'Passed: *[1-9][0-9]*,.*Total: *[1-9][0-9]*' "$output"; then
    echo 'DOTNET_GATE_NO_EXECUTED_TESTS' >&2
    exit 1
fi
printf '%s\n' "$marker"
