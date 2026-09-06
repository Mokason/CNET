#!/usr/bin/env bash
# Opt-in synthetic capacity, guarded replay and final ordinary publication.
set -euo pipefail
umask 077
count=${CNET_LARGE_CAPSULES:-4096}
[[ $count =~ ^[1-9][0-9]{0,3}$ ]] && (( count >= 2 && count <= 4096 && count % 2 == 0 )) || exit 2
root=$(mktemp -d /tmp/cnet-large-inventory-XXXXXX)
trap 'rc=$?; (( !rc )) || echo "CAPSULE_LARGE_INVENTORY_RED artifacts=$root" >&2' EXIT
echo "CAPSULE_LARGE_ARTIFACTS $root"
mkdir "$root/capsules"
CNET_SCALE_COUNTS=2 CNET_SCALE_REPEATS=1 bash scripts/cnet_capsule_scale_bench.sh > "$root/seed.jsonl"
seed=$(jq -er '.artifact_root + "/capsules"' "$root/seed.jsonl")
sha256sum bin/libcnet_capsule_core.so bin/cnet_capsule_core bin/test_capsule_large_inventory \
  src/serve/cnet_capsule_core.c tests/test_capsule_large_inventory.c > "$root/sha256.txt"
bin/test_capsule_large_inventory "$seed" "$root/capsules" "$count" | tee "$root/probe.log"
if (( count == 4096 )); then
    mv "$root/capsules/edge_4096" "$root/excess"
fi
# Exercise ordinary publication at N-1 -> N, in addition to bulk import.
last=$((count - 1)); group=$((last / 2))
mv "$root/capsules/edge_$last" "$root/last-seed"
printf -v tag '%03dq%03d' "$group" "$group"
for ((x=0; x<32; x++)); do
    y=$(bin/cnet_capsule_tool xor 7 "$x")
    [[ $y == "$((x ^ 7))" ]]
    printf '%d %d\n' "$x" "$y" >> "$root/rows.tsv"
done
env -i PATH="$PATH" LC_ALL=C bin/cnet_capsule_core teach "$root/capsules" "edge_$last" \
  "beta$tag" "gamma$tag" 8 8 verified_tool "$root/rows.tsv" > "$root/publication.out" 2> "$root/publication.log"
rg '^CAPSULE_HISTORY_PASS |^CAPSULE_TEACH_PASS ' "$root/publication.out"
rg -q '^CAPSULE_HISTORY_PASS ' "$root/publication.out"
rg -q '^CAPSULE_TEACH_PASS ' "$root/publication.out"
bin/cnet_capsule_core ask "$root/capsules" "capsule alpha$tag gamma$tag 3" | tee "$root/final-ask.log"
rg -q 'value=11' "$root/final-ask.log"
echo "CAPSULE_LARGE_PUBLICATION_PASS boundary=$last-to-$count"
