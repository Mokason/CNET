#!/bin/sh
# test_benchmark_verdict.sh -- WITHHELD must not share PASS's exit semantics.
#
# The vision benchmark gate accepted VISION_DETECTION_MECHANISM_PASS *or*
# ..._WITHHELD as its final condition and exited 0 either way. This gate pins
# the corrected mapping, and pins that the word WITHHELD is still reported --
# the fix is a distinct exit code, not a renamed verdict.
#
# Every log this test reads is synthetic and lives under a disposable temp root.
# It never runs the benchmark and never reads real evidence.
#
# Usage: sh tests/test_benchmark_verdict.sh
# Exit 0 = verdict mapping is correct, 1 = a verdict maps to the wrong status.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERDICT="$ROOT/scripts/benchmark_verdict.sh"

if [ ! -f "$VERDICT" ]; then
    printf 'FAIL: %s not found\n' "$VERDICT"
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/cnet-verdict-XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

checks=0
failures=0

expect() {
    # $1 = log body (or NONE for a missing log), $2 = expected exit,
    # $3 = expected marker substring, $4 = description
    body=$1
    want=$2
    want_marker=$3
    description=$4
    log="$TMP/bench.log"
    rm -f "$log"
    if [ "$body" != "NONE" ]; then
        printf '%s\n' "$body" > "$log"
    fi
    out=$(sh "$VERDICT" "$log" SYNTH_MECHANISM SYNTH_GATE 2>&1)
    status=$?
    checks=$((checks + 1))
    if [ "$status" -ne "$want" ]; then
        printf 'FAIL: %s -- expected exit %s, got %s\n' "$description" "$want" "$status"
        printf '      output: %s\n' "$out"
        failures=$((failures + 1))
        return
    fi
    checks=$((checks + 1))
    case "$out" in
        *"$want_marker"*) ;;
        *)
            printf 'FAIL: %s -- expected marker %s, got: %s\n' \
                "$description" "$want_marker" "$out"
            failures=$((failures + 1))
            ;;
    esac
}

expect 'TEST rows=1000
SYNTH_MECHANISM_PASS ap50=0.114518' 0 'SYNTH_GATE_PASS' \
    'an earned PASS exits 0'

expect 'TEST rows=1000
SYNTH_MECHANISM_WITHHELD refusal=0.0906 floor=0.30' 3 'SYNTH_GATE_WITHHELD' \
    'WITHHELD exits 3, not 0'

expect 'SYNTH_MECHANISM_BLOCKED reason=cnu1_rejects_continuous_exemplars' 4 \
    'SYNTH_GATE_BLOCKED' 'BLOCKED exits 4, not 0'

expect 'TEST rows=1000
the producer died before printing any verdict' 1 'SYNTH_GATE_NO_VERDICT' \
    'a log with no verdict marker fails'

expect NONE 4 'SYNTH_GATE_BLOCKED' 'a missing evidence log is BLOCKED, never PASS'

# PASS must win over an earlier WITHHELD line only when it is genuinely present;
# a log that contains WITHHELD alone must never be read as PASS.
expect 'SYNTH_MECHANISM_WITHHELD first
SYNTH_MECHANISM_WITHHELD second' 3 'SYNTH_GATE_WITHHELD' \
    'repeated WITHHELD stays WITHHELD'

if [ "$failures" -gt 0 ]; then
    printf 'BENCHMARK_VERDICT_FAIL checks=%d failures=%d\n' "$checks" "$failures"
    exit 1
fi

printf 'BENCHMARK_VERDICT_PASS checks=%d pass=0 withheld=3 blocked=4 no_verdict=1\n' "$checks"
exit 0
