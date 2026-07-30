#!/bin/sh
# benchmark_verdict.sh -- turn a benchmark's recorded verdict into an exit code.
#
# WHY THIS EXISTS. `vision_detection_bench_v2` accepted
# VISION_DETECTION_MECHANISM_PASS *or* ..._WITHHELD as its final condition and
# exited 0 either way, so automation could not tell an earned benchmark result
# from an explicitly unearned one by exit status. WITHHELD is an honest verdict
# and stays honest -- it is not renamed to PASS here, it is given its own
# non-success exit code.
#
# CARDINALITY IS PART OF THE VERDICT. The first version of this script used
# `grep -q PREFIX_PASS`, which is a substring test against the whole file: a log
# containing both a PASS and a WITHHELD line answered PASS, a line reading
# `NOT_PREFIX_PASS` or `PREFIX_PASSED` answered PASS, and a log repeating a
# verdict looked identical to one stating it once. A benchmark that reports two
# different terminal verdicts has not reported a verdict; saying so is the only
# honest answer. Markers are therefore matched ANCHORED at line start and
# followed by whitespace or end of line, and exactly one terminal verdict line
# must be present.
#
#   0  PASS       exactly one PASS line, no other terminal verdict
#   3  WITHHELD   exactly one WITHHELD line; the benchmark declined to claim it
#   4  BLOCKED    exactly one BLOCKED line, or no evidence log at all
#   5  AMBIGUOUS  more than one terminal verdict line
#   1  no verdict in the log (crash, truncation, or a renamed marker)
#
# A separately named evidence-collection target may return 0 while recording
# WITHHELD; the GATE may not.
#
# Usage: sh scripts/benchmark_verdict.sh <log-path> <marker-prefix> [gate-name]

set -u

if [ "$#" -lt 2 ]; then
    printf 'usage: %s <log-path> <marker-prefix> [gate-name]\n' "$0" >&2
    exit 1
fi

LOG=$1
PREFIX=$2
GATE=${3:-$PREFIX}

if [ ! -f "$LOG" ]; then
    printf '%s_BLOCKED reason=no_evidence_log path=%s\n' "$GATE" "$LOG"
    exit 4
fi

# Anchored at line start, terminated by whitespace or end of line. `grep -c`
# counts LINES, which is what cardinality means here.
count_verdict() {
    grep -cE "^${PREFIX}_$1([[:space:]]|\$)" "$LOG" 2>/dev/null || true
}

n_pass=$(count_verdict PASS)
n_withheld=$(count_verdict WITHHELD)
n_blocked=$(count_verdict BLOCKED)
: "${n_pass:=0}" "${n_withheld:=0}" "${n_blocked:=0}"
total=$((n_pass + n_withheld + n_blocked))

if [ "$total" -eq 0 ]; then
    printf '%s_NO_VERDICT reason=no_%s_terminal_marker evidence=%s\n' \
        "$GATE" "$PREFIX" "$LOG"
    exit 1
fi

if [ "$total" -gt 1 ]; then
    printf '%s_AMBIGUOUS pass=%s withheld=%s blocked=%s evidence=%s\n' \
        "$GATE" "$n_pass" "$n_withheld" "$n_blocked" "$LOG"
    printf '%s\n' "A benchmark reporting more than one terminal verdict has not reported a verdict."
    exit 5
fi

if [ "$n_pass" -eq 1 ]; then
    printf '%s_PASS verdict=earned evidence=%s\n' "$GATE" "$LOG"
    exit 0
fi

if [ "$n_withheld" -eq 1 ]; then
    printf '%s_WITHHELD verdict=not_earned evidence=%s\n' "$GATE" "$LOG"
    printf '%s\n' "WITHHELD is not success: the benchmark ran and declined to claim the result."
    exit 3
fi

printf '%s_BLOCKED verdict=could_not_run evidence=%s\n' "$GATE" "$LOG"
exit 4
