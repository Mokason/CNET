#!/usr/bin/env bash
set -euo pipefail
trap 'printf "CAPSULE_FRONTDOOR_RED line=%s\n" "$LINENO"' ERR
repo=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d /tmp/cnet-frontdoor-growth-XXXXXX)
pid=''
cleanup() {
    if [[ -n $pid ]]; then kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; fi
    rm -rf "$tmp"
}
trap cleanup EXIT
mkdir "$tmp/packs" "$tmp/capsules"
printf '{"pattern":"fixture_route","pack":"pack_fixture"}\n' > "$tmp/packs/ROUTES.jsonl"
printf '{"chunk":"capsule minutes seconds credits convert: unverified notes must not override a refusal"}\n' > "$tmp/notes.jsonl"
start() {
    (cd "$tmp"; exec env -i PATH="$PATH" CNET_PACKS_ROOT="$tmp/packs" \
        CNET_MINIMAL_ROOT="$tmp" CNET_CAPSULES_DIR="$tmp/capsules" CNET_SOCK="$tmp/sock" \
        CNET_KNOWLEDGE_PATH="$tmp/notes.jsonl" \
        CNET_SELF_ANSWER=0 CNET_TEACHER_ON_MISS=0 CNET_CORE_AUTO_EVOLVE=0 \
        "$repo/bin/cnetd") > "$tmp/daemon.log" 2>&1 &
    pid=$!
    for ((i=0;i<100;i++)); do [[ -S $tmp/sock ]] && return; sleep .05; done
    sed -n '1,80p' "$tmp/daemon.log"
    printf 'CAPSULE_FRONTDOOR_RED daemon_not_ready\n'; return 1
}
ask() { "$repo/bin/test_capsule_socket_client" "$tmp/sock" "{\"q\":\"$1\"}"; }
start
ask 'capsule minutes seconds 3' | jq -e '.verified == false and .miss == true and (.answer | contains("ABSTAIN"))' >/dev/null
for ((x=0;x<6;x++)); do printf '%d\t%d\n' "$x" "$((x*60))"; done > "$tmp/minutes.tsv"
"$repo/bin/cnet_capsule_core" teach "$tmp/capsules" minute_conversion minutes seconds 3 9 verified_tool "$tmp/minutes.tsv"
correct=0
for ((x=0;x<6;x++)); do
    ask "capsule minutes seconds $x" | jq -e --arg n "$((x*60))" '.source == "LOCAL" and .verified and .answer == $n' >/dev/null
    correct=$((correct+1))
done
ask 'convert 3 minutes to seconds' | jq -e '.verified and .answer == "180"' >/dev/null
ask 'How many seconds in 3 minutes?' | jq -e '.verified and .answer == "180"' >/dev/null
ask 'HOW  MANY seconds IN 3 minutes?' | jq -e '.verified and .answer == "180"' >/dev/null
for query in 'please convert 3 minutes into seconds' 'what is 3 minutes in seconds?' '3 minutes in seconds' 'how many seconds are in 3 minutes?'; do
    ask "$query" | jq -e '.verified and .answer == "180" and .teacher == false' >/dev/null
done
ask 'what is 3.5 minutes in seconds?' | jq -e '.verified == false and .skill == "capsule_clarify" and .teacher == false' >/dev/null
ask 'How Many seconds in -3 minutes?' | jq -e '.verified == false and .skill == "capsule_clarify" and .teacher == false' >/dev/null
ask 'convert 3 minutes to seconds or credits' | jq -e '.verified == false and .miss and (.answer | contains("CLARIFY"))' >/dev/null
ask 'capsule minutes seconds 6' | jq -e '.verified == false and .miss' >/dev/null
# Independent second capsule; intermediate 120 deliberately excluded.
for x in 0 1 3 4 5; do printf '%d\t%d\n' "$((x*60))" "$((x*2))"; done > "$tmp/credits.tsv"
"$repo/bin/cnet_capsule_core" teach "$tmp/capsules" metered_cost seconds credits 9 4 verified_tool "$tmp/credits.tsv"
ask 'capsule minutes credits 3' | jq -e '.verified and .answer == "6"' >/dev/null
# A conflicting learned unit must not poison the serving inventory.
printf '0\t1\n1\t1\n' > "$tmp/conflict.tsv"
if "$repo/bin/cnet_capsule_core" teach "$tmp/capsules" conflicting_conversion minutes seconds 3 9 verified_tool "$tmp/conflict.tsv"; then
    echo CAPSULE_FRONTDOOR_RED conflicting_publication; exit 1
fi
ask 'capsule minutes seconds 1' | jq -e '.verified and .answer == "60"' >/dev/null
ask 'capsule minutes credits 2' | jq -e '.verified == false and .miss' >/dev/null
# A fresh process must reload the same capsule gates, not a remembered answer.
kill "$pid"; wait "$pid"; pid=''; start
ask 'capsule minutes credits 5' | jq -e '.verified and .answer == "10"' >/dev/null
ask 'capsule minutes credits 2' | jq -e '.verified == false and .miss' >/dev/null
ask 'capsule minutes seconds 3.5' | jq -e '.verified == false and .miss' >/dev/null
# Independently verified expansion is a NEW capsule; the old one remains.
for ((x=0;x<6;x++)); do printf '%d\t%d\n' "$((x*60))" "$((x*2))"; done > "$tmp/expanded.tsv"
"$repo/bin/cnet_capsule_core" teach "$tmp/capsules" metered_cost_v2 seconds credits 9 4 verified_tool "$tmp/expanded.tsv"
ask 'capsule minutes credits 2' | jq -e '.verified and .answer == "4"' >/dev/null
ask 'capsule minutes credits 3' | jq -e '.verified and .answer == "6"' >/dev/null
# Corrupt one capsule: the fresh request must refuse the whole load.
printf '\ncorruption\n' >> "$tmp/capsules/minute_conversion/manifest.cknow"
ask 'capsule minutes seconds 3' | jq -e '.verified == false and .miss' >/dev/null
printf 'CAPSULE_FRONTDOOR_PASS before_correct=0 after_correct=%d wrong_certified=0 two_hop=1 restart=1 intermediate_ood_refused=1 expansion=1\n' "$correct"
