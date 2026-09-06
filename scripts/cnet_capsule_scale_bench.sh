#!/usr/bin/env bash
# Run from the repository root. stdout: JSONL only; stderr: progress/evidence path.
set -euo pipefail
umask 077
read -ra counts <<< "${CNET_SCALE_COUNTS:-2 16 64 256}"
repeats=${CNET_SCALE_REPEATS:-3}
[[ $repeats =~ ^([1-9]|1[0-9]|20)$ && ${#counts[@]} -gt 0 ]] || exit 2
previous=0
for count in "${counts[@]}"; do
    [[ $count =~ ^[1-9][0-9]{0,3}$ ]] || exit 2
    (( count >= 2 && count <= 4096 && count % 2 == 0 && count > previous )) || exit 2
    previous=$count
done
repo=$PWD
test -x bin/cnet_capsule_scale_probe
test -x bin/cnet_capsule_core
test -x bin/cnet_capsule_tool
root=$(mktemp -d /tmp/cnet-scale-bench-XXXXXX)
echo "CAPSULE_SCALE_ARTIFACTS $root" >&2
trap 'rc=$?; (( !rc )) || echo "CAPSULE_SCALE_BENCH_RED artifacts=$root" >&2' EXIT
mkdir "$root/capsules" "$root/evidence"
sha256sum bin/cnet_capsule_scale_probe bin/cnet_capsule_core bin/libcnet_capsule_core.so \
    bin/cnet_capsule_tool tools/cnet_capsule_scale_probe.c scripts/cnet_capsule_scale_bench.sh > "$root/sha256.txt"
uname -a > "$root/host.txt"
LC_ALL=C lscpu >> "$root/host.txt"
published=0
tool_calls=0
failure() {
    jq -nc --arg root "$root" --arg reason "$1" --argjson published "$published" \
      --argjson count "$count" --argjson calls "$tool_calls" \
      '{schema_version:1,status:"fail",reason:$reason,artifact_root:$root,
        capsules:$published,target_capsules:$count,acquisition_tool_calls:$calls}' \
      | tee -a "$root/results.jsonl"
}
for count in "${counts[@]}"; do
    echo "CAPSULE_SCALE_STAGE capsules=$count" >&2
    while (( published < count )); do
        group=$((published / 2))
        # Follow flagship's doubled-ID convention: intentional families must
        # remain farther than one edit apart under existing tag governance.
        printf -v group_tag '%03dq%03d' "$group" "$group"
        if (( published % 2 == 0 )); then
            input=alpha$group_tag; output=beta$group_tag; operand=15
        else
            input=beta$group_tag; output=gamma$group_tag; operand=7
        fi
        unit=edge_$published
        rows=$root/evidence/$unit.tsv
        for ((x=0; x<32; x++)); do
            tool_calls=$((tool_calls + 1))
            y=$(bin/cnet_capsule_tool xor "$operand" "$x")
            [[ $y == "$((x ^ operand))" ]] || exit 1
            printf '%d %d\n' "$x" "$y" >> "$rows"
        done
        # env -i prevents a live evaluation file or deployment configuration
        # from influencing this synthetic fixture. All ordinary gates remain.
        if ! bin/cnet_capsule_scale_probe --time \
            timeout 120 env -i PATH="$PATH" LC_ALL=C \
            "$repo/bin/cnet_capsule_core" teach "$root/capsules" "$unit" \
            "$input" "$output" 8 8 verified_tool "$rows" \
            > "$root/evidence/$unit.seconds" 2> "$root/evidence/$unit.log"; then
            failure publication_refused_or_timed_out
            exit 1
        fi
        if ! rg -q '^CAPSULE_HISTORY_PASS ' "$root/evidence/$unit.log" ||
           ! rg -q '^CAPSULE_TEACH_PASS ' "$root/evidence/$unit.log"; then
            failure publication_evidence_invalid
            exit 1
        fi
        published=$((published + 1))
    done
    seconds=$(awk '{s+=$1} END {printf "%.6f", s}' "$root"/evidence/*.seconds)
    rc=0
    bin/cnet_capsule_scale_probe "$root/capsules" "$count" "$repeats" > "$root/probe-$count.json" || rc=$?
    test -s "$root/probe-$count.json"
    jq -c --arg root "$root" --argjson calls "$tool_calls" --argjson rows "$((count * 32))" \
      --argjson seconds "$seconds" \
      '. + {artifact_root:$root,acquisition_tool_calls:$calls,supplied_primitive_rows:$rows,
        cumulative_publication_seconds:$seconds,scope:"synthetic_two_edge_xor_chains"}' \
      "$root/probe-$count.json" | tee -a "$root/results.jsonl"
    (( rc == 0 )) || exit "$rc"
done
