#!/bin/sh
# The managed warning gate must RESTORE before it builds, offline, or refuse.
#
# WHY THIS EXISTS. `make ci_core` passed in the writer's worktree and exited 2
# in a truly fresh detached worktree of the same commit, 7396275:
#
#   error NETSDK1004: Assets file '.../dotnet/Cce.Tests/obj/project.assets.json'
#   not found. Run a NuGet package restore to generate this file.
#   make: *** [Makefile:3714: managed_warning_gate] Error 1
#
# `managed_warning_gate` ran `dotnet build --no-restore` against projects a
# fresh checkout has never restored. The writer's tree passed only because it
# held ignored `obj/` state. That is the same defect class as F9 -- a gate whose
# prerequisites live outside the tree -- in a different gate.
#
# The fix must not buy hermeticity with network egress: release verification is
# deliberately offline. So each project is restored against an explicitly EMPTY
# local source, which resolves from the machine's existing package cache and
# CANNOT reach a feed; a package that is not already cached fails closed.
#
# This harness proves the property causally with a fake `dotnet` that records
# every invocation:
#
#   * restore precedes build, per project, for EVERY declared project;
#   * exactly one --source is supplied and the directory it names is EMPTY at
#     call time -- no feed URL is ever passed;
#   * a restore that FAILS stops the gate, and build never runs for it;
#   * a restore that "succeeds" without producing assets is refused -- exit 0 is
#     not evidence that a restore happened;
#   * a restore that hangs is bounded and refused;
#   * the committed project list is real, and is what gets restored.
#
# CNET_MANAGED_PREREQ_LEGACY=<rev> replays the recipe as it was at <rev> and
# requires the ordering property to FAIL, so the RED stays re-runnable instead
# of being a screenshot in a report.
#
# Every fixture is a mktemp tree. No project in this repository is built,
# restored or written by this file.
#
# Usage: sh tests/test_managed_warning_prereq.sh
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MAKEFILE=${CNET_MANAGED_PREREQ_MAKEFILE:-$ROOT/Makefile}
LEGACY=${CNET_MANAGED_PREREQ_LEGACY:-}

# CNET_MANAGED_PREREQ_LEGACY=<rev> installs the recipe as it was at <rev> and
# runs the same scenarios against it. The caller requires this to FAIL.
if [ -n "$LEGACY" ]; then
    MAKEFILE=$(mktemp "${TMPDIR:-/tmp}/cnet-managed-legacy-XXXXXX.mk") || exit 1
    if ! git -C "$ROOT" show "$LEGACY:Makefile" > "$MAKEFILE" 2>/dev/null; then
        echo "MANAGED_WARNING_PREREQ_FAIL reason=cannot_install_legacy rev=$LEGACY"
        rm -f "$MAKEFILE"
        exit 1
    fi
    echo "MANAGED_PREREQ_LEGACY rev=$LEGACY makefile=$MAKEFILE"
    trap 'rm -f "$MAKEFILE"' EXIT HUP INT TERM
fi

checks=0
failures=0

check() {
    checks=$((checks + 1))
    if [ "$1" != "0" ]; then
        failures=$((failures + 1))
        echo "FAIL: $2"
    fi
}

yes_no() { [ "$1" = "0" ] && echo 0 || echo 1; }

# ---- the fake dotnet ----------------------------------------------------
make_fake_dotnet() {
    cat > "$1" <<'FAKE'
#!/bin/sh
# Fake dotnet. Records every invocation, and records whether the --source
# directory it was handed was EMPTY at the moment of the call -- which is the
# whole isolation claim, and cannot be checked after the gate deletes it.
trace=$CNET_FAKE_TRACE
line=""
for a in "$@"; do line="$line|$a"; done
printf 'ARGV%s\n' "$line" >> "$trace"
verb=$1
project=${2:-}
if [ "$verb" = "restore" ]; then
    src=""
    prev=""
    for a in "$@"; do
        if [ "$prev" = "--source" ]; then src=$a; fi
        prev=$a
    done
    if [ -n "$src" ] && [ -d "$src" ]; then
        n=$(ls -A "$src" 2>/dev/null | wc -l)
        printf 'SOURCE %s entries=%s\n' "$src" "$n" >> "$trace"
    else
        printf 'SOURCE_MISSING %s\n' "$src" >> "$trace"
    fi
    if [ -n "${CNET_FAKE_RESTORE_HANG:-}" ]; then sleep "$CNET_FAKE_RESTORE_HANG"; fi
    if [ -n "${CNET_FAKE_RESTORE_FAIL:-}" ] && [ "$project" = "$CNET_FAKE_RESTORE_FAIL" ]; then
        echo "fake: restore refused for $project" >&2
        exit 7
    fi
    if [ -z "${CNET_FAKE_SKIP_ASSETS:-}" ]; then
        mkdir -p "${project%/*}/obj"
        printf '{}\n' > "${project%/*}/obj/project.assets.json"
    fi
    exit 0
fi
if [ "$verb" = "build" ]; then
    echo "fake: build $project"
    exit 0
fi
exit 0
FAKE
    chmod +x "$1"
}

# ---- one scenario -------------------------------------------------------
# run_gate <workdir> [extra make args...]
run_gate() {
    work=$1
    shift
    CNET_FAKE_TRACE="$work/trace" \
    CNET_FAKE_RESTORE_FAIL="${FAKE_RESTORE_FAIL:-}" \
    CNET_FAKE_SKIP_ASSETS="${FAKE_SKIP_ASSETS:-}" \
    CNET_FAKE_RESTORE_HANG="${FAKE_RESTORE_HANG:-}" \
    make -C "$ROOT" -f "$MAKEFILE" managed_warning_gate \
        DOTNET="$work/fake_dotnet" \
        MANAGED_WARNING_LOG="$work/gate.log" \
        MANAGED_RESTORE_LOG="$work/restore.log" \
        "$@" > "$work/make.log" 2>&1
    echo $? > "$work/rc"
}

new_work() {
    work=$(mktemp -d "${TMPDIR:-/tmp}/cnet-managed-prereq-XXXXXX") || exit 1
    make_fake_dotnet "$work/fake_dotnet"
    : > "$work/trace"
    mkdir -p "$work/proj/alpha" "$work/proj/beta" "$work/proj/gamma"
    : > "$work/proj/alpha/alpha.csproj"
    : > "$work/proj/beta/beta.csproj"
    : > "$work/proj/gamma/gamma.csproj"
    echo "$work"
}

# Index (1-based) of the first trace line matching a verb+project, or 0.
line_of() {
    awk -v want="$2" -v verb="$3" '
        index($0, "ARGV|" verb "|" want) == 1 { print NR; found=1; exit }
        END { if (!found) print 0 }
    ' "$1"
}

count_of() {
    # `grep -c` prints 0 AND exits 1 when nothing matches, so a `|| echo 0`
    # fallback would emit the count twice and every comparison against it would
    # be false -- a test that fails for a reason that has nothing to do with
    # what it is testing.
    n=$(grep -c "^ARGV|$2|" "$1" 2>/dev/null) || n=0
    echo "$n"
}

# ---- S1: the normal sequence -------------------------------------------
work=$(new_work)
P1="$work/proj/alpha/alpha.csproj"
P2="$work/proj/beta/beta.csproj"
P3="$work/proj/gamma/gamma.csproj"
FAKE_RESTORE_FAIL= FAKE_SKIP_ASSETS= FAKE_RESTORE_HANG= \
    run_gate "$work" MANAGED_WARNING_PROJECTS="$P1 $P2 $P3"
rc=$(cat "$work/rc")
check "$(yes_no "$rc")" "normal sequence: the gate passes (rc=$rc)
$(tail -5 "$work/make.log" 2>/dev/null)"
grep -q '^MANAGED_WARNING_GATE_PASS' "$work/make.log" 2>/dev/null
check $? "normal sequence: the gate reports MANAGED_WARNING_GATE_PASS"

restores=$(count_of "$work/trace" restore)
builds=$(count_of "$work/trace" build)
check "$([ "$restores" = "3" ] && echo 0 || echo 1)" \
    "every declared project is restored (saw $restores of 3)"
check "$([ "$builds" = "3" ] && echo 0 || echo 1)" \
    "every declared project is built (saw $builds of 3)"

prev_build=0
for p in "$P1" "$P2" "$P3"; do
    r=$(line_of "$work/trace" "$p" restore)
    b=$(line_of "$work/trace" "$p" build)
    check "$([ "$r" -gt 0 ] && [ "$b" -gt 0 ] && [ "$r" -lt "$b" ] && echo 0 || echo 1)" \
        "restore precedes build for $p (restore@$r build@$b)"
    check "$([ "$r" -gt "$prev_build" ] && echo 0 || echo 1)" \
        "$p is restored after the previous project finished (restore@$r prev_build@$prev_build)"
    prev_build=$b
done

# Isolation: exactly one --source, and the directory it names was EMPTY.
bad_source=0
sources=0
while IFS= read -r line; do
    case "$line" in
        ARGV\|restore\|*)
            n=$(printf '%s\n' "$line" | tr '|' '\n' | grep -c '^--source$')
            [ "$n" = "1" ] || bad_source=1
            sources=$((sources + 1))
            case "$line" in
                *nuget.org*|*http://*|*https://*) bad_source=1 ;;
            esac
            ;;
        SOURCE\ *)
            entries=${line##*entries=}
            [ "$entries" = "0" ] || bad_source=1
            ;;
        SOURCE_MISSING*) bad_source=1 ;;
    esac
done < "$work/trace"
check "$bad_source" \
    "every restore is given exactly one --source, it names an EMPTY directory, \
and no feed URL is ever passed"
check "$([ "$sources" = "3" ] && echo 0 || echo 1)" \
    "the isolation check saw all three restores (saw $sources)"

norestore=$(grep '^ARGV|build|' "$work/trace" | grep -c -- '|--no-restore|')
check "$([ "$norestore" = "3" ] && echo 0 || echo 1)" \
    "every build still runs --no-restore, so the restore above is the only one \
that can have happened (saw $norestore of 3)"
echo "MANAGED_PREREQ_NORMAL rc=$rc restores=$restores builds=$builds sources=$sources"
rm -rf "$work"

# ---- S2: a restore that FAILS stops the gate ---------------------------
work=$(new_work)
P1="$work/proj/alpha/alpha.csproj"
P2="$work/proj/beta/beta.csproj"
P3="$work/proj/gamma/gamma.csproj"
FAKE_RESTORE_FAIL="$P2" FAKE_SKIP_ASSETS= FAKE_RESTORE_HANG= \
    run_gate "$work" MANAGED_WARNING_PROJECTS="$P1 $P2 $P3"
rc=$(cat "$work/rc")
check "$([ "$rc" != "0" ] && echo 0 || echo 1)" \
    "a failed restore fails the gate (rc=$rc)"
grep -q "MANAGED_WARNING_GATE_FAIL reason=restore_failed project=$P2" \
    "$work/make.log" 2>/dev/null
check $? "a failed restore names the reason and the project"
check "$([ "$(line_of "$work/trace" "$P2" build)" = "0" ] && echo 0 || echo 1)" \
    "build never runs for a project whose restore failed"
check "$([ "$(line_of "$work/trace" "$P3" restore)" = "0" ] && echo 0 || echo 1)" \
    "the gate stops rather than carrying on to the next project"
echo "MANAGED_PREREQ_RESTORE_FAIL rc=$rc builds=$(count_of "$work/trace" build)"
rm -rf "$work"

# ---- S3: a restore that produced no assets is not a restore -------------
work=$(new_work)
P1="$work/proj/alpha/alpha.csproj"
P2="$work/proj/beta/beta.csproj"
FAKE_RESTORE_FAIL= FAKE_SKIP_ASSETS=1 FAKE_RESTORE_HANG= \
    run_gate "$work" MANAGED_WARNING_PROJECTS="$P1 $P2"
rc=$(cat "$work/rc")
check "$([ "$rc" != "0" ] && echo 0 || echo 1)" \
    "a restore that exits 0 without producing assets fails the gate (rc=$rc)"
grep -q "MANAGED_WARNING_GATE_FAIL reason=assets_absent project=$P1" \
    "$work/make.log" 2>/dev/null
check $? "the missing assets file is named"
check "$([ "$(count_of "$work/trace" build)" = "0" ] && echo 0 || echo 1)" \
    "no build runs when the assets file is absent"
echo "MANAGED_PREREQ_ASSETS_ABSENT rc=$rc builds=$(count_of "$work/trace" build)"
rm -rf "$work"

# ---- S4: a restore that hangs is bounded --------------------------------
work=$(new_work)
P1="$work/proj/alpha/alpha.csproj"
FAKE_RESTORE_FAIL= FAKE_SKIP_ASSETS= FAKE_RESTORE_HANG=10 \
    run_gate "$work" MANAGED_WARNING_PROJECTS="$P1" MANAGED_RESTORE_TIMEOUT=2
rc=$(cat "$work/rc")
check "$([ "$rc" != "0" ] && echo 0 || echo 1)" \
    "a restore that hangs fails the gate rather than waiting forever (rc=$rc)"
grep -q "MANAGED_WARNING_GATE_FAIL reason=restore_failed project=$P1 status=124" \
    "$work/make.log" 2>/dev/null
check $? "a timed-out restore is reported as such (status=124)"
check "$([ "$(count_of "$work/trace" build)" = "0" ] && echo 0 || echo 1)" \
    "no build runs after a restore timeout"
echo "MANAGED_PREREQ_TIMEOUT rc=$rc"
rm -rf "$work"

# ---- S5: the committed list is real ------------------------------------
declared=$(make -C "$ROOT" -f "$MAKEFILE" --no-print-directory \
    print-MANAGED_WARNING_PROJECTS 2>/dev/null || true)
count=0
missing=0
for p in $declared; do
    count=$((count + 1))
    [ -f "$ROOT/$p" ] || { missing=$((missing + 1)); echo "FAIL: declared project is absent: $p"; }
done
check "$([ "$count" -gt 0 ] && echo 0 || echo 1)" \
    "the Makefile declares a nonempty MANAGED_WARNING_PROJECTS list (saw $count)"
check "$([ "$missing" = "0" ] && echo 0 || echo 1)" \
    "every declared project file exists ($missing missing)"
echo "MANAGED_PREREQ_DECLARED projects=$count missing=$missing"

if [ "$failures" != "0" ]; then
    echo "MANAGED_WARNING_PREREQ_FAIL checks=$checks failures=$failures"
    exit 1
fi
echo "MANAGED_WARNING_PREREQ_PASS checks=$checks projects=$count \
order=restore_before_build source=empty_local_only refusals=failed,absent,timeout"
exit 0
