#!/bin/bash
# test_gate_evidence.sh -- a gate's log must belong to the run that claims it,
# and the binding must be able to notice the things that actually change.
#
# WHY THIS EXISTS. The re-analysis could not classify the headline logs it found
# as fresh or carried: stable paths under an ignored `logs/` tree, no run
# identity, so "the marker is present" proved only that some process once wrote
# it. scripts/gate_evidence.py is the fix, and the review then found four ways
# the first (POSIX sh) version could still be fooled:
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
WRAPPER=${CNET_GATE_EVIDENCE:-$ROOT/scripts/gate_evidence.py}

if [ ! -f "$WRAPPER" ]; then
    printf 'FAIL: %s not found\n' "$WRAPPER"
    exit 1
fi

# Resolve a working Python instead of hardcoding `python3`.
#
# On Windows/MinGW `python3` is usually NOT the interpreter: it resolves to the
# Microsoft Store "App execution alias" stub, which prints an install
# advertisement to stdout and exits non-zero. Every python3 call in this file
# then failed, and the gate reported 28/36 checks failing for a reason that had
# nothing to do with evidence binding -- indistinguishable, from the exit code
# alone, from the gate catching a real forgery. Probe for one that actually
# runs, and say so plainly if none does.
PY=""
for _cand in python3 python py; do
    if command -v "$_cand" >/dev/null 2>&1 &&
       "$_cand" -c 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1; then
        PY="$_cand"
        break
    fi
done
if [ -z "$PY" ]; then
    printf 'FAIL: no working Python 3 found (tried python3, python, py)\n'
    printf '      this gate cannot run; not reporting a pass it did not earn.\n'
    exit 1
fi

case "$WRAPPER" in
    *.py) RUNNER=("$PY" "$WRAPPER") ;;
    *)    RUNNER=(sh "$WRAPPER") ;;
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

# How to launch a producer script.
#
# gate_evidence.py spawns the producer with subprocess.run(argv, shell=False).
# On POSIX the kernel honours the `#!/bin/sh` shebang, so the script path alone
# is a valid argv[0]. Windows CreateProcess does NOT read shebangs and cannot
# execute a .sh at all, chmod +x notwithstanding -- every producer here died
# with exit 127, which is indistinguishable from the gate correctly catching a
# broken producer. Prepend an explicit interpreter there.
#
# argv-length expectations are derived from this, never hardcoded: the whole
# point of the checks below is that argv boundaries survive, so silently
# changing the argv the test sends while leaving a literal `3` in the assertion
# would make the check pass for the wrong reason.
case "$(uname -s 2>/dev/null || echo unknown)" in
    MINGW*|MSYS*|CYGWIN*) PRODUCER_PREFIX=(sh) ;;
    *)                    PRODUCER_PREFIX=() ;;
esac
PREFIX_N=${#PRODUCER_PREFIX[@]}

run() {
    OUT=$( (cd "$ROOT" && "${RUNNER[@]}" synthetic_gate "$LOG" "$MARKER" \
            -- "${PRODUCER_PREFIX[@]}" "$@") 2>&1 )
    STATUS=$?
}

json_get() {  # $1 = python expression over the parsed binding
    "$PY" -c "
import json,sys
d=json.load(open(sys.argv[1]))
print($1)
" "$BINDING" 2>/dev/null
}

# --- 1. honest producer -----------------------------------------------------
run "$TMP/good.sh"
check "$([ "$STATUS" -eq 0 ] && echo 0 || echo 1)" \
    "an honest producer passes (exit $STATUS)"
case "$OUT" in *GATE_PASS*) ;; *) check 1 "an honest run reports GATE_PASS" ;; esac
check "$([ -f "$BINDING" ] && echo 0 || echo 1)" \
    "an honest run writes an evidence binding"
"$PY" -c "import json,sys; json.load(open(sys.argv[1]))" "$BINDING" 2>/dev/null
check $? "the binding is valid JSON"
check "$([ "$(json_get 'd["marker_present"]')" = "True" ] && echo 0 || echo 1)" \
    "the binding records that the marker was found"
check "$([ "$(json_get 'd["exit_status"]')" = "0" ] && echo 0 || echo 1)" \
    "the binding records the producer's exit status"
check "$([ "$(json_get 'len(d["run_id"])')" -ge 32 ] && echo 0 || echo 1)" \
    "the binding carries a run id"
check "$([ "$(json_get 'len(d["evidence_sha256"])')" = "64" ] && echo 0 || echo 1)" \
    "the binding carries the digest of the log it wrote"
# A COUNT of assume-unchanged paths, which this used to require, names a blind
# spot without closing it: `git status` cannot see those paths, so a producer
# could rewrite one and the digest would not move. Require the digest itself.
check "$([ "$(json_get 'len(d["binding_pre"]["special_index_sha256"])')" = "64" ] && echo 0 || echo 1)" \
    "the binding carries a content digest of the special-index paths"
check "$([ "$(json_get '"special_index_files" in d["binding_pre"] and "special_index_bytes" in d["binding_pre"]')" = "True" ] && echo 0 || echo 1)" \
    "the binding discloses how many special-index paths it covered, and their size"
check "$([ "$(json_get 'd["binding_stable"]')" = "True" ] && echo 0 || echo 1)" \
    "an honest run reports a stable binding"

FIRST_RUN=$(json_get 'd["run_id"]')
run "$TMP/good.sh"
SECOND_RUN=$(json_get 'd["run_id"]')
check "$([ "$FIRST_RUN" != "$SECOND_RUN" ] && echo 0 || echo 1)" \
    "each run gets its own run id"

# --- 2. argv fidelity -------------------------------------------------------
# "$*" collapses these two into the same string; a JSON array does not.
run "$TMP/echo_args.sh" "a b"
ONE=$(json_get 'json.dumps(d["command"])')
run "$TMP/echo_args.sh" "a" "b"
TWO=$(json_get 'json.dumps(d["command"])')
check "$([ "$ONE" != "$TWO" ] && echo 0 || echo 1)" \
    "one argument 'a b' binds differently from two arguments 'a' 'b'"
check "$([ "$(json_get 'len(d["command"])')" = "$((PREFIX_N + 3))" ] && echo 0 || echo 1)" \
    "the binding records argv as a list, not a joined string"

# Quote, backslash and newline in one argument. The binding must stay valid
# JSON and round-trip the argument byte for byte.
WEIRD='he said "hi" \ then
a newline'
run "$TMP/echo_args.sh" "$WEIRD"
"$PY" -c "import json,sys; json.load(open(sys.argv[1]))" "$BINDING" 2>/dev/null
check $? "an argument with a quote, backslash and newline keeps the JSON valid"
# Compare the LAST argv element, not a fixed index: the producer prefix varies
# by platform, and an index that silently pointed at the wrong element would
# make this pass or fail for reasons unrelated to round-tripping. Both sides are
# JSON-encoded before comparing so an embedded newline cannot be mangled by
# command substitution stripping trailing whitespace.
GOT=$(json_get 'json.dumps(d["command"][-1])')
WANT=$("$PY" -c 'import json,sys; print(json.dumps(sys.argv[1]))' "$WEIRD")
check "$([ -n "$GOT" ] && [ "$GOT" = "$WANT" ] && echo 0 || echo 1)" \
    "such an argument round-trips exactly"

# --- 3. no raw secret env values -------------------------------------------
SENTINEL="SENTINEL-8c31d0-DO-NOT-LEAK"
OUT=$( (cd "$ROOT" && CNET_RESIDUAL_TOKEN="$SENTINEL" \
        "${RUNNER[@]}" synthetic_gate "$LOG" "$MARKER" \
        -- "${PRODUCER_PREFIX[@]}" "$TMP/good.sh") 2>&1 )
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

state_of() {  # $1 = repo, prints the two content digests
    "$PY" - "$1" <<'PY'
import hashlib, subprocess, sys
from pathlib import Path
root = Path(sys.argv[1])
def git(*a):
    r = subprocess.run(["git", *a], cwd=root, text=True,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    return r.stdout if r.returncode == 0 else ""
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() else "absent"
t = hashlib.sha256()
for line in git("status", "--porcelain=v1").splitlines():
    if len(line) < 4: continue
    e = line[3:].split(" -> ")[-1].strip().strip('"')
    t.update(line[:3].encode()); t.update(e.encode()); t.update(sha(root / e).encode())
u = hashlib.sha256()
for e in sorted(x for x in git("ls-files", "--others", "--exclude-standard", "-z").split("\0") if x):
    u.update(e.encode()); u.update(sha(root / e).encode())
print(t.hexdigest(), u.hexdigest())
PY
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

if [ "$failures" -gt 0 ]; then
    printf 'GATE_EVIDENCE_FAIL checks=%d failures=%d\n' "$checks" "$failures"
    exit 1
fi

printf 'GATE_EVIDENCE_PASS checks=%d stale=refused marker_then_exit=refused silent=refused duplicate=refused conflicting=refused substring=refused argv=exact secrets=redacted\n' \
    "$checks"
exit 0
