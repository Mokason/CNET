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
# CNET_VERDICT_SCRIPT lets the RED run point at the pre-fix script without
# copying these cases anywhere.
VERDICT=${CNET_VERDICT_SCRIPT:-$ROOT/scripts/benchmark_verdict.sh}

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

# --- cardinality -----------------------------------------------------------
# `grep -q PREFIX_PASS` is a substring test over the whole file. It answered
# PASS for a log that also said WITHHELD, for a log that said PASS twice, and
# for lines that merely CONTAIN the marker. A benchmark reporting more than one
# terminal verdict has not reported a verdict.

expect 'TEST rows=1000
SYNTH_MECHANISM_PASS ap50=0.114518
SYNTH_MECHANISM_WITHHELD refusal=0.0906' 5 'SYNTH_GATE_AMBIGUOUS' \
    'a log claiming both PASS and WITHHELD is ambiguous, not PASS'

expect 'SYNTH_MECHANISM_WITHHELD refusal=0.09
SYNTH_MECHANISM_PASS ap50=0.11' 5 'SYNTH_GATE_AMBIGUOUS' \
    'order does not decide an ambiguous log'

expect 'SYNTH_MECHANISM_PASS one
SYNTH_MECHANISM_PASS two' 5 'SYNTH_GATE_AMBIGUOUS' \
    'a duplicated PASS is ambiguous'

expect 'SYNTH_MECHANISM_WITHHELD first
SYNTH_MECHANISM_WITHHELD second' 5 'SYNTH_GATE_AMBIGUOUS' \
    'a duplicated WITHHELD is ambiguous'

expect 'SYNTH_MECHANISM_PASS ok
SYNTH_MECHANISM_BLOCKED nope' 5 'SYNTH_GATE_AMBIGUOUS' \
    'PASS with BLOCKED is ambiguous'

# --- substring and prefix markers ------------------------------------------
expect 'SYNTH_MECHANISM_PASSED ap50=0.11' 1 'SYNTH_GATE_NO_VERDICT' \
    'PASSED is not PASS'

expect 'NOT_SYNTH_MECHANISM_PASS ap50=0.11' 1 'SYNTH_GATE_NO_VERDICT' \
    'a marker with a prefix in front of it is not the marker'

expect 'note: we hope to reach SYNTH_MECHANISM_PASS next quarter' 1 \
    'SYNTH_GATE_NO_VERDICT' 'a marker mentioned mid-line is not a verdict'

expect 'SYNTH_MECHANISM_PASS_EXTRA ap50=0.11' 1 'SYNTH_GATE_NO_VERDICT' \
    'a marker with a suffix is not the marker'

# A verdict alone on its line, with no trailing fields, is still a verdict.
expect 'SYNTH_MECHANISM_PASS' 0 'SYNTH_GATE_PASS' \
    'a bare verdict line with no fields is accepted'

if [ "$failures" -gt 0 ]; then
    printf 'BENCHMARK_VERDICT_FAIL checks=%d failures=%d\n' "$checks" "$failures"
    exit 1
fi

printf 'BENCHMARK_VERDICT_PASS checks=%d pass=0 withheld=3 blocked=4 no_verdict=1\n' "$checks"
exit 0
