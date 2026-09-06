#!/bin/bash
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/src/cce" "$work/include"
printf '#include "value.h"\nint value(void) { return VALUE; }\n' > "$work/src/cce/probe.c"
printf '#define VALUE 1\n' > "$work/include/value.h"
run() {
    make -s -C "$work" -f "$root/mk/cce_lib.mk" CC=gcc CCE=src/cce/probe.c \
        BIN_DIR=bin CFLAGS="-Iinclude ${1:-}" libcce
}
run
cp "$work/bin/libcce.a" "$work/before.a"
sleep 1
printf '#define VALUE 2\n' > "$work/include/value.h"
run
if cmp -s "$work/before.a" "$work/bin/libcce.a"; then
    echo CCE_BUILD_DEPENDENCIES_RED_header; exit 1
fi
run -O2
cp "$work/bin/libcce.a" "$work/optimized.a"
run -O0
if cmp -s "$work/optimized.a" "$work/bin/libcce.a"; then
    echo CCE_BUILD_DEPENDENCIES_RED_flags; exit 1
fi
before=$(stat -c %Y "$work/build/cce/probe.o")
sleep 1
run -O0
[ "$before" = "$(stat -c %Y "$work/build/cce/probe.o")" ] || {
    echo CCE_BUILD_DEPENDENCIES_RED_noop; exit 1;
}
echo CCE_BUILD_DEPENDENCIES_PASS
