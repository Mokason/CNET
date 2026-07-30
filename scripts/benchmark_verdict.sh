#!/bin/sh
# benchmark_verdict.sh -- turn a benchmark's recorded verdict into an exit code.
#
# WHY THIS EXISTS. `vision_detection_bench_v2` accepted
# VISION_DETECTION_MECHANISM_PASS *or* ..._WITHHELD as its final condition and
# exited 0 either way, so automation could not tell an earned benchmark result
# from an explicitly unearned one by exit status. WITHHELD is an honest verdict
# and must stay honest -- it is not renamed to PASS here, it is given its own
# non-success exit code.
#
#   0  PASS      the benchmark earned its verdict
#   3  WITHHELD  the benchmark ran and declined to claim the result
#   4  BLOCKED   the benchmark could not run at all
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

if grep -q "${PREFIX}_PASS" "$LOG"; then
    printf '%s_PASS verdict=earned evidence=%s\n' "$GATE" "$LOG"
    exit 0
fi

if grep -q "${PREFIX}_WITHHELD" "$LOG"; then
    printf '%s_WITHHELD verdict=not_earned evidence=%s\n' "$GATE" "$LOG"
    printf '%s\n' "WITHHELD is not success: the benchmark ran and declined to claim the result."
    exit 3
fi

if grep -q "${PREFIX}_BLOCKED" "$LOG"; then
    printf '%s_BLOCKED verdict=could_not_run evidence=%s\n' "$GATE" "$LOG"
    exit 4
fi

printf '%s_NO_VERDICT reason=no_%s_marker evidence=%s\n' "$GATE" "$PREFIX" "$LOG"
exit 1
