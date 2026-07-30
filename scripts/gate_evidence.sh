#!/bin/sh
# gate_evidence.sh -- run a gate so its log cannot be mistaken for a stale one.
#
# WHY THIS EXISTS. The re-analysis could not classify the headline logs it found
# as fresh or carried: they live at stable paths under an ignored `logs/` tree
# and carry no run identity, so "the marker is present" proves only that some
# process once wrote it. Reviewers had to treat marker-bearing output as
# untrusted, which is the correct call and also a waste of a real result.
#
# This wrapper does four things:
#   1. deletes any pre-existing log FIRST, so a stale file can never be read as
#      this run's evidence;
#   2. runs the producer with its exit status captured directly (no pipe);
#   3. writes a sidecar <log>.evidence.json binding the gate name, a run UUID,
#      the start time, the commit, the working-tree digest, the assume-unchanged
#      count, the exact argv, the producer's exit status, the SHA-256 of the log
#      it just wrote, and whether the marker was found;
#   4. fails on a non-zero producer status OR a missing marker.
#
# `git status` cannot see assume-unchanged paths, so their count is reported
# rather than left as a silent blind spot in the binding.
#
# Usage: sh scripts/gate_evidence.sh GATE LOG MARKER -- CMD [ARG...]

set -u

if [ "$#" -lt 5 ]; then
    printf 'usage: %s GATE LOG MARKER -- CMD [ARG...]\n' "$0" >&2
    exit 2
fi

GATE=$1; shift
LOG=$1; shift
MARKER=$1; shift
if [ "$1" != "--" ]; then
    printf '%s: expected -- before the command\n' "$0" >&2
    exit 2
fi
shift

mkdir -p "$(dirname "$LOG")" || exit 2

# 1. No stale authority. If the producer never runs, there is no log to read.
rm -f "$LOG" "$LOG.evidence.json"

RUN_ID=$(cat /proc/sys/kernel/random/uuid 2>/dev/null)
[ -n "$RUN_ID" ] || RUN_ID="pid$$-$(date -u +%s 2>/dev/null)"
STARTED=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null)
COMMIT=$(git rev-parse HEAD 2>/dev/null)
[ -n "$COMMIT" ] || COMMIT=unknown
WORKTREE=$(git status --porcelain=v1 2>/dev/null | sha256sum 2>/dev/null | cut -d' ' -f1)
[ -n "$WORKTREE" ] || WORKTREE=unknown
DIRTY=$(git status --porcelain=v1 2>/dev/null | wc -l | tr -d ' ')
ASSUME=$(git ls-files -v 2>/dev/null | grep -c '^[a-z]' || true)

printf 'GATE_RUN gate=%s run_id=%s started=%s commit=%s worktree=%s dirty=%s assume_unchanged=%s\n' \
    "$GATE" "$RUN_ID" "$STARTED" "$COMMIT" "$(printf '%s' "$WORKTREE" | cut -c1-16)" \
    "$DIRTY" "$ASSUME"

# 2. Direct redirection, so the producer's status is the status.
"$@" > "$LOG" 2>&1
STATUS=$?
cat "$LOG"

LOG_SHA=$(sha256sum "$LOG" 2>/dev/null | cut -d' ' -f1)
[ -n "$LOG_SHA" ] || LOG_SHA=unknown
if grep -q -- "$MARKER" "$LOG"; then MARKER_OK=true; else MARKER_OK=false; fi

# 3. The binding, next to the log rather than inside it, so the log's own digest
#    is the digest of exactly what the producer wrote.
{
    printf '{\n'
    printf '  "gate": "%s",\n' "$GATE"
    printf '  "run_id": "%s",\n' "$RUN_ID"
    printf '  "started_at": "%s",\n' "$STARTED"
    printf '  "commit": "%s",\n' "$COMMIT"
    printf '  "worktree_sha256": "%s",\n' "$WORKTREE"
    printf '  "worktree_dirty_files": %s,\n' "$DIRTY"
    printf '  "assume_unchanged_files": %s,\n' "$ASSUME"
    printf '  "command": "%s",\n' "$*"
    printf '  "exit_status": %d,\n' "$STATUS"
    printf '  "evidence_log": "%s",\n' "$LOG"
    printf '  "evidence_sha256": "%s",\n' "$LOG_SHA"
    printf '  "required_marker": "%s",\n' "$MARKER"
    printf '  "marker_present": %s\n' "$MARKER_OK"
    printf '}\n'
} > "$LOG.evidence.json"

# 4. Producer status first; the marker is corroboration, never the authority.
if [ "$STATUS" -ne 0 ]; then
    printf 'GATE_FAIL gate=%s reason=producer_exit_%d evidence=%s\n' \
        "$GATE" "$STATUS" "$LOG.evidence.json"
    exit "$STATUS"
fi
if [ "$MARKER_OK" != "true" ]; then
    printf 'GATE_FAIL gate=%s reason=missing_marker:%s evidence=%s\n' \
        "$GATE" "$MARKER" "$LOG.evidence.json"
    exit 1
fi

printf 'GATE_PASS gate=%s run_id=%s commit=%s evidence_sha256=%s\n' \
    "$GATE" "$RUN_ID" "$COMMIT" "$LOG_SHA"
exit 0
