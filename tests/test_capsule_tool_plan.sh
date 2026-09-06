#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-tool-plan-XXXXXX)
trap 'rc=$?; (( !rc )) || echo CAPSULE_TOOL_PLAN_RED; rm -rf -- "$root"' EXIT
umask 077
tool=${CNET_TEST_TOOL:-./bin/cnet_capsule_tool}
printf 'alpha beta 5 5 xor 15 0 31\nbeta gamma 5 6 mul 2 0 31\ngamma delta 6 6 xor 63 0 63\n' > "$root/policy"
$tool plan "$root/policy" alpha delta 3 > "$root/plan"
[[ $(wc -l < "$root/plan") == 3 ]]
tail -n 1 "$root/plan" | grep -qx 'gamma delta 6 6 xor 63 0 63 24 39'
# A short path reaches an uncovered suffix; a longer value-valid path wins.
printf 'alpha beta 2 2 mul 2 1 1\nalpha gamma 2 2 mul 1 1 1\ngamma beta 2 2 xor 2 1 1\nbeta delta 2 2 mul 1 3 3\n' > "$root/policy"
$tool plan "$root/policy" alpha delta 1 > "$root/plan"
[[ $(wc -l < "$root/plan") == 3 ]]
tail -n 1 "$root/plan" | grep -qx 'beta delta 2 2 mul 1 3 3 3 3'
printf 'alpha gamma 2 2 xor 1 0 3\ngamma alpha 2 2 xor 1 0 3\n' > "$root/policy"
$tool plan "$root/policy" alpha alpha 1 > "$root/plan"
[[ $(wc -l < "$root/plan") == 2 ]]
refused() { if $tool plan "$root/policy" alpha delta 1 > "$root/plan"; then exit 1; fi; [[ ! -s $root/plan ]]; }
refused # unreachable positive cycle terminates
printf 'alpha beta 2 3 mul 1 0 3\nbeta delta 2 2 mul 1 0 3\n' > "$root/policy"
refused # typed width mismatch
printf 'alpha delta 2 2 mul 1 2 3\n' > "$root/policy"
refused # requested value outside policy
printf 'alpha delta 2 2 mul 65535 0 3\n' > "$root/policy"
refused # output cannot fit
printf 'alpha delta 2 2 mul 1 0 3\nalpha delta 2 2 xor 1 0 3\n' > "$root/policy"
refused # duplicate domain policy
printf 'alpha delta 2 2 mul 1 0 3\nmalformed\n' > "$root/policy"
refused # validate whole policy before emitting any path
printf 'alpha delta 2 2 mul 1 0 3' > "$root/policy"
refused # truncated policy line must not be silently ignored
printf 'alpha delta 8 8 mul 10 0 255\n' > "$root/policy"
$tool plan "$root/policy" alpha delta 25 > "$root/plan"
grep -qx 'alpha delta 8 8 mul 10 0 25 25 250' "$root/plan"
if $tool plan "$root/policy" alpha delta 25 > /dev/full 2>/dev/null; then exit 1; fi
printf 'alpha delta 8 4 xor 255 0 255\n' > "$root/policy"
$tool plan "$root/policy" alpha delta 250 > "$root/plan"
grep -qx 'alpha delta 8 4 xor 255 240 255 250 5' "$root/plan"
# Positive cycles generate many values; prove bounded refusal without output.
printf 'alpha beta 16 16 mul 2 0 65535\nbeta alpha 16 16 xor 1 0 65535\n' > "$root/policy"
for ((i=0;i<40;i++)); do printf 'alpha branch_%02d 16 16 xor %d 0 65535\nbranch_%02d alpha 16 16 xor 127 0 65535\n' "$i" "$i" "$i"; done >> "$root/policy"
if $tool plan "$root/policy" alpha nowhere 1 > "$root/plan" 2> "$root/error"; then exit 1; else status=$?; fi
[[ $status == 2 && ! -s $root/plan ]]
grep -q work_or_state_budget "$root/error"
echo CAPSULE_TOOL_PLAN_PASS
