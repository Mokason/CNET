#!/bin/sh
# test_accumulation_faults_harness.sh -- the fault harness must reject a bench
# that prints the right words and then does the wrong thing.
#
# WHY THIS EXISTS. tests/test_accumulation_faults.sh asserted only that the
# forced-failure line appeared and that the exit status was NOT 99 (the
# ASan exitcode). Everything else passed: exit 0, exit 7, a SIGSEGV after the
# marker, or a `timeout` kill at 124. A harness whose whole job is to prove the
# failure path is clean cannot treat "crashed" and "refused correctly" as the
# same outcome.
#
# The documented contract for an injected failure is exact: the bench prints
#   build FAILED at k=<at>
#   KNOWLEDGE_ACCUMULATION_BENCH_FAIL failures=<n>
# and exits 1. Anything else is a different event and must be reported as one.
#
# Every bench below is a fake written into a mkdtemp root. The real benchmark is
# never built or run here; this tests the HARNESS.
#
# Usage: sh tests/test_accumulation_faults_harness.sh
set -u
HARNESS=tests/test_accumulation_faults.sh
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cnet-accfault-XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

checks=0
failures=0
check() {
    checks=$((checks + 1))
    if [ "$1" -ne 0 ]; then
        printf 'FAIL: %s\n' "$2"
        failures=$((failures + 1))
    fi
}

# Every fake prints exactly what an honest injected failure prints, so the only
# thing distinguishing them is what they do afterwards.
emit() {
    cat <<'SH'
printf '  build FAILED at k=%s (tag mint refusal or admit refusal)\n' "$CNET_ACC_FAIL_AT"
printf 'checks=12 failures=3\n'
printf 'KNOWLEDGE_ACCUMULATION_BENCH_FAIL failures=3\n'
SH
}

make_bench() {  # $1 = name, $2 = trailing behaviour
    {
        printf '#!/bin/sh\n'
        emit
        printf '%s\n' "$2"
    } > "$TMP/$1"
    chmod +x "$TMP/$1"
}

make_bench honest        'exit 1'
make_bench exit_zero     'exit 0'
make_bench exit_wrong    'exit 7'
make_bench crash         'kill -SEGV $$'
make_bench hang          'sleep 60'
make_bench asan_finding  'printf "ERROR: LeakSanitizer: detected memory leaks\n"; exit 1'

# A short budget so the hang case cannot stall this suite. The harness must
# honour it; if it does not, the `timeout` below bounds the damage.
ACC_FAULT_TIMEOUT=3
export ACC_FAULT_TIMEOUT

expect() {  # $1 = bench, $2 = expected harness status (0 accept / 1 refuse), $3 = why
    out=$(timeout 120 sh "$HARNESS" "$TMP/$1" 2>&1)
    status=$?
    if [ "$2" -eq 0 ]; then
        check "$([ "$status" -eq 0 ] && echo 0 || echo 1)" \
            "$3 (harness exit $status, expected 0)"
    else
        check "$([ "$status" -ne 0 ] && echo 0 || echo 1)" \
            "$3 (harness exit $status, expected nonzero)"
    fi
    printf 'ACCFAULT_CASE bench=%-13s harness_exit=%s\n' "$1" "$status"
    LAST_OUT=$out
}

expect honest 0 \
    "a bench that reports the injected failure and exits 1 is ACCEPTED"
expect exit_zero 1 \
    "a bench that prints the failure marker and then exits 0 must be REFUSED"
expect exit_wrong 1 \
    "a bench that exits 7 instead of the documented 1 must be REFUSED"
expect crash 1 \
    "a bench that prints the marker and then segfaults must be REFUSED"
expect hang 1 \
    "a bench that prints the marker and then hangs must be REFUSED"
expect asan_finding 1 \
    "a sanitizer finding on the fault path must be REFUSED"

# The refusals must be distinguishable, not one generic failure.
out=$(timeout 120 sh "$HARNESS" "$TMP/crash" 2>&1)
case "$out" in
    *signal*|*SIG*) check 0 "" ;;
    *) check 1 "a crash must be reported as a signal, not as a wrong exit code" ;;
esac
out=$(timeout 120 sh "$HARNESS" "$TMP/hang" 2>&1)
case "$out" in
    *timed?out*|*timeout*) check 0 "" ;;
    *) check 1 "a timeout must be reported as a timeout" ;;
esac

if [ "$failures" -gt 0 ]; then
    printf 'ACCUMULATION_FAULTS_HARNESS_FAIL checks=%d failures=%d\n' \
        "$checks" "$failures"
    exit 1
fi
printf 'ACCUMULATION_FAULTS_HARNESS_PASS checks=%d exit_code=exact signals=refused timeouts=refused\n' \
    "$checks"
exit 0
