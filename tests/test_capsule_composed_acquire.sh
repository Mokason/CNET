#!/usr/bin/env bash
set -euo pipefail
repo=$PWD
root=$(mktemp -d /tmp/cnet-composed-acquire-XXXXXX)
pid=''
trap 'rc=$?; [[ -z $pid ]] || { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }; (( !rc )) || echo CAPSULE_COMPOSED_ACQUIRE_RED; rm -rf -- "$root"' EXIT
umask 077
mkdir "$root/packs" "$root/capsules" "$root/demand" "$root/queue"
printf '{"pattern":"fixture_route","pack":"pack_fixture"}\n' > "$root/packs/ROUTES.jsonl"
printf 'alpha beta 5 5 xor 15 0 31\nbeta gamma 5 6 mul 2 0 31\ngamma delta 6 6 xor 63 0 63\n' > "$root/policy"
export CNET_CAPSULE_DEMAND_DIR="$root/demand" CNET_CAPSULE_TOOL_POLICY="$root/policy"
export CNET_CAPSULE_QUEUE="$root/queue" CNET_CAPSULES_DIR="$root/capsules"
start() {
    (cd "$root"; exec env -i PATH="$PATH" CNET_PACKS_ROOT="$root/packs" \
      CNET_MINIMAL_ROOT="$root" CNET_CAPSULES_DIR="$root/capsules" CNET_CAPSULE_DEMAND_DIR="$root/demand" \
      CNET_SOCK="$root/sock" CNET_SELF_ANSWER=0 CNET_TEACHER_ON_MISS=0 CNET_CORE_AUTO_EVOLVE=0 "$repo/bin/cnetd") > "$root/daemon.log" 2>&1 & pid=$!
    for ((i=0;i<100;i++)); do [[ ! -S $root/sock ]] || return 0; sleep .05; done
    sed -n '1,80p' "$root/daemon.log"; return 1
}
ask() { "$repo/bin/test_capsule_socket_client" "$root/sock" "{\"q\":\"$1\"}"; }
worker() { bash scripts/cnet_capsule_acquire_tick.sh; }
start
ask 'convert 3 alpha to delta' | jq -e '.verified == false and (.answer | contains("demand=queued"))' >/dev/null
worker # A three-hop request must survive the two-new-job tick budget.
test "$(find "$root/queue" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 2
test "$(find "$root/demand" -name '*.req' | wc -l)" -eq 1
ask 'convert 3 alpha to delta' | jq -e '.verified == false and .teacher == false' >/dev/null
worker # Reusing the first two jobs must not starve the third hop.
test "$(find "$root/queue" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 3
test "$(find "$root/demand" -name '*.req' | wc -l)" -eq 0
# End-to-end alpha→delta rows are held out from ALL primitive training jobs.
awk '$2=="alpha" && $3=="delta" {bad=1} END {exit bad ? 1 : 0}' "$root/queue"/*/request.tsv
awk '($2=="alpha" && $3=="gamma") || ($2=="beta" && $3=="delta") {bad=1} END {exit bad ? 1 : 0}' "$root/queue"/*/request.tsv
for ((x=0;x<16;x++)); do
    expected=$(( ((x^15)*2)^63 ))
    ask "HOW MANY delta IN $x alpha?" | jq -e --arg expected "$expected" '.verified and .answer == $expected and .teacher == false' >/dev/null
done
bin/cnet_capsule_core ask "$root/capsules" 'capsule alpha delta 3' | grep -q 'verified=1 value=39 hops=3'
# Targeted suffix expansion unlocks the other half without retraining prefix.
ask 'capsule alpha delta 17' | jq -e '.verified == false' >/dev/null
worker
test "$(find "$root/queue" -mindepth 1 -maxdepth 1 -type d | wc -l)" -eq 4
for ((x=0;x<32;x++)); do
    expected=$(( ((x^15)*2)^63 ))
    ask "convert $x alpha to delta" | jq -e --arg expected "$expected" '.verified and .answer == $expected' >/dev/null
    ask "convert $x alpha to gamma" | jq -e --arg expected "$(((x^15)*2))" '.verified and .answer == $expected' >/dev/null
    ask "convert $x beta to delta" | jq -e --arg expected "$(((x*2)^63))" '.verified and .answer == $expected' >/dev/null
done
cp -a "$root/capsules" "$root/portable"
bin/cnet_capsule_core ask "$root/portable" 'capsule alpha delta 17' | grep -q 'verified=1 value=3 hops=3'
ask 'capsule alpha delta 32' | jq -e '.verified == false' >/dev/null
kill "$pid"; wait "$pid"; pid=''; start
ask 'capsule alpha delta 17' | jq -e '.verified and .answer == "3"' >/dev/null
# Width-valid input near an output boundary must acquire a trimmed block.
printf 'small large 8 8 mul 10 0 255\n' >> "$root/policy"
ask 'capsule small large 25' | jq -e '.verified == false' >/dev/null
worker
ask 'capsule small large 25' | jq -e '.verified and .answer == "250"' >/dev/null
echo CAPSULE_COMPOSED_ACQUIRE_PASS heldout_task_pairs=3 initial_composed_correct=16 expanded_composed_correct=96 primitive_unseen_claim=withheld restart=1 portable=1 composition_jobs=4
