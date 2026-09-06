#!/usr/bin/env bash
set -euo pipefail
root=$(mktemp -d /tmp/cnet-capsule-curriculum-XXXXXX)
trap 'rc=$?; if ((rc)); then echo CAPSULE_CURRICULUM_RED; fi; rm -rf -- "$root"' EXIT
mkdir -p "$root/queue/conversion" "$root/capsules"
job="$root/queue/conversion"
printf 'minute_conversion minutes seconds 3 9 verified_tool\n' > "$job/request.tsv"
for x in 0 1 2 3 4 5; do printf '%s\t%s\n' "$x" "$((x*60))"; done > "$job/training.tsv"
printf 'minutes seconds 3 179\n' > "$job/evaluation.tsv"
run() { CNET_CAPSULES_DIR="$root/capsules" CNET_CAPSULE_QUEUE="${queue_override:-$root/queue}" CNET_CAPSULE_JOBS_PER_TICK=2 bash scripts/cnet_capsule_curriculum_tick.sh; }
if run; then echo CAPSULE_CURRICULUM_RED invalid_evaluation_promoted; exit 1; fi
test ! -e "$root/capsules/minute_conversion"
for x in 0 1 2 3 4 5; do printf 'minutes seconds %s %s\n' "$x" "$((x*60))"; done > "$job/evaluation.tsv"
run
test -f "$job/receipt.txt"
bin/cnet_capsule_core ask "$root/capsules" 'capsule minutes seconds 3' | grep -q 'verified=1 value=180'
# Crash after publication, before receipt: retry proves the existing capsule.
rm "$job/receipt.txt"
run
grep -q 'reused=1' "$job/receipt.txt"
run | grep -q 'processed=0 failed=0'
mkdir "$root/queue/self_answer"
printf 'own_data seconds credits 9 4 LOCAL\n' > "$root/queue/self_answer/request.tsv"
cp "$job/training.tsv" "$root/queue/self_answer/training.tsv"
cp "$job/evaluation.tsv" "$root/queue/self_answer/evaluation.tsv"
if run; then echo CAPSULE_CURRICULUM_RED self_answer_accepted; exit 1; fi
test ! -e "$root/capsules/own_data"
# Two permanently refused proposals must not monopolize every future tick.
for name in a_failed b_failed z_later; do
    mkdir "$root/queue/$name"
    printf '%s hours ticks 3 9 verified_tool\n' "$name" > "$root/queue/$name/request.tsv"
    cp "$job/training.tsv" "$root/queue/$name/training.tsv"
    printf 'hours ticks 3 179\n' > "$root/queue/$name/evaluation.tsv"
done
printf 'hours ticks 3 180\n' > "$root/queue/z_later/evaluation.tsv"
rm -f "$root/queue/.cursor"
if run; then :; fi
test ! -f "$root/queue/z_later/receipt.txt"
for tick in 2 3; do if run; then :; fi; done
if [[ ! -f $root/queue/z_later/receipt.txt ]]; then
    echo CAPSULE_CURRICULUM_RED failed_prefix_starves_valid_job; exit 1
fi
bin/cnet_capsule_core ask "$root/capsules" 'capsule hours ticks 3' | grep -q 'verified=1 value=180'
# A legitimate producer edit after hashing must not change the evaluated bytes.
queue_override="$root/race_queue"
mkdir -p "$queue_override/race"
printf 'race_unit race_input race_output 1 1 verified_tool\n' > "$queue_override/race/request.tsv"
printf '0\t0\n1\t1\n' > "$queue_override/race/training.tsv"
printf 'race_input race_output 1 1\n' > "$queue_override/race/evaluation.tsv"
expected_digest=$(cd "$queue_override/race" && sha256sum request.tsv training.tsv evaluation.tsv | sha256sum | cut -d' ' -f1)
export CNET_TEST_MUTATE_TRAINING="$queue_override/race/training.tsv"
sha256sum() {
    command sha256sum "$@"
    printf '0\t1\n1\t0\n' > "$CNET_TEST_MUTATE_TRAINING"
}
export -f sha256sum
if ! run; then echo CAPSULE_CURRICULUM_RED evidence_changed_after_hash; exit 1; fi
unset -f sha256sum
unset CNET_TEST_MUTATE_TRAINING
grep -qx "evidence_sha256=$expected_digest" "$queue_override/race/receipt.txt"
if compgen -G "$queue_override/.evidence-*" >/dev/null; then echo CAPSULE_CURRICULUM_RED snapshot_leaked; exit 1; fi
bin/cnet_capsule_core ask "$root/capsules" 'capsule race_input race_output 1' | grep -q 'verified=1 value=1'
# Invalid prefixes consume a bounded scan budget and advance the next tick.
queue_override="$root/bounded_queue"
mkdir -p "$queue_override/a_invalid" "$queue_override/b_invalid" "$queue_override/z_valid"
printf 'bounded_unit bounded_input bounded_output 1 1 verified_tool\n' > "$queue_override/z_valid/request.tsv"
printf '0\t0\n1\t1\n' > "$queue_override/z_valid/training.tsv"
printf 'bounded_input bounded_output 1 1\n' > "$queue_override/z_valid/evaluation.tsv"
scan_log=$(CNET_CAPSULE_SCAN_PER_TICK=2 run || :)
if [[ -e $queue_override/z_valid/receipt.txt ]] || ! grep -q 'examined=2 .*scan_budget=2' <<< "$scan_log"; then
    echo CAPSULE_CURRICULUM_RED unbounded_invalid_prefix; exit 1
fi
if CNET_CAPSULE_SCAN_PER_TICK=2 run; then :; fi
test -f "$queue_override/z_valid/receipt.txt"
for invalid_limit in 0 257 invalid; do
    if CNET_CAPSULE_SCAN_PER_TICK="$invalid_limit" run; then echo CAPSULE_CURRICULUM_RED invalid_scan_limit; exit 1; fi
done
# Moving an unchanged job preserves its evidence identity and skips retraining.
rmdir "$queue_override/a_invalid" "$queue_override/b_invalid"
mv "$queue_override/z_valid" "$queue_override/renamed"
CNET_CAPSULE_SCAN_PER_TICK=2 run | grep -q 'processed=0 failed=0 .*examined=1 scan_budget=2'
echo CAPSULE_CURRICULUM_PASS
