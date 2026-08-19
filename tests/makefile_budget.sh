#!/bin/sh
# makefile_budget.sh -- the root Makefile may shrink, never grow.
#
# WHY THIS EXISTS
# ---------------
# The root Makefile reached 8,530 lines / 989 targets / 527 KB. At that size a
# rule that does not exist is invisible: tests/clgemm_unit.c was cited five
# times across README.md and docs/ARCHITECTURE.md as the executable proof of
# GPU bit-identity and had no target at all, and twenty-six test files plus six
# tools were in the same state.
#
# Splitting the whole file is staged work (see mk/README.md) because the
# original has no section boundaries a script can cut on. This gate makes the
# staging safe: the ceiling only moves down. Adding rules to the root file
# instead of a mk/ fragment has to be a deliberate act that edits the number in
# mk/makefile_budget.txt, in the same commit, with a reason in the message.
#
# Run: sh tests/makefile_budget.sh          (check)
#      sh tests/makefile_budget.sh --lower  (record the current, smaller, size)
set -u

BUDGET_FILE=mk/makefile_budget.txt
actual=$(wc -l < Makefile | tr -d ' ')

if [ "${1:-}" = "--lower" ]; then
    if [ -f "$BUDGET_FILE" ]; then
        current=$(head -1 "$BUDGET_FILE" | tr -d ' ')
        if [ "$actual" -gt "$current" ]; then
            printf 'MAKEFILE_BUDGET: refusing to raise the ceiling (%s > %s).\n' \
                "$actual" "$current"
            printf 'Move rules into mk/ instead, or edit %s by hand with a reason.\n' \
                "$BUDGET_FILE"
            exit 1
        fi
    fi
    printf '%s\n' "$actual" > "$BUDGET_FILE"
    printf 'MAKEFILE_BUDGET: ceiling lowered to %s lines.\n' "$actual"
    exit 0
fi

if [ ! -f "$BUDGET_FILE" ]; then
    printf 'MAKEFILE_BUDGET: FAIL - %s missing. Run with --lower.\n' "$BUDGET_FILE"
    exit 1
fi

budget=$(head -1 "$BUDGET_FILE" | tr -d ' ')
if [ "$actual" -gt "$budget" ]; then
    printf 'MAKEFILE_BUDGET: FAIL - Makefile is %s lines, ceiling is %s.\n' \
        "$actual" "$budget"
    printf 'Put the new rules in a mk/*.mk fragment (see mk/README.md), or raise\n'
    printf 'the number in %s deliberately and say why.\n' "$BUDGET_FILE"
    exit 1
fi

if [ "$actual" -lt "$budget" ]; then
    printf 'MAKEFILE_BUDGET: %s lines, %s under the ceiling. Lower it with:\n' \
        "$actual" "$((budget - actual))"
    printf '  sh tests/makefile_budget.sh --lower\n'
fi
printf 'MAKEFILE_BUDGET_PASS\n'
exit 0
