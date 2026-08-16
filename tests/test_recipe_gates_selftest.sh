#!/bin/sh
# test_recipe_gates_selftest.sh -- a red-test for the recipe gate itself.
#
# WHY THIS EXISTS
# ---------------
# tests/test_recipe_gates.sh shipped for months with a dead branch:
#
#     case "$line" in
#         *'$('*)  ;;                              # matched FIRST
#         *'./'*|*'$(BIN_DIR)'*) has_swallow=1 ;;  # unreachable
#     esac
#
# POSIX `case` takes the first matching arm, and `$(BIN_DIR)` contains `$(`,
# so every `$(BIN_DIR)/x || true` recipe was skipped. The gate printed
# RECIPE_GATE_PASS while two real swallowed exits sat in the Makefile. A gate
# that cannot fail is worse than no gate, because it launders "unaudited" into
# a printed claim of "audited".
#
# A gate is only trustworthy if it has been SEEN to reject. This script builds
# synthetic Makefiles and requires the gate to fail on the bad ones *for the
# right reason* and pass on the good ones (so an over-broad "reject
# everything" fix cannot pass either).
#
# Usage: sh tests/test_recipe_gates_selftest.sh
# Exit 0 = the gate discriminates correctly. Exit 1 = the gate is broken.

set -u

GATE="${GATE:-tests/test_recipe_gates.sh}"

if [ ! -f "$GATE" ]; then
    printf 'SELFTEST: FAIL - gate not found at %s\n' "$GATE"
    exit 1
fi

WORK=$(mktemp -d 2>/dev/null) || WORK="./.recipe_gate_selftest.$$"
mkdir -p "$WORK" 2>/dev/null || true
# shellcheck disable=SC2064
trap "rm -rf '$WORK'" EXIT INT TERM

TAB=$(printf '\t')

# Build a fixture Makefile that satisfies every OTHER check the gate makes
# (bash SHELL, pipefail, all headline gates present and .PHONY) so that when it
# fails, it fails on the swallowed-exit property and nothing else. This is the
# same discipline managed_warning_prereq uses: a fixture that fails for an
# unrelated reason proves nothing.
# Read from the same file the gate itself reads, never a private copy.
GATES_FILE="$(dirname "$0")/headline_gates.txt"
if [ ! -f "$GATES_FILE" ]; then
    printf 'SELFTEST: FAIL - %s is missing\n' "$GATES_FILE"
    exit 1
fi
HEADLINE=$(sed 's/#.*//' "$GATES_FILE" | tr -s '[:space:]' ' ')

make_fixture() {
    # make_fixture <path> <recipe-line>
    _path="$1"
    _recipe="$2"
    {
        echo 'SHELL := /bin/bash'
        echo '.SHELLFLAGS := -o pipefail -c'
        echo 'BIN_DIR := bin'
        printf '.PHONY:'
        for _g in $HEADLINE; do printf ' %s' "$_g"; done
        printf '\n\n'
        for _g in $HEADLINE; do
            printf '%s:\n%s@echo %s\n\n' "$_g" "$TAB" "$_g"
        done
        printf 'subject:\n%s%s\n' "$TAB" "$_recipe"
    } > "$_path"
}

fails=0

expect() {
    # expect <name> <want-exit 0|1> <must-contain|-> <recipe-line>
    _name="$1"; _want="$2"; _needle="$3"; _recipe="$4"
    _fx="$WORK/Makefile.$_name"
    make_fixture "$_fx" "$_recipe"

    _out=$(sh "$GATE" "$_fx" 2>&1)
    _rc=$?

    if [ "$_rc" -ne "$_want" ]; then
        printf 'SELFTEST FAIL [%s]: wanted exit %s, got %s\n' "$_name" "$_want" "$_rc"
        printf '  recipe: %s\n' "$_recipe"
        printf '  output: %s\n' "$_out"
        fails=$((fails + 1))
        return
    fi

    if [ "$_needle" != "-" ]; then
        case "$_out" in
            *"$_needle"*) ;;
            *)
                printf 'SELFTEST FAIL [%s]: exit %s was right but output lacked %s\n' \
                    "$_name" "$_rc" "$_needle"
                printf '  output: %s\n' "$_out"
                fails=$((fails + 1))
                return
                ;;
        esac
    fi

    printf '  ok  %s\n' "$_name"
}

printf 'recipe_gate selftest: the gate must reject bad recipes and accept good ones\n'

# --- MUST REJECT ----------------------------------------------------------
# THE REGRESSION. `$(BIN_DIR)` contains `$(`; this is the exact line shape the
# dead `*'$('*)` arm used to skip. If this case ever passes, the gate is blind
# again.
expect 'bindir_or_true' 1 'SWALLOWED' \
    '@./$(BIN_DIR)/failing_test > logs/x.log 2>&1 || true'

# Same shape without the leading `./` -- also a $( line.
expect 'bindir_bare_or_true' 1 'SWALLOWED' \
    '@$(BIN_DIR)/failing_test > logs/x.log 2>&1 || true'

# Plain path with no `$(` at all. This one was caught even before the fix, so
# it is the control that proves the harness itself works.
expect 'plain_or_true' 1 'SWALLOWED' \
    '@./bin/failing_test > logs/x.log 2>&1 || true'

expect 'bindir_or_echo' 1 'SWALLOWED' \
    '@./$(BIN_DIR)/failing_test || echo "bench unavailable"'

expect 'bindir_or_colon' 1 'SWALLOWED' \
    '@./$(BIN_DIR)/failing_test > logs/x.log 2>&1 || :'

expect 'compile_and_run' 1 'SWALLOWED' \
    '@$(CC) -o $(BIN_DIR)/t t.c && ./$(BIN_DIR)/t || echo skipped'

# --- MUST ACCEPT ----------------------------------------------------------
# These guard against the opposite failure: a fix that rejects everything is
# just as useless as one that rejects nothing.
expect 'clean_run' 0 '-' \
    '@./$(BIN_DIR)/good_test > logs/x.log 2>&1'

expect 'rc_propagating_run' 0 '-' \
    '@rc=0; ./$(BIN_DIR)/t > logs/x.log 2>&1 || rc=$$?; cat logs/x.log; [ $$rc -eq 0 ] || exit $$rc'

expect 'deliberate_grep' 0 '-' \
    '@grep -q MARKER logs/x.log || true'

expect 'make_recursion' 0 '-' \
    '@$(MAKE) -C subdir something || true'

expect 'git_metadata' 0 '-' \
    '@git rev-parse HEAD || echo unknown'

if [ "$fails" -gt 0 ]; then
    printf '\nRECIPE_GATE_SELFTEST: FAIL - %d case(s) wrong; the gate does not discriminate.\n' "$fails"
    exit 1
fi

printf 'RECIPE_GATE_SELFTEST_PASS: gate rejects 6 swallow shapes and accepts 5 legitimate ones.\n'
exit 0
