#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-composed-refusal-XXXXXX)
trap 'rc=$?; (( !rc )) || echo CAPSULE_COMPOSED_REFUSAL_RED; rm -rf -- "$root"' EXIT
umask 077
mkdir "$root/demand" "$root/queue" "$root/capsules"
export CNET_CAPSULE_DEMAND_DIR="$root/demand" CNET_CAPSULE_TOOL_POLICY="$root/policy"
export CNET_CAPSULE_QUEUE="$root/queue" CNET_CAPSULES_DIR="$root/capsules"
printf 'alpha beta 16 16 mul 2 0 65535\nbeta alpha 16 16 xor 1 0 65535\n' > "$root/policy"
for ((i=0;i<40;i++)); do printf 'alpha branch_%02d 16 16 xor %d 0 65535\nbranch_%02d alpha 16 16 xor 127 0 65535\n' "$i" "$i" "$i"; done >> "$root/policy"
printf 'alpha nowhere 1\n' > "$root/demand/task.req"
if bash scripts/cnet_capsule_acquire_tick.sh > "$root/log" 2>&1; then exit 1; fi
[[ -f $root/demand/task.req ]]
[[ -z $(find "$root/queue" -mindepth 1 -maxdepth 1 -type d) ]]
grep -q work_or_state_budget "$root/log"
printf 'alpha beta 2 3 mul 1 0 3\nbeta delta 2 2 mul 1 0 3\n' > "$root/policy"
if bash scripts/cnet_capsule_acquire_tick.sh > "$root/log" 2>&1; then exit 1; fi
[[ -f $root/demand/task.req ]]
[[ -z $(find "$root/queue" -mindepth 1 -maxdepth 1 -type d) ]]
# Over-depth paths must not be mistaken for completed acquisitions.
printf 'alpha br0 2 2 mul 1 0 3\n' > "$root/policy"
for ((i=0;i<7;i++)); do printf 'br%d br%d 2 2 mul 1 0 3\n' "$i" "$((i+1))"; done >> "$root/policy"
printf 'br7 nowhere 2 2 mul 1 0 3\n' >> "$root/policy"
if bash scripts/cnet_capsule_acquire_tick.sh > "$root/log" 2>&1; then exit 1; fi
[[ ! -f $root/demand/task.req ]] # no path within supported eight-hop domain
[[ -z $(find "$root/queue" -mindepth 1 -maxdepth 1 -type d) ]]
# Fault-injection uses a private copy, never replaces the workspace executable.
mkdir -p "$root/repo/scripts" "$root/repo/bin"
cp scripts/cnet_capsule_acquire_tick.sh scripts/cnet_capsule_curriculum_tick.sh "$root/repo/scripts/"
cp tests/fixtures/capsule_tool_failure.sh "$root/repo/bin/cnet_capsule_tool"
chmod 700 "$root/repo/bin/cnet_capsule_tool"
printf 'alpha delta 1 1 mul 1 0 0\n' > "$root/policy"
printf 'alpha delta 0\n' > "$root/demand/task.req"
if bash "$root/repo/scripts/cnet_capsule_acquire_tick.sh" > "$root/log" 2>&1; then exit 1; fi
[[ -f $root/demand/task.req ]]
grep -q 'tool_calls=1 failed=1' "$root/log" # failed invocation consumes work
rm "$root/demand/task.req"
echo CAPSULE_COMPOSED_REFUSAL_PASS
