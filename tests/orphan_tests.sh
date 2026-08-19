#!/bin/sh
# orphan_tests.sh -- fail when a file in tests/ has no rule to build it.
#
# WHY THIS EXISTS
# ---------------
# docs/ARCHITECTURE.md cited `clgemm_unit` four times, and README.md told the
# reader to run it, as the executable proof that the multi-GPU OpenCL forward is
# bit-identical ("360 calls", "proven, not asserted"). tests/clgemm_unit.c
# existed. No rule built it, and there is no generic tests/%.c pattern rule, so
# `make clgemm_unit` answered "No rule to make target" -- the project's
# strongest correctness claim had no runnable gate. Twenty-six test files were
# in that state, including cce_archive_test and cce_forest_test, the two storage
# layers named in the architecture table.
#
# In an 8,530-line Makefile a test with no target is invisible. This makes it
# loud instead.
#
# Run: sh tests/orphan_tests.sh   (or `make orphan_tests`)
set -u

# Files that are deliberately not targets. Each needs a reason, not just a name.
#   stubs_q5_test      - link-time stub provider, no main()
#   standalone_main    - the -DCNET_TEST_ENTRY shim used by every test_<suite>
#   tiny_model_fixture - header-only fixture included by other suites
allow=" stubs_q5_test standalone_main tiny_model_fixture "

orphans=""
for f in tests/*.c; do
    [ -e "$f" ] || continue
    b=$(basename "$f" .c)
    case "$allow" in
        *" $b "*) continue ;;
    esac
    grep -qs "$b" Makefile mk/*.mk || orphans="$orphans $b"
done

if [ -n "$orphans" ]; then
    printf 'ORPHAN_TESTS: FAIL - these tests/*.c files have no Makefile rule:\n'
    for o in $orphans; do printf '  %s\n' "$o"; done
    printf 'Add a target, or add the name to the allowlist in this script with a reason.\n'
    exit 1
fi
printf 'ORPHAN_TESTS_PASS\n'
exit 0
