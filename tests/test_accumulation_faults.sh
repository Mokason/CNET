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

for at in 0 3 17; do
    ASAN_OPTIONS=detect_leaks=1:exitcode=99 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    CNET_ACC_FAIL_AT="$at" timeout 900 "$BENCH" > "$LOG.$at" 2>&1
    status=$?
    cat "$LOG.$at" >> "$LOG"
    grep -q "build FAILED at k=$at" "$LOG.$at"
    check $? "a forced failure at k=$at is reported"
    check "$([ "$status" -ne 99 ] && echo 0 || echo 1)" \
        "no sanitizer finding on the k=$at fault path (exit $status)"
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
