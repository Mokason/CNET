#!/bin/bash
# test_pipeline_status.sh -- a producer that prints a gate's PASS marker and
# then dies must not produce a green build.
#
# WHY THIS EXISTS. The 2026-07-30 re-analysis found 35+ recipes of the shape
# `producer | tee log` followed by `grep -q MARKER log`, with no `pipefail`
# anywhere in the Makefile. `tee` exits 0 regardless of what the producer did,
# so a sanitizer failing at teardown, a timeout, a crash after the marker, or a
# late cleanup error all became PASS. The static recipe gate did not catch this
# because it only looked for `|| echo`, `|| true` and `|| :`.
#
# The fixture below is the mutation the report asked for: it prints the exact
# marker a gate greps for and exits 7. Every pipeline shape used by real recipes
# is instantiated with it and must fail.
#
# Usage: bash tests/test_pipeline_status.sh [Makefile-path]
# Exit 0 = pipelines propagate producer status, 1 = a shape false-greens.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MAKEFILE="${1:-$ROOT/Makefile}"

if [ ! -f "$MAKEFILE" ]; then
    printf 'FAIL: Makefile not found at %s\n' "$MAKEFILE"
    exit 1
fi

# Everything this test writes lives under a disposable temp root. It never
# invokes a real gate, never touches a base, and never writes into the repo.
TMP=$(mktemp -d "${TMPDIR:-/tmp}/cnet-pipeline-XXXXXX") || exit 1
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

MARKER="SYNTHETIC_GATE_PASS"

# --- the mutation fixture -------------------------------------------------
cat > "$TMP/false_green_producer.sh" <<'PRODUCER'
#!/bin/sh
# Prints the marker a gate greps for, then fails. Stands in for a sanitizer
# teardown error, a timeout, or a crash after the summary line.
echo "SYNTHETIC_GATE_PASS checks=1 metric=1.000"
exit 7
PRODUCER
chmod +x "$TMP/false_green_producer.sh"

"$TMP/false_green_producer.sh" > "$TMP/producer.out" 2>&1
producer_status=$?
check "$([ "$producer_status" -eq 7 ] && echo 0 || echo 1)" \
    "fixture producer must exit 7 (saw $producer_status)"
grep -q "$MARKER" "$TMP/producer.out"
check $? "fixture producer must print the $MARKER marker before dying"

# --- the shell declaration under test -------------------------------------
# Extract the repo's own SHELL/.SHELLFLAGS lines so the fixture runs under
# exactly the settings the real recipes run under.
DECLARATION=$(grep -E '^(SHELL|\.SHELLFLAGS)[[:space:]]*:?=' "$MAKEFILE")

if [ -z "$DECLARATION" ]; then
    printf 'FAIL: %s declares no SHELL/.SHELLFLAGS; pipelines cannot propagate producer status\n' \
        "$MAKEFILE"
    failures=$((failures + 1))
    checks=$((checks + 1))
fi

printf '%s\n' "$DECLARATION" | grep -q 'pipefail'
check $? "Makefile must enable pipefail so producer status survives a pipe"

# --- recipe shapes --------------------------------------------------------
# Each shape below is copied from a real recipe in the Makefile. `sed` puts a
# literal tab in front of the recipe lines.
emit_makefile() {
    # $1 = output path, $2 = declaration block (may be empty), $3 = recipe body
    {
        printf '%s\n\n' "$2"
        printf 'PRODUCER := %s/false_green_producer.sh\n\n' "$TMP"
        printf 'gate:\n'
        printf '%s\n' "$3" | sed 's/^/\t/'
    } > "$1"
}

run_shape() {
    # $1 = human name, $2 = recipe body, $3 = expected outcome: fail|pass
    name=$1
    body=$2
    want=$3
    emit_makefile "$TMP/Makefile.$name" "$DECLARATION" "$body"
    ( cd "$TMP" && make -f "Makefile.$name" gate ) > "$TMP/$name.log" 2>&1
    status=$?
    if [ "$want" = "fail" ]; then
        check "$([ "$status" -ne 0 ] && echo 0 || echo 1)" \
            "shape '$name' must reject a marker-then-exit-7 producer (exit $status)"
    else
        check "$([ "$status" -eq 0 ] && echo 0 || echo 1)" \
            "shape '$name' must accept an honest producer (exit $status)"
    fi
}

# tee with stderr folded in, then a marker grep -- the dominant shape.
run_shape tee_stderr_grep \
'@$(PRODUCER) 2>&1 | tee run.log
@grep -q SYNTHETIC_GATE_PASS run.log' fail

# tee without stderr redirection.
run_shape tee_plain \
'@$(PRODUCER) | tee run.log' fail

# appending tee, used by the warning-debt style gates.
run_shape tee_append \
'@$(PRODUCER) 2>&1 | tee -a run.log
@grep -q SYNTHETIC_GATE_PASS run.log' fail

# a pipeline whose consumer is a filter rather than tee.
run_shape pipe_grep \
'@$(PRODUCER) 2>&1 | grep -E "^SYNTHETIC"' fail

# timeout-wrapped producer, as the vision gates use.
run_shape timeout_tee \
'@timeout 60 $(PRODUCER) 2>&1 | tee run.log
@grep -q SYNTHETIC_GATE_PASS run.log' fail

# The explicit status-capture shape already propagated; assert it still does,
# so the fix does not quietly become the only thing holding the line.
run_shape status_capture \
'@$(PRODUCER) > run.log 2>&1; status=$$?; \
	cat run.log; test $$status -eq 0 && \
	grep -q SYNTHETIC_GATE_PASS run.log' fail

# Control: an honest producer must still pass every shape, or the gate would be
# unconditionally red and prove nothing.
cat > "$TMP/honest_producer.sh" <<'HONEST'
#!/bin/sh
echo "SYNTHETIC_GATE_PASS checks=1 metric=1.000"
exit 0
HONEST
chmod +x "$TMP/honest_producer.sh"
sed -i "s|false_green_producer.sh|honest_producer.sh|" "$TMP/Makefile.tee_stderr_grep"
( cd "$TMP" && make -f Makefile.tee_stderr_grep gate ) > "$TMP/honest.log" 2>&1
check $? "an honest producer must still pass the tee shape"

# Control: without the declaration the same fixture false-greens. This is the
# defect itself, kept executable so the fix cannot be silently reverted.
emit_makefile "$TMP/Makefile.nopipefail" "" \
'@$(PRODUCER) 2>&1 | tee run.log
@grep -q SYNTHETIC_GATE_PASS run.log'
( cd "$TMP" && make -f Makefile.nopipefail gate ) > "$TMP/nopipefail.log" 2>&1
status=$?
check "$([ "$status" -eq 0 ] && echo 0 || echo 1)" \
    "control: without pipefail the same fixture must false-green (exit $status)"

if [ "$failures" -gt 0 ]; then
    printf 'PIPELINE_STATUS_FAIL checks=%d failures=%d\n' "$checks" "$failures"
    exit 1
fi

printf 'PIPELINE_STATUS_PASS checks=%d shapes=6 producer=marker_then_exit_7\n' "$checks"
exit 0
