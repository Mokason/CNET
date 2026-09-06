#!/usr/bin/env bash
set -euo pipefail
trap 'rc=$?; (( !rc )) || echo CAPSULE_SCALE_BENCH_RED >&2' EXIT
test -x bin/cnet_capsule_scale_probe
root=$(mktemp -d /tmp/cnet-scale-test-XXXXXX)
mkdir "$root/empty"
bin/cnet_capsule_scale_probe --time sh -c 'echo child-output' > "$root/time" 2> "$root/child"
awk '$1 > 0 {ok=1} END {exit !ok}' "$root/time"
test "$(< "$root/child")" = child-output
rc=0
bin/cnet_capsule_scale_probe --time sh -c 'exit 7' > "$root/time" || rc=$?
test "$rc" -eq 7
if bin/cnet_capsule_scale_probe --time /nonexistent/cnet-scale-command > "$root/time"; then exit 1; fi
if bin/cnet_capsule_scale_probe --time true > /dev/full; then exit 1; fi
if bin/cnet_capsule_scale_probe "$root/empty" 2 1 > "$root/empty.json"; then exit 1; fi
jq -e '.status == "fail" and .unexpected_abstentions > 0' "$root/empty.json" >/dev/null
for count in 0 1 257 4097 4098 -2 2junk; do
    if bin/cnet_capsule_scale_probe "$root/empty" "$count" 1; then exit 1; fi
done
for repeats in 0 -1 21 1junk; do
    if bin/cnet_capsule_scale_probe "$root/empty" 2 "$repeats"; then exit 1; fi
done
CNET_CAPSULE_EVAL_FILE=/nonexistent/production-evaluation CNET_SCALE_COUNTS='2 4' \
  CNET_SCALE_REPEATS=1 bash scripts/cnet_capsule_scale_bench.sh > "$root/results.jsonl"
jq -es 'length == 2 and ([.[].capsules] == [2,4]) and all(.[];
  .schema_version == 1 and .status == "pass" and
  .unique_covered_queries == (.capsules * 48) and .correct == (.capsules * 48) and
  .refused == (.capsules * 1.5 + 1) and
  .wrong_certified == 0 and .unexpected_abstentions == 0 and
  .acquisition_tool_calls == (.capsules * 32) and .supplied_primitive_rows == (.capsules * 32) and
  .heldout_composed_pairs == (.capsules / 2) and .load_samples == 3 and
  .cumulative_publication_seconds > 0 and .energy_cost == null and
  .resident_ask_ms.p95 >= .resident_ask_ms.p50 and .reload_ask_ms.p95 > 0 and
  .monetary_cost == null)' "$root/results.jsonl" >/dev/null
inventory=$(jq -sr '.[-1].artifact_root + "/capsules"' "$root/results.jsonl")
# Misdeclaring a larger fixture must fail; no answers may be assumed present.
if bin/cnet_capsule_scale_probe "$inventory" 6 1 > "$root/mismatch.json"; then exit 1; fi
jq -e '.status == "fail"' "$root/mismatch.json" >/dev/null
if bin/cnet_capsule_scale_probe "$inventory" 2 1 > "$root/mismatch-small.json"; then exit 1; fi
jq -e '.status == "fail" and .observed_capsules == 4' "$root/mismatch-small.json" >/dev/null
if bin/cnet_capsule_scale_probe "$inventory" 4 1 > /dev/full; then exit 1; fi
# Exercise failure evidence without depending on a permanent capacity ceiling.
mkdir "$root/fail-bin"
ln -s /usr/bin/false "$root/fail-bin/timeout"
if PATH="$root/fail-bin:$PATH" CNET_SCALE_COUNTS=2 \
    bash scripts/cnet_capsule_scale_bench.sh > "$root/failure.jsonl"; then exit 1; fi
jq -e '.status == "fail" and .capsules == 0 and .acquisition_tool_calls == 32' "$root/failure.jsonl" >/dev/null
failed_root=$(jq -r .artifact_root "$root/failure.jsonl")
cmp "$root/failure.jsonl" "$failed_root/results.jsonl"
mkdir "$root/empty-success-bin"
ln -s /usr/bin/true "$root/empty-success-bin/timeout"
if PATH="$root/empty-success-bin:$PATH" CNET_SCALE_COUNTS=2 \
    bash scripts/cnet_capsule_scale_bench.sh > "$root/empty-success.jsonl"; then exit 1; fi
jq -e '.status == "fail" and .reason == "publication_evidence_invalid"' "$root/empty-success.jsonl" >/dev/null
failed_root=$(jq -r .artifact_root "$root/empty-success.jsonl")
cmp "$root/empty-success.jsonl" "$failed_root/results.jsonl"
for counts in '1' '2 2' '4 2' '4098' '2junk'; do
    if CNET_SCALE_COUNTS="$counts" bash scripts/cnet_capsule_scale_bench.sh; then exit 1; fi
done
echo CAPSULE_SCALE_BENCH_PASS
