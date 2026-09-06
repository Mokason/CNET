#!/usr/bin/env bash
# Live asks for finite domains installed in the certified-core profile.
# A missing covered row can enqueue a demand under that profile's existing
# approved policy before this script fails. It never starts acquisition itself.
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
socket=${1:?usage: cnet_verify_live_capsules.sh SOCKET}
client=${CNET_SOCKET_CLIENT:-$repo/bin/test_capsule_socket_client}
correct=0
ask() { "$client" "$socket" "{\"q\":\"$1\"}"; }
for ((x=0;x<32;x++)); do
    for rule in bytes:bits:8 u8:masked8:xor minutes:seconds:60 minutes:frames_24fps:1440 seconds:frames_24fps:24; do
        IFS=: read -r input output operation <<< "$rule"
        if [[ $operation == xor ]]; then expected=$((x ^ 255)); else expected=$((x * operation)); fi
        ask "capsule $input $output $x" | jq -e --arg answer "$expected" \
          '.verified == true and .teacher == false and .answer == $answer' >/dev/null
        correct=$((correct + 1))
    done
done
for q in 'please convert 3 minutes into seconds' 'what is 3 minutes in seconds?' '3 minutes in seconds' 'how many seconds are in 3 minutes?'; do
    ask "$q" | jq -e '.verified == true and .teacher == false and .answer == "180"' >/dev/null
done
ask 'capsule bytes bits 256' | jq -e '.verified == false and .teacher == false and .skill == "capsule_refusal"' >/dev/null
ask 'what is 3.5 minutes in seconds?' | jq -e '.verified == false and .teacher == false and .skill == "capsule_clarify"' >/dev/null
printf 'LIVE_CAPSULE_SEQUENCE_PASS covered=%d grammar=4 refusals=2 wrong_certified=0\n' "$correct"
