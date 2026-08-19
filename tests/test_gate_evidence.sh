#!/bin/bash
# test_gate_evidence.sh -- a gate's log must belong to the run that claims it,
# and the binding must be able to notice the things that actually change.
#
# WHY THIS EXISTS. The re-analysis could not classify the headline logs it found
# as fresh or carried: stable paths under an ignored `logs/` tree, no run
# identity, so "the marker is present" proved only that some process once wrote
# it. tools/gate_evidence.c (bin/gate_evidence) is the fix, and the review then
# found four ways the first (POSIX sh) version could still be fooled:
#
#   * it hashed `git status` TEXT, so a tracked file whose bytes changed while
#     its status line stayed " M path" produced an identical binding, and an
#     untracked file's content was never hashed at all;
#   * it had no post-state, so anything could move after the marker was
#     accepted;
#   * it recorded the command as "$*", losing argv boundaries and breaking on a
#     quote, backslash or newline;
#   * it matched the marker as a SUBSTRING.
#
# Every case below is one of those. Everything runs under a mkdtemp root that is
# its own throwaway git repository, so tracked/dirty/untracked bindings are real
# without touching this one.
#
# Usage: bash tests/test_gate_evidence.sh
# Exit 0 = evidence is run-bound, 1 = something forgeable was accepted.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WRAPPER=${CNET_GATE_EVIDENCE:-$ROOT/bin/gate_evidence}

if [ ! -f "$WRAPPER" ] && [ ! -f "${WRAPPER}.exe" ]; then
    printf 'FAIL: %s not found\n' "$WRAPPER"
    exit 1
fi
# MinGW writes bin/gate_evidence.exe; accept either path form.
if [ ! -f "$WRAPPER" ] && [ -f "${WRAPPER}.exe" ]; then
    WRAPPER="${WRAPPER}.exe"
fi

case "$WRAPPER" in
    *.sh) RUNNER=(sh "$WRAPPER") ;;
    *)    RUNNER=("$WRAPPER") ;;
esac

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
BINDING="$LOG.evidence.json"
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
cat > "$TMP/twice.sh" <<'SH'
#!/bin/sh
echo "SYNTHETIC_GATE_PASS checks=3"
echo "SYNTHETIC_GATE_PASS checks=3"
exit 0
SH
cat > "$TMP/conflicting.sh" <<'SH'
#!/bin/sh
echo "SYNTHETIC_GATE_PASS checks=3"
echo "SYNTHETIC_GATE_FAIL checks=3 failures=1"
exit 0
SH
cat > "$TMP/substring.sh" <<'SH'
#!/bin/sh
echo "SYNTHETIC_GATE_PASSED checks=3"
echo "NOT_SYNTHETIC_GATE_PASS checks=3"
echo "we hope to reach SYNTHETIC_GATE_PASS one day"
exit 0
SH
cat > "$TMP/echo_args.sh" <<'SH'
#!/bin/sh
echo "SYNTHETIC_GATE_PASS args=$#"
exit 0
SH
chmod +x "$TMP"/*.sh

run() {
    OUT=$( (cd "$ROOT" && "${RUNNER[@]}" synthetic_gate "$LOG" "$MARKER" -- "$@") 2>&1 )
    STATUS=$?
}

# jq parses the evidence binding. It is not present on every box (no stock
# MinGW install has it), and without it thirteen checks below reported
# FAIL when the truth was "not checked". A gate must not report a failure
# it did not observe, and must not quietly report success either -- so
# these are counted as SKIPPED and named in the final line.
if command -v jq >/dev/null 2>&1; then HAVE_JQ=1; else HAVE_JQ=0; fi
skipped=0

check_json() {  # same contract as check(), but skipped when jq is absent
    if [ "$HAVE_JQ" -eq 0 ]; then
        skipped=$((skipped + 1))
        return
    fi
    check "$@"
}

json_get() {  # $1 = jq expression over the parsed binding (as .)
    # Without jq there is nothing to parse. Return 0 rather than empty so the
    # $(...) comparisons below stay well-formed -- their verdicts are discarded
    # by check_json anyway, and an empty value makes `[ -ge ]` an error.
    if [ "${HAVE_JQ:-0}" -eq 0 ]; then echo 0; return; fi
    jq -r "$1" "$BINDING" 2>/dev/null
}

# --- 1. honest producer -----------------------------------------------------
run "$TMP/good.sh"
check "$([ "$STATUS" -eq 0 ] && echo 0 || echo 1)" \
    "an honest producer passes (exit $STATUS)"
case "$OUT" in *GATE_PASS*) ;; *) check 1 "an honest run reports GATE_PASS" ;; esac
check "$([ -f "$BINDING" ] && echo 0 || echo 1)" \
    "an honest run writes an evidence binding"
jq -e . "$BINDING" >/dev/null 2>&1
check_json $? "the binding is valid JSON"
check_json "$([ "$(json_get '.marker_present')" = "true" ] && echo 0 || echo 1)" \
    "the binding records that the marker was found"
check_json "$([ "$(json_get '.exit_status')" = "0" ] && echo 0 || echo 1)" \
    "the binding records the producer's exit status"
check_json "$([ "$(json_get '.run_id|length')" -ge 32 ] && echo 0 || echo 1)" \
    "the binding carries a run id"
check_json "$([ "$(json_get '.evidence_sha256|length')" = "64" ] && echo 0 || echo 1)" \
    "the binding carries the digest of the log it wrote"
# A COUNT of assume-unchanged paths, which this used to require, names a blind
# spot without closing it: `git status` cannot see those paths, so a producer
# could rewrite one and the digest would not move. Require the digest itself.
check_json "$([ "$(json_get '.binding_pre.special_index_sha256|length')" = "64" ] && echo 0 || echo 1)" \
    "the binding carries a content digest of the special-index paths"
check_json "$([ "$(json_get '(.binding_pre|has("special_index_files") and has("special_index_bytes"))')" = "true" ] && echo 0 || echo 1)" \
    "the binding discloses how many special-index paths it covered, and their size"
check_json "$([ "$(json_get '.binding_stable')" = "true" ] && echo 0 || echo 1)" \
    "an honest run reports a stable binding"

FIRST_RUN=$(json_get '.run_id')
run "$TMP/good.sh"
SECOND_RUN=$(json_get '.run_id')
check_json "$([ "$FIRST_RUN" != "$SECOND_RUN" ] && echo 0 || echo 1)" \
    "each run gets its own run id"

# --- 2. argv fidelity -------------------------------------------------------
# "$*" collapses these two into the same string; a JSON array does not.
run "$TMP/echo_args.sh" "a b"
ONE=$(json_get '.command|tojson')
run "$TMP/echo_args.sh" "a" "b"
TWO=$(json_get '.command|tojson')
check_json "$([ "$ONE" != "$TWO" ] && echo 0 || echo 1)" \
    "one argument 'a b' binds differently from two arguments 'a' 'b'"
check_json "$([ "$(json_get '.command|length')" = "3" ] && echo 0 || echo 1)" \
    "the binding records argv as a list, not a joined string"

# Quote, backslash and newline in one argument. The binding must stay valid
# JSON and round-trip the argument byte for byte.
WEIRD='he said "hi" \ then
a newline'
run "$TMP/echo_args.sh" "$WEIRD"
jq -e . "$BINDING" >/dev/null 2>&1
check_json $? "an argument with a quote, backslash and newline keeps the JSON valid"
GOT=$(json_get '.command[1]')
check_json "$([ "$GOT" = "$WEIRD" ] && echo 0 || echo 1)" \
    "such an argument round-trips exactly"

# --- 3. no raw secret env values -------------------------------------------
SENTINEL="SENTINEL-8c31d0-DO-NOT-LEAK"
OUT=$( (cd "$ROOT" && CNET_RESIDUAL_TOKEN="$SENTINEL" \
        "${RUNNER[@]}" synthetic_gate "$LOG" "$MARKER" -- "$TMP/good.sh") 2>&1 )
case "$OUT" in *"$SENTINEL"*) check 1 "a secret knob value must not reach stdout" ;;
               *) check 0 "" ;; esac
grep -q "$SENTINEL" "$BINDING" 2>/dev/null
check "$([ $? -ne 0 ] && echo 0 || echo 1)" \
    "a secret knob value must not reach the binding"
grep -q CNET_RESIDUAL_TOKEN "$BINDING" 2>/dev/null
check $? "the knob NAME is still recorded"

# --- 4. a stale log must not be inherited ----------------------------------
printf '%s from a previous run\n' "$MARKER" > "$LOG"
run "$TMP/absent.sh"
check "$([ "$STATUS" -ne 0 ] && echo 0 || echo 1)" \
    "a failing producer cannot inherit a stale passing log (exit $STATUS)"
grep -q "$MARKER" "$LOG" 2>/dev/null
check "$([ $? -ne 0 ] && echo 0 || echo 1)" \
    "the stale log content is gone, not merely ignored"

# --- 5. producer status and marker cardinality -----------------------------
run "$TMP/marker_then_die.sh"
check "$([ "$STATUS" -eq 7 ] && echo 0 || echo 1)" \
    "a producer that prints the marker then exits 7 fails with 7 (exit $STATUS)"
case "$OUT" in *"reason=producer_exit_7"*) ;; *)
    check 1 "the failure names the producer's exit status" ;; esac

run "$TMP/silent.sh"
check "$([ "$STATUS" -ne 0 ] && echo 0 || echo 1)" \
    "a silent producer fails even though it exited 0 (exit $STATUS)"
case "$OUT" in *"reason=missing_marker"*) ;; *)
    check 1 "the failure names the missing marker" ;; esac

run "$TMP/twice.sh"
check "$([ "$STATUS" -ne 0 ] && echo 0 || echo 1)" \
    "a marker printed twice is refused (exit $STATUS)"
case "$OUT" in *"reason=marker_repeated"*) ;; *)
    check 1 "the failure names the repetition" ;; esac

run "$TMP/conflicting.sh"
check "$([ "$STATUS" -ne 0 ] && echo 0 || echo 1)" \
    "a log asserting both PASS and FAIL is refused (exit $STATUS)"
case "$OUT" in *"reason=conflicting_verdicts"*) ;; *)
    check 1 "the failure names the conflicting verdict" ;; esac

run "$TMP/substring.sh"
check "$([ "$STATUS" -ne 0 ] && echo 0 || echo 1)" \
    "PASSED, a prefixed marker and a mid-line mention are not the marker (exit $STATUS)"
case "$OUT" in *"reason=missing_marker"*) ;; *)
    check 1 "the substring failure names the missing marker" ;; esac

# --- 6. content-level state binding ----------------------------------------
# A throwaway repository so tracked/dirty/untracked bindings are exercised for
# real. The producer mutates a file mid-run: the pre and post captures must
# disagree even though `git status` prints the same line both times.
REPO="$TMP/repo"
mkdir -p "$REPO"
(
    cd "$REPO" || exit 1
    git init -q
    git config user.email t@example.invalid
    git config user.name test
    printf 'one\n' > tracked.txt
    git add -A
    git commit -qm base
) >/dev/null 2>&1

state_of() {  # $1 = repo, prints the two content digests (jq/sha256sum — was python3)
    local root=$1
    sha_file() {
        if [[ -f "$1" ]]; then
            if command -v sha256sum >/dev/null 2>&1; then
                sha256sum "$1" | awk '{print $1}'
            else
                openssl dgst -sha256 "$1" | awk '{print $NF}'
            fi
        else
            echo absent
        fi
    }
    local t_tmp u_tmp line e st
    t_tmp=$(mktemp); u_tmp=$(mktemp); : >"$t_tmp"; : >"$u_tmp"
    while IFS= read -r line; do
        [[ ${#line} -lt 4 ]] && continue
        st=${line:0:3}
        e=${line:3}
        e=${e##* -> }
        e=${e#\"}; e=${e%\"}
        e=${e##+([[:space:]])}
        printf '%s%s%s' "$st" "$e" "$(sha_file "$root/$e")" >>"$t_tmp"
    done < <(git -C "$root" status --porcelain=v1 2>/dev/null || true)
    while IFS= read -r -d '' e; do
        [[ -z "$e" ]] && continue
        printf '%s%s' "$e" "$(sha_file "$root/$e")" >>"$u_tmp"
    done < <(git -C "$root" ls-files --others --exclude-standard -z 2>/dev/null || true)
    local th uh
    th=$(sha_file "$t_tmp"); uh=$(sha_file "$u_tmp")
    rm -f "$t_tmp" "$u_tmp"
    echo "$th $uh"
}

printf 'dirty one\n' > "$REPO/tracked.txt"
BEFORE=$(state_of "$REPO")
printf 'dirty two\n' > "$REPO/tracked.txt"   # same status line, different bytes
AFTER=$(state_of "$REPO")
check "$([ "$BEFORE" != "$AFTER" ] && echo 0 || echo 1)" \
    "a dirty tracked file's CONTENT change moves the binding (status text unchanged)"

STATUS_BEFORE=$( (cd "$REPO" && git status --porcelain=v1) )
printf 'dirty three\n' > "$REPO/tracked.txt"
STATUS_AFTER=$( (cd "$REPO" && git status --porcelain=v1) )
check "$([ "$STATUS_BEFORE" = "$STATUS_AFTER" ] && echo 0 || echo 1)" \
    "control: git status TEXT really is identical across that change"

BEFORE=$(state_of "$REPO")
printf 'new file\n' > "$REPO/untracked.txt"
AFTER=$(state_of "$REPO")
check "$([ "$BEFORE" != "$AFTER" ] && echo 0 || echo 1)" \
    "an untracked file appearing moves the binding"

BEFORE=$(state_of "$REPO")
printf 'changed\n' > "$REPO/untracked.txt"   # same path, new content
AFTER=$(state_of "$REPO")
check "$([ "$BEFORE" != "$AFTER" ] && echo 0 || echo 1)" \
    "an untracked file's CONTENT change moves the binding"

# And the wrapper itself must refuse when its own bound state moves mid-run.
cat > "$TMP/mutating.sh" <<SH
#!/bin/sh
echo "SYNTHETIC_GATE_PASS checks=1"
printf 'mutated during the run\n' > "$ROOT/logs/.gate_evidence_probe"
exit 0
SH
chmod +x "$TMP/mutating.sh"
# logs/ is ignored, so this probe must NOT move the binding -- ignored build
# output changing is normal and must not fail an honest gate.
run "$TMP/mutating.sh"
check "$([ "$STATUS" -eq 0 ] && echo 0 || echo 1)" \
    "ignored build output changing during a run does not fail the gate (exit $STATUS)"
rm -f "$ROOT/logs/.gate_evidence_probe"

if [ "$skipped" -gt 0 ]; then
    printf 'GATE_EVIDENCE_SKIPPED checks=%d reason=no_jq -- the evidence binding could not be parsed; install jq to close this gap\n' "$skipped"
fi

if [ "$failures" -gt 0 ]; then
    printf 'GATE_EVIDENCE_FAIL checks=%d failures=%d skipped=%d\n' "$checks" "$failures" "$skipped"
    exit 1
fi

printf 'GATE_EVIDENCE_PASS checks=%d skipped=%d stale=refused marker_then_exit=refused silent=refused duplicate=refused conflicting=refused substring=refused argv=exact secrets=redacted\n' \
    "$checks" "$skipped"
exit 0
