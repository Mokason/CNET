#!/bin/sh
# orphan_tools.sh -- fail when a file in tools/ has no rule to build it.
#
# Same failure mode as tests/ (see orphan_tests.sh): in an 8,500-line Makefile,
# a source file with no target is invisible. Six tools were in that state, and
# among them were two superseded compete-fixture generations that nothing
# marked as superseded -- so "which fixture is current?" had no answer in the
# tree, only in whoever last ran a campaign. Those two now live in attic/.
#
# Run: sh tests/orphan_tools.sh   (or `make orphan_tools`)
set -u

# Deliberately not targets. Each entry needs a reason.
#   (none at present)
allow=" "

orphans=""
for f in tools/*.c; do
    [ -e "$f" ] || continue
    b=$(basename "$f" .c)
    case "$allow" in
        *" $b "*) continue ;;
    esac
    grep -qsE "\b$b\b" Makefile mk/*.mk || orphans="$orphans $b"
done

if [ -n "$orphans" ]; then
    printf 'ORPHAN_TOOLS: FAIL - these tools/*.c files have no Makefile rule:\n'
    for o in $orphans; do printf '  %s\n' "$o"; done
    printf 'Add a target, move it to attic/, or allowlist it here with a reason.\n'
    exit 1
fi
printf 'ORPHAN_TOOLS_PASS\n'
exit 0
