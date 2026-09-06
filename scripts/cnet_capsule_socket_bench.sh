#!/usr/bin/env bash
# Read-only inventory, private daemon; never connect to/restart live services.
set -euo pipefail
[[ $# == 2 && $2 =~ ^[1-9][0-9]{0,2}$ ]] || exit 2
(( $2 <= 128 )) || exit 2
capsules=$(realpath "$1"); groups=$2; repo=$PWD
requests=${CNET_SOCKET_REQUESTS:-128}
[[ $requests =~ ^[1-9][0-9]{0,3}$ ]] && (( requests <= 1024 )) || exit 2
read -ra clients <<< "${CNET_SOCKET_CLIENTS:-1 4 16}"
seen=' '
for n in "${clients[@]}"; do
    [[ $n =~ ^(1|2|4|8|16)$ && $seen != *" $n "* ]] || exit 2
    seen+="$n "
done
umask 077
root=$(mktemp -d /tmp/cnet-socket-bench-XXXXXX); pid=''
trap 'rc=$?; [[ -z $pid ]] || { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }; (( !rc )) || echo "CAPSULE_SOCKET_BENCH_RED artifacts=$root" >&2' EXIT
echo "CAPSULE_SOCKET_ARTIFACTS $root" >&2
mkdir "$root/packs"
printf '{"pattern":"fixture_route","pack":"pack_fixture"}\n' > "$root/packs/ROUTES.jsonl"
(cd "$root"; exec env -i PATH="$PATH" CNET_PACKS_ROOT="$root/packs" CNET_MINIMAL_ROOT="$root" \
  CNET_CAPSULES_DIR="$capsules" CNET_SOCK="$root/sock" CNET_SELF_ANSWER=0 CNET_TEACHER_ON_MISS=0 \
  CNET_CORE_AUTO_EVOLVE=0 "$repo/bin/cnetd") > "$root/daemon.log" 2>&1 & pid=$!
ready=0
for ((i=0;i<100;i++)); do
    if bin/test_capsule_socket_client "$root/sock" '{"q":"capsule unknown missing 0"}' > "$root/ready.json"; then ready=1; break; fi
    kill -0 "$pid"; sleep .05
done
(( ready ))
worker() {
    local worker=$1 concurrency=$2 id group x query rc elapsed good refused wrong transport
    for ((id=worker;id<requests;id+=concurrency)); do
        group=$((id % groups)); x=$((id % 32)); (( id % 8 )) || x=32
        printf -v query '{"q":"capsule alpha%03dq%03d gamma%03dq%03d %d"}' "$group" "$group" "$group" "$group" "$x"
        rc=0
        bin/cnet_capsule_scale_probe --time bin/test_capsule_socket_client "$root/sock" "$query" \
          > "$root/$concurrency-$id.time" 2> "$root/$concurrency-$id.json" || rc=$?
        elapsed=$(< "$root/$concurrency-$id.time"); elapsed=${elapsed:-0}
        good=0; refused=0; wrong=0; transport=0
        if (( rc )); then transport=1
        elif (( x == 32 )); then
            if jq -e '.verified == false and .teacher == false and .skill == "capsule_refusal"' "$root/$concurrency-$id.json" >/dev/null; then refused=1; else wrong=1; fi
        elif jq -e --arg answer "$((x ^ 8))" '.verified == true and .teacher == false and .answer == $answer' "$root/$concurrency-$id.json" >/dev/null; then good=1
        else wrong=1
        fi
        printf '%s\t%d\t%d\t%d\t%d\n' "$elapsed" "$good" "$refused" "$wrong" "$transport" >> "$root/$concurrency-$worker.tsv"
    done
}
for concurrency in "${clients[@]}"; do
    pids=()
    for ((w=0;w<concurrency;w++)); do worker "$w" "$concurrency" & pids+=("$!"); done
    for w in "${pids[@]}"; do wait "$w"; done
    jq -Rsc --argjson clients "$concurrency" --argjson requested "$requests" --arg root "$root" \
      'split("\n")|map(select(length>0)|split("\t")|map(tonumber)) as $rows |
       ($rows|map(.[0])|sort) as $times |
       {schema_version:1,clients:$clients,requested:$requested,completed:($rows|length),
        correct:($rows|map(.[1])|add),refused:($rows|map(.[2])|add),
        wrong_results:($rows|map(.[3])|add),transport_failures:($rows|map(.[4])|add),
        p50_ms:($times[(($times|length)*0.50|ceil)-1]*1000),
        p95_ms:($times[(($times|length)*0.95|ceil)-1]*1000),max_ms:($times[-1]*1000),
        timing_scope:"socket_roundtrip_including_client_process_startup",artifact_root:$root} |
        . + {status:(if .completed == .requested and .wrong_results == 0 and .transport_failures == 0 then "pass" else "fail" end)}' \
      < <(cat "$root/$concurrency-"*.tsv) | tee -a "$root/results.jsonl"
done
jq -es 'all(.[]; .status == "pass")' "$root/results.jsonl" >/dev/null
