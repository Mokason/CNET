#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-acquire-security-XXXXXX)
trap 'rc=$?; (( !rc )) || echo CAPSULE_ACQUIRE_SECURITY_RED; if [[ ${CNET_TEST_KEEP_TEMP:-0} == 1 ]]; then echo "$root"; else rm -rf -- "$root"; fi' EXIT
umask 077
mkdir "$root/demand" "$root/queue" "$root/capsules"
export CNET_CAPSULE_DEMAND_DIR="$root/demand" CNET_CAPSULE_TOOL_POLICY="$root/policy"
export CNET_CAPSULE_QUEUE="$root/queue" CNET_CAPSULES_DIR="$root/capsules"
worker() { bash scripts/cnet_capsule_acquire_tick.sh; }
refused() { if worker; then exit 1; fi; }
printf 'alpha beta 8 8 mul 1 0 255\n' > "$root/policy"
mkfifo "$root/demand/.acquire.lock"
if timeout 2 bash scripts/cnet_capsule_acquire_tick.sh; then status=0; else status=$?; fi
[[ $status == 2 ]] # unsafe lock is a refusal, never a hung tick
rm "$root/demand/.acquire.lock"
printf 'alpha beta 16 16 mul 65535 0 65535\n' > "$root/policy"
printf 'alpha beta 2\n' > "$root/demand/a.req"
refused # output overflow creates no job/capsule
[[ -z $(find "$root/queue" -mindepth 1 -maxdepth 1 -type d) ]]
[[ -z $(find "$root/capsules" -mindepth 1 -maxdepth 1 -type d) ]]
printf 'alpha beta 8 8 xor 255 0 255\n' > "$root/policy"
printf 'alpha beta 2\nextra\n' > "$root/demand/a.req"
refused
rm "$root/demand/a.req"
ln -s "$root/policy" "$root/demand/a.req"
refused
rm "$root/demand/a.req"
printf 'alpha beta 8 8 command 1 0 255\n' > "$root/policy"
refused
printf 'alpha beta 8 8 mul 1 0 255\nalpha beta 8 8 mul 2 0 255\n' > "$root/policy"
refused
printf 'alpha beta 8 8 xor 255 0 255\n' > "$root/policy"
# 64 unsupported demands cannot starve a later supported request forever.
for ((i=0;i<64;i++)); do printf 'unknown nowhere %d\n' "$i" > "$root/demand/a$i.req"; done
printf 'alpha beta 2\n' > "$root/demand/z.req"
refused
[[ -f $root/demand/z.req ]]
worker # terminal unsupported requests free admission slots; valid job advances
[[ ! -f $root/demand/z.req ]]
bin/cnet_capsule_core ask "$root/capsules" 'capsule alpha beta 2' | grep -q 'verified=1 value=253'
# Simulate crash after durable job publication, before removing the demand.
printf 'alpha beta 2\n' > "$root/demand/z.req"
worker
test "$(find "$root/queue" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 1
test "$(find "$root/capsules" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 1
[[ ! -f $root/demand/z.req ]]
# A full unsupported spool must recover admission space on the next tick.
for ((i=0;i<256;i++)); do printf 'unknown nowhere %d\n' "$i" > "$root/demand/a$i.req"; done
refused
test "$(find "$root/demand" -name '*.req' | wc -l)" -eq 192
for ((i=0;i<256;i++)); do rm -f -- "$root/demand/a$i.req"; done
# Producer must not outgrow the existing curriculum consumer's queue bound.
for ((i=0;i<4095;i++)); do mkdir "$root/queue/fixture_$i"; done
printf 'alpha beta 34\n' > "$root/demand/z.req"
refused
[[ -f $root/demand/z.req ]]
test "$(find "$root/queue" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 4096
echo CAPSULE_ACQUIRE_SECURITY_PASS
