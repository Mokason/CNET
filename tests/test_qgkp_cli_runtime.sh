#!/usr/bin/env bash
set -euo pipefail

repo="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo"

make --no-print-directory cnet_qgkp >/dev/null

set +e
output="$(./bin/cnet_qgkp 2>&1)"
rc=$?
set -e

if [[ $rc -ne 2 ]]; then
    printf 'expected usage exit 2, got %d\n%s\n' "$rc" "$output" >&2
    exit 1
fi
if [[ "$output" != usage:* ]]; then
    printf 'expected CLI usage output, got:\n%s\n' "$output" >&2
    exit 1
fi

echo "QGKP_CLI_RUNTIME_PASS"
