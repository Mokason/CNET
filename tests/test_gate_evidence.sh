#!/bin/bash
# test_gate_evidence.sh -- a gate's log must belong to the run that claims it.
#
# WHY THIS EXISTS. The re-analysis could not classify the headline logs it found
# as fresh or carried: stable paths under an ignored `logs/` tree, no run
# identity, so "the marker is present" proved only that some process once wrote
# it. Reviewers correctly refused to count them as evidence.
#
# scripts/gate_evidence.sh is the fix. These are its negatives: a stale log left
# behind by a previous run must not be readable as this run's evidence, a
# producer that prints the marker and then dies must fail, and a producer that
# succeeds without printing the marker must fail too.
#
# Everything runs under a mkdtemp root. No real gate, no real log.
#
# Usage: bash tests/test_gate_evidence.sh
# Exit 0 = evidence is run-bound, 1 = a stale or unmarked log was accepted.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WRAPPER="$ROOT/scripts/gate_evidence.sh"

if [ ! -f "$WRAPPER" ]; then
    printf 'FAIL: %s not found\n' "$WRAPPER"
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/cnet-gateev-XXXXXX") || exit 1
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

LOG="$TMP/gate.log"
MARKER="SYNTHETIC_GATE_PASS"

cat > "$TMP/good.sh" <<'SH'
#!/bin/sh
echo "SYNTHETIC_GATE_PASS checks=3"
exit 0
SH
cat > "$TMP/marker_then_die.sh" <<'SH'
#!/bin/sh
echo "SYNTHETIC_GATE_PASS checks=3"
exit 7
SH
cat > "$TMP/silent.sh" <<'SH'
#!/bin/sh
echo "ran, but said nothing a gate could check"
exit 0
SH
cat > "$TMP/absent.sh" <<'SH'
#!/bin/sh
exit 3
SH
chmod +x "$TMP"/*.sh

run() {
    # $1 = producer, sets OUT and STATUS
    OUT=$(cd "$ROOT" && sh "$WRAPPER" synthetic_gate "$LOG" "$MARKER" -- "$1" 2>&1)
    STATUS=$?
}

# --- 1. honest producer -----------------------------------------------------
run "$TMP/good.sh"
check "$([ "$STATUS" -eq 0 ] && echo 0 || echo 1)" \
    "an honest producer passes (exit $STATUS)"
case "$OUT" in *GATE_PASS*) ;; *)
    check 1 "an honest run reports GATE_PASS" ;;
esac
check "$([ -f "$LOG.evidence.json" ] && echo 0 || echo 1)" \
    "an honest run writes an evidence binding"
grep -q '"marker_present": true' "$LOG.evidence.json" 2>/dev/null
check $? "the binding records that the marker was found"
grep -q '"exit_status": 0' "$LOG.evidence.json" 2>/dev/null
check $? "the binding records the producer's exit status"
grep -qE '"run_id": "[0-9a-fA-F-]{8,}"' "$LOG.evidence.json" 2>/dev/null
check $? "the binding carries a run id"
grep -qE '"evidence_sha256": "[0-9a-f]{64}"' "$LOG.evidence.json" 2>/dev/null
check $? "the binding carries the digest of the log it wrote"
grep -q '"assume_unchanged_files"' "$LOG.evidence.json" 2>/dev/null
check $? "the binding discloses the assume-unchanged count"

FIRST_RUN=$(grep '"run_id"' "$LOG.evidence.json")

# Two runs of the same gate must not share a run id, or the binding says nothing.
run "$TMP/good.sh"
SECOND_RUN=$(grep '"run_id"' "$LOG.evidence.json")
check "$([ "$FIRST_RUN" != "$SECOND_RUN" ] && echo 0 || echo 1)" \
    "each run gets its own run id"

# --- 2. a stale log must not be inherited ----------------------------------
# The previous run left a passing log at exactly this path. A producer that now
# fails to start must not be able to borrow it.
printf '%s from a previous run\n' "$MARKER" > "$LOG"
run "$TMP/absent.sh"
check "$([ "$STATUS" -ne 0 ] && echo 0 || echo 1)" \
    "a failing producer cannot inherit a stale passing log (exit $STATUS)"
grep -q "$MARKER" "$LOG" 2>/dev/null
check "$([ $? -ne 0 ] && echo 0 || echo 1)" \
    "the stale log content is gone, not merely ignored"

# --- 3. marker then non-zero exit ------------------------------------------
run "$TMP/marker_then_die.sh"
check "$([ "$STATUS" -eq 7 ] && echo 0 || echo 1)" \
    "a producer that prints the marker then exits 7 fails with 7 (exit $STATUS)"
case "$OUT" in *"reason=producer_exit_7"*) ;; *)
    check 1 "the failure names the producer's exit status" ;;
esac

# --- 4. success without the marker -----------------------------------------
run "$TMP/silent.sh"
check "$([ "$STATUS" -ne 0 ] && echo 0 || echo 1)" \
    "a silent producer fails even though it exited 0 (exit $STATUS)"
case "$OUT" in *"reason=missing_marker"*) ;; *)
    check 1 "the failure names the missing marker" ;;
esac
grep -q '"marker_present": false' "$LOG.evidence.json" 2>/dev/null
check $? "the binding records the missing marker"

if [ "$failures" -gt 0 ]; then
    printf 'GATE_EVIDENCE_FAIL checks=%d failures=%d\n' "$checks" "$failures"
    exit 1
fi

printf 'GATE_EVIDENCE_PASS checks=%d stale=refused marker_then_exit=refused silent=refused\n' \
    "$checks"
exit 0
