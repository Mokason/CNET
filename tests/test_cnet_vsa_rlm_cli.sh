#!/bin/sh
set -eu
binary=${1:-bin/cnet_vsa_rlm}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
for streams in 0 -1 257 999999999999999999999 abc 16x; do
    rc=0
    "$binary" train --streams "$streams" --train-dir "$scratch/missing" >"$scratch/output" 2>&1 || rc=$?
    if [ "$rc" -ne 2 ] || ! grep -q 'streams must be an integer in \[1,256\]' "$scratch/output"; then
        echo "CNET_VSA_RLM_STREAMS_RED value=$streams"
        exit 1
    fi
done
echo CNET_VSA_RLM_STREAMS_PASS
