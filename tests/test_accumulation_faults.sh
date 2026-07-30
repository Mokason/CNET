#!/bin/sh
# test_accumulation_faults.sh -- the accumulation benchmark's fault paths must
# be clean, and they are the paths a green run never takes.
#
# L1 in the adversarial audit: if add_unit failed, the bench kept looping to the
# requested N and could read names[k] it had never written, and several early
# failure returns leaked the BTN they had just allocated. A passing run hides
# both, so CNET_ACC_FAIL_AT forces the failure and a sanitizer looks at what
# happens next.
#
# Usage: sh tests/test_accumulation_faults.sh <asan-built-bench>
set -u
BENCH=${1:?usage: test_accumulation_faults.sh <bench>}
LOG=logs/knowledge_accumulation_faults.log
mkdir -p logs
checks=0
failures=0
check() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then
        printf 'FAIL: %s\n' "$2"
        failures=$((failures + 1))
    fi
}

# The documented contract for an injected failure: the bench reports the forced
# failure, prints KNOWLEDGE_ACCUMULATION_BENCH_FAIL, and exits EXACTLY 1.
#
# The earlier version asserted only `status != 99` -- the ASan exitcode -- so a
# bench that printed the right words and then exited 0, exited 7, segfaulted, or
# was killed by `timeout` all counted as a clean fault path. A harness whose
# whole job is to prove the failure path is clean cannot treat "crashed" and
# "refused correctly" as the same outcome. tests/test_accumulation_faults_harness.sh
# drives this file with fakes that do each of those things.
EXPECTED_STATUS=1
BUDGET=${ACC_FAULT_TIMEOUT:-900}

for at in 0 3 17; do
    ASAN_OPTIONS=detect_leaks=1:exitcode=99 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    CNET_ACC_FAIL_AT="$at" timeout "$BUDGET" "$BENCH" > "$LOG.$at" 2>&1
    status=$?
    cat "$LOG.$at" >> "$LOG"
    grep -q "build FAILED at k=$at" "$LOG.$at"
    check $? "a forced failure at k=$at is reported"
    grep -q "KNOWLEDGE_ACCUMULATION_BENCH_FAIL" "$LOG.$at"
    check $? "the k=$at fault path reports the bench's FAIL verdict"

    # Classify the status before judging it, so a refusal names what happened.
    if [ "$status" -eq 124 ]; then
        check 1 "the k=$at fault path timed out after ${BUDGET}s (exit 124); a hang is not a clean refusal"
    elif [ "$status" -ge 128 ]; then
        check 1 "the k=$at fault path died from signal $((status - 128)) (exit $status); a crash after the marker is not a clean refusal"
    elif [ "$status" -eq 99 ]; then
        check 1 "a sanitizer finding on the k=$at fault path (exit 99)"
    else
        check "$([ "$status" -eq "$EXPECTED_STATUS" ] && echo 0 || echo 1)" \
            "the k=$at fault path exits exactly $EXPECTED_STATUS (exit $status)"
    fi

    if grep -qE 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:' "$LOG.$at"; then
        grep -E 'ERROR: (AddressSanitizer|LeakSanitizer)|runtime error:' "$LOG.$at" | head -3
        check 1 "the k=$at fault path is free of sanitizer findings"
    else
        check 0 ""
    fi
done

if [ "$failures" -gt 0 ]; then
    printf 'ACCUMULATION_FAULTS_FAIL checks=%d failures=%d\n' "$checks" "$failures"
    exit 1
fi
printf 'ACCUMULATION_FAULTS_PASS checks=%d forced_failures=3 sanitizers=asan+ubsan+leak\n' "$checks"
exit 0
