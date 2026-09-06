#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Exercise the actual expanded recipe without overwriting ordinary gate logs.
make -s -n -C "$root" capability_cert_runner_test |
    sed "s@logs/capability_cert_runner.log@$work/runner.log@g" > "$work/recipe"
ln -s "$root/tests/fixtures/fake_dotnet_gate.sh" "$work/dotnet"
for mode in failed empty passed; do
    rc=0
    (cd "$root"; PATH="$work:$PATH" CNET_TEST_DOTNET_MODE="$mode" bash "$work/recipe") > "$work/output" 2>&1 || rc=$?
    if [ "$mode" = passed ]; then
        if [ "$rc" -ne 0 ]; then cat "$work/output"; echo CAPABILITY_RUNNER_STATUS_RED; exit 1; fi
    elif [ "$rc" -eq 0 ] || grep -q '^CAPABILITY_CERT_RUNNER_PASS' "$work/runner.log"; then
        cat "$work/output"
        echo "CAPABILITY_RUNNER_STATUS_RED mode=$mode rc=$rc"
        exit 1
    fi
done
echo CAPABILITY_RUNNER_STATUS_PASS
