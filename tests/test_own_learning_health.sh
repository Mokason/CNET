#!/bin/bash
# test_own_learning_health.sh -- the watchdog must fail the things it names.
#
# WHY THIS EXISTS. The 2026-07-30 re-analysis reproduced this:
#
#   CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1 CNET_COVERAGE_ABSTAIN=1 \
#   CNET_RESIDUAL_HTTP=http://127.0.0.1:1 \
#   ./bin/cnet_own_learning_health --base /definitely/not/a/cnet/base.cnb
#   -> "base_loaded":0 ... "status":"ok" ... OWN_LEARNING_HEALTH_PASS, exit 0
#
# A deployment-health gate certified an uninspectable deployment as healthy,
# with a teacher URL nothing was listening on. It also accepted arbitrary
# strings as booleans: CNET_COVERAGE_ABSTAIN=off reported the gate as "on".
#
# Every fixture here lives under a mkdtemp root. No real base, no real coverage
# state, no real config, no live service, and the only network address used is
# 127.0.0.1:1, which is guaranteed to refuse rather than reach anything.
#
# Usage: bash tests/test_own_learning_health.sh [health-binary] [mk_test_base]
# Exit 0 = strict mode is strict, 1 = a dangerous configuration passed.

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
HEALTH=${1:-$ROOT/bin/cnet_own_learning_health}
MKBASE=${2:-$ROOT/bin/mk_test_base}

if [ ! -x "$HEALTH" ]; then
    printf 'FAIL: health binary not found at %s\n' "$HEALTH"
    exit 1
fi

TMP=$(mktemp -d "${TMPDIR:-/tmp}/cnet-health-XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT INT TERM

checks=0
failures=0

# A dead port. Nothing listens on TCP 1 and the kernel refuses immediately, so
# this probes "configured teacher is not there" without touching a network.
DEAD_TEACHER="http://127.0.0.1:1"

note() { printf '  %s\n' "$1"; }

# run_health <expect-exit> <expect-marker> <forbid-marker> <description> -- <argv...>
# Environment for the run is taken from the caller's exported HEALTH_ENV_* set
# via `env` arguments placed before `--`.
run_health() {
    want_exit=$1; shift
    want_marker=$1; shift
    forbid_marker=$1; shift
    description=$1; shift
    [ "$1" = "--" ] && shift

    out=$("$@" 2>&1)
    status=$?

    checks=$((checks + 1))
    if [ "$status" -ne "$want_exit" ]; then
        printf 'FAIL: %s -- expected exit %s, got %s\n' "$description" "$want_exit" "$status"
        note "$out"
        failures=$((failures + 1))
    fi

    if [ -n "$want_marker" ]; then
        checks=$((checks + 1))
        case "$out" in
            *"$want_marker"*) ;;
            *)
                printf 'FAIL: %s -- expected marker %s\n' "$description" "$want_marker"
                note "$out"
                failures=$((failures + 1))
                ;;
        esac
    fi

    if [ -n "$forbid_marker" ]; then
        checks=$((checks + 1))
        case "$out" in
            *"$forbid_marker"*)
                printf 'FAIL: %s -- forbidden marker %s was printed\n' \
                    "$description" "$forbid_marker"
                note "$out"
                failures=$((failures + 1))
                ;;
        esac
    fi
    LAST_OUT=$out
}

# --- 1. the exact reproduction from the re-analysis -----------------------
run_health 1 "OWN_LEARNING_HEALTH_FAIL" "OWN_LEARNING_HEALTH_PASS" \
    "nonexistent base must not certify as healthy" -- \
    env CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1 CNET_COVERAGE_ABSTAIN=1 \
        CNET_RESIDUAL_HTTP="$DEAD_TEACHER" \
        "$HEALTH" --base /definitely/not/a/cnet/base.cnb
checks=$((checks + 1))
case "$LAST_OUT" in
    *base_not_loaded*) ;;
    *) printf 'FAIL: missing base must be reported as base_not_loaded\n'
       failures=$((failures + 1)) ;;
esac
checks=$((checks + 1))
case "$LAST_OUT" in
    *residual_http_unreachable*) ;;
    *) printf 'FAIL: a dead configured teacher must be reported unreachable\n'
       failures=$((failures + 1)) ;;
esac

# --- 2. a base that exists but is not a base -----------------------------
printf 'this is not a CNB container\n' > "$TMP/garbage.cnb"
run_health 1 "OWN_LEARNING_HEALTH_FAIL" "OWN_LEARNING_HEALTH_PASS" \
    "unloadable base must not certify as healthy" -- \
    env CNET_RESIDUAL_HTTP= CNET_RESIDUAL_GGUF= \
        "$HEALTH" --base "$TMP/garbage.cnb"

# --- 3. no base named at all ---------------------------------------------
run_health 1 "OWN_LEARNING_HEALTH_FAIL" "OWN_LEARNING_HEALTH_PASS" \
    "an unnamed base must not certify as healthy" -- \
    env CNET_BASE_PATH= CNET_RESIDUAL_HTTP= CNET_RESIDUAL_GGUF= "$HEALTH"

# --- 4. invalid booleans --------------------------------------------------
run_health 1 "invalid_boolean_coverage_abstain" "OWN_LEARNING_HEALTH_PASS" \
    "CNET_COVERAGE_ABSTAIN=off must be refused, not read as on" -- \
    env CNET_COVERAGE_ABSTAIN=off CNET_RESIDUAL_HTTP= CNET_RESIDUAL_GGUF= \
        "$HEALTH" --base /definitely/not/a/cnet/base.cnb

run_health 1 "invalid_boolean_mine_on_serve" "OWN_LEARNING_HEALTH_PASS" \
    "CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=yes must be refused" -- \
    env CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=yes CNET_RESIDUAL_HTTP= \
        CNET_RESIDUAL_GGUF= "$HEALTH" --base /definitely/not/a/cnet/base.cnb

# --- 5. config-only mode is not deployment health ------------------------
run_health 0 "CONFIG_ONLY_PASS" "OWN_LEARNING_HEALTH_PASS" \
    "config-only must pass on a sane profile without a deployment marker" -- \
    env CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1 CNET_COVERAGE_ABSTAIN=1 \
        CNET_RESIDUAL_HTTP="$DEAD_TEACHER" "$HEALTH" --config-only

run_health 1 "CONFIG_ONLY_FAIL" "CONFIG_ONLY_PASS" \
    "config-only must still catch the mine-on/gate-off typo" -- \
    env CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1 CNET_COVERAGE_ABSTAIN=0 \
        CNET_RESIDUAL_HTTP="$DEAD_TEACHER" "$HEALTH" --config-only

# --- 6. a real base, with and without required coverage ------------------
if [ -x "$MKBASE" ]; then
    if "$MKBASE" "$TMP/deployed.cnb" --mined > "$TMP/mkbase.log" 2>&1; then
        # Gate armed, sidecar absent: an unenforced guard, not "nothing to do".
        run_health 1 "coverage_file_missing" "OWN_LEARNING_HEALTH_PASS" \
            "armed coverage gate with no sidecar must fail" -- \
            env CNET_COVERAGE_ABSTAIN=1 CNET_RESIDUAL_HTTP= CNET_RESIDUAL_GGUF= \
                "$HEALTH" --base "$TMP/deployed.cnb"

        # The mined unit has no coverage record either: also reported.
        checks=$((checks + 1))
        case "$LAST_OUT" in
            *mined_units_without_coverage*) ;;
            *) printf 'FAIL: a mined unit with no record must be reported\n'
               failures=$((failures + 1)) ;;
        esac

        # Now give it a valid sidecar and a reachable-file teacher: healthy.
        if "$MKBASE" "$TMP/ok.cnb" --mined --coverage "$TMP/ok.cnb.coverage" \
                > "$TMP/mkbase2.log" 2>&1; then
            printf 'stand-in teacher weights\n' > "$TMP/teacher.gguf"
            run_health 0 "OWN_LEARNING_HEALTH_PASS" "OWN_LEARNING_HEALTH_FAIL" \
                "a fully inspected healthy deployment still passes" -- \
                env CNET_COVERAGE_ABSTAIN=1 CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=0 \
                    CNET_RESIDUAL_HTTP= CNET_RESIDUAL_GGUF="$TMP/teacher.gguf" \
                    "$HEALTH" --base "$TMP/ok.cnb"

            # A truncated sidecar must not read as coverage.
            head -c 12 "$TMP/ok.cnb.coverage" > "$TMP/trunc.coverage"
            cp "$TMP/ok.cnb" "$TMP/trunc.cnb"
            cp "$TMP/trunc.coverage" "$TMP/trunc.cnb.coverage"
            run_health 1 "OWN_LEARNING_HEALTH_FAIL" "OWN_LEARNING_HEALTH_PASS" \
                "a truncated coverage sidecar must not certify as healthy" -- \
                env CNET_COVERAGE_ABSTAIN=1 CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=0 \
                    CNET_RESIDUAL_HTTP= CNET_RESIDUAL_GGUF="$TMP/teacher.gguf" \
                    "$HEALTH" --base "$TMP/trunc.cnb"
        else
            printf 'FAIL: mk_test_base could not build the covered fixture\n'
            cat "$TMP/mkbase2.log"
            failures=$((failures + 1))
            checks=$((checks + 1))
        fi
    else
        printf 'FAIL: mk_test_base could not build a loadable base\n'
        cat "$TMP/mkbase.log"
        failures=$((failures + 1))
        checks=$((checks + 1))
    fi
else
    printf 'FAIL: mk_test_base not found at %s; loadable-base cases cannot run\n' \
        "$MKBASE"
    failures=$((failures + 1))
    checks=$((checks + 1))
fi

if [ "$failures" -gt 0 ]; then
    printf 'OWN_LEARNING_HEALTH_STRICT_FAIL checks=%d failures=%d\n' \
        "$checks" "$failures"
    exit 1
fi

printf 'OWN_LEARNING_HEALTH_STRICT_PASS checks=%d\n' "$checks"
exit 0
