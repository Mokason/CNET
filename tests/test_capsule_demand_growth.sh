#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
root=$(mktemp -d /tmp/cnet-demand-growth-XXXXXX)
pid=''
trap 'rc=$?; [[ -z $pid ]] || { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }; (( !rc )) || echo CAPSULE_DEMAND_GROWTH_RED; find "$root" -type d -exec chmod u+rwx -- {} +; rm -rf -- "$root"' EXIT
mkdir -m 700 "$root/packs" "$root/sets" "$root/state" "$root/demand" "$root/queue"
capsules="$root/sets/working"
mkdir -m 700 "$capsules"
printf '{"pattern":"fixture_route","pack":"pack_fixture"}\n' > "$root/packs/ROUTES.jsonl"
start() {
    (cd "$root"; exec env -i PATH="$PATH" CNET_PACKS_ROOT="$root/packs" \
        CNET_MINIMAL_ROOT="$root" CNET_CAPSULE_SETS_DIR="$root/sets" CNET_CAPSULE_STATE_DIR="$root/state" \
        CNET_CAPSULE_CONTROL_SOCK="$root/control.sock" \
        CNET_CAPSULE_DEMAND_DIR="$root/demand" CNET_SOCK="$root/sock" \
        CNET_SELF_ANSWER=0 CNET_TEACHER_ON_MISS=0 CNET_CORE_AUTO_EVOLVE=0 "${CNET_TEST_DAEMON:-$repo/bin/cnetd}") > "$root/daemon.log" 2>&1 & pid=$!
    for ((i=0;i<100;i++)); do [[ -S $root/sock && -S $root/control.sock ]] && return 0; sleep .05; done
    sed -n '1,80p' "$root/daemon.log"; return 1
}
ask() { "$repo/bin/test_capsule_socket_client" "$root/sock" "{\"q\":\"$1\"}"; }
control() { "$repo/bin/cnet_capsulectl" "$root/control.sock" "$@"; }
activate_growth() {
    local status revision digest
    find "$capsules" -mindepth 1 -maxdepth 1 -type d -exec chmod 700 -- {} +
    status=$(control STATUS); revision=${status#* revision=}; revision=${revision%% *}
    status=$(control STAGE "$revision" 1 working) || { printf '%s\n' "$status" >&2; return 1; }
    digest=${status#* staged=}; digest=${digest%% *}
    control ACTIVATE "$revision" "demand-$revision" "$digest"
}
start
ask 'capsule bytes bits 12' | jq -e '.verified == false and (.answer | contains("demand=queued"))' >/dev/null
test "$(find "$root/demand" -name '*.req' | wc -l)" -eq 1
ask 'capsule bytes bits 012' | jq -e '.verified == false and (.answer | contains("demand=existing"))' >/dev/null
test "$(find "$root/demand" -name '*.req' | wc -l)" -eq 1
ask 'capsule bytes bits 1.5' | jq -e '.verified == false and (.answer | contains("demand=") | not)' >/dev/null
printf 'bytes bits 8 11 mul 8 0 255\nu8 masked8 8 8 xor 255 0 255\n' > "$root/policy.tsv"
chmod 600 "$root/policy.tsv"
worker() { CNET_CAPSULE_DEMAND_DIR="$root/demand" CNET_CAPSULE_TOOL_POLICY="$root/policy.tsv" \
    CNET_CAPSULE_QUEUE="$root/queue" CNET_CAPSULES_DIR="$capsules" bash "$repo/scripts/cnet_capsule_acquire_tick.sh"; }
worker
activate_growth
correct=0
for ((x=0;x<32;x++)); do
    ask "convert $x bytes to bits" | jq -e --arg expected "$((x*8))" '.verified and .answer == $expected and .teacher == false' >/dev/null
    correct=$((correct+1))
done
test "$(find "$root/demand" -name '*.req' | wc -l)" -eq 0
# A new block is acquired by the worker, then explicitly activated by the owner.
ask 'capsule bytes bits 40' | jq -e '.verified == false' >/dev/null
worker
activate_growth
ask 'capsule bytes bits 40' | jq -e '.verified and .answer == "320"' >/dev/null
ask 'capsule bytes bits 12' | jq -e '.verified and .answer == "96"' >/dev/null
ask 'capsule u8 masked8 7' | jq -e '.verified == false' >/dev/null
worker
activate_growth
ask 'capsule u8 masked8 7' | jq -e '.verified and .answer == "248"' >/dev/null
# Unknown policy never becomes an answer, and an unsafe policy is refused.
ask 'capsule unknown nowhere 2' | jq -e '.verified == false' >/dev/null
if worker; then exit 1; fi
test "$(find "$capsules" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 3
chmod 666 "$root/policy.tsv"
if worker; then exit 1; fi
kill "$pid"; wait "$pid"; pid=''; start
ask 'capsule bytes bits 12' | jq -e '.verified and .answer == "96"' >/dev/null
printf 'CAPSULE_DEMAND_GROWTH_PASS before_correct=0 after_correct=%d expansion=1 second_domain=1 restart=1 unknown_refused=1 explicit_activation=3\n' "$correct"
