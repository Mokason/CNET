#!/bin/sh
# layering_guard.sh -- stop the certified core from growing new dependencies
# on the layers above it.
#
# WHY THIS EXISTS
# ---------------
# docs/ARCHITECTURE.md says the core stays domain-agnostic. Measured from
# #include, that held for exactly one file: nn.c. Fifteen bidirectional
# subsystem pairs exist, and contract/ and router/ both point up into the agent
# layer as well as down.
#
# Untangling fifteen cycles is not a refactor worth starting mid-flight, and
# this script does NOT try to. It freezes the situation: every existing
# back-edge is listed in tests/layering_baseline.txt, and the build fails if a
# NEW one appears. The cycle count stops at fifteen instead of growing, and
# nn.c stays the leaf the thesis rests on.
#
# Removing a back-edge is always allowed -- the script reports it and tells you
# to re-freeze the baseline.
#
# Run: sh tests/layering_guard.sh          (check)
#      sh tests/layering_guard.sh --freeze (rewrite the baseline)
set -u

BASELINE=tests/layering_baseline.txt

# CORE = the layer the thesis rests on: frozen primitives, typed contracts,
# the planner. A header is "core-owned" if a core .c or .h of the same basename
# exists under these roots.
core_files() {
    echo src/nn.c
    echo include/nn.h
    echo include/router.h
    ls src/contract/*.c include/contract/*.h 2>/dev/null
    ls src/router/*.c include/router/*.h 2>/dev/null
}

core_owned() {
    b=$1
    for d in include/contract include/router src/contract src/router; do
        [ -e "$d/$b" ] && return 0
    done
    case "$b" in
        nn.h | router.h | cnet_export.h) return 0 ;;
    esac
    # a .h whose implementation lives in a core directory
    stem=${b%.h}
    for d in src/contract src/router; do
        [ -e "$d/$stem.c" ] && return 0
    done
    return 1
}

current() {
    for f in $(core_files); do
        [ -e "$f" ] || continue
        grep -ohE '#include "[^"]+\.h"' "$f" 2>/dev/null |
            sed 's/#include "//; s/"$//' |
            while read -r h; do
                b=$(basename "$h")
                core_owned "$b" || printf '%s -> %s\n' "$(basename "$f")" "$b"
            done
    done | sort -u
}

if [ "${1:-}" = "--freeze" ]; then
    current > "$BASELINE"
    printf 'LAYERING_GUARD: baseline re-frozen with %d back-edges.\n' \
        "$(wc -l < "$BASELINE")"
    exit 0
fi

if [ ! -f "$BASELINE" ]; then
    printf 'LAYERING_GUARD: FAIL - %s is missing. Run with --freeze.\n' "$BASELINE"
    exit 1
fi

current > /tmp/layering_current.$$ 2>/dev/null
added=$(comm -13 "$BASELINE" /tmp/layering_current.$$)
removed=$(comm -23 "$BASELINE" /tmp/layering_current.$$)
rm -f /tmp/layering_current.$$

if [ -n "$added" ]; then
    printf 'LAYERING_GUARD: FAIL - new dependency from the core into a layer above it:\n'
    printf '%s\n' "$added" | sed 's/^/  /'
    printf 'Either invert the dependency, or add it to %s deliberately.\n' "$BASELINE"
    exit 1
fi

if [ -n "$removed" ]; then
    printf 'LAYERING_GUARD: %d back-edge(s) REMOVED since the baseline:\n' \
        "$(printf '%s\n' "$removed" | grep -c .)"
    printf '%s\n' "$removed" | sed 's/^/  /'
    printf 'Progress. Re-freeze with: sh tests/layering_guard.sh --freeze\n'
fi

printf 'LAYERING_GUARD_PASS\n'
exit 0
