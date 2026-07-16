#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
lib="$root/cnet.so"
test -f "$lib" || {
    echo "ADMISSION_ABI_AUDIT_FAIL: cnet.so missing" >&2
    exit 1
}

fail=0
for internal in registry_init registry_add registry_add_certified; do
    if nm -D --defined-only "$lib" | grep -Eq "[[:space:]]${internal}$"; then
        echo "ADMISSION_ABI_AUDIT_FAIL: internal symbol exported: $internal" >&2
        fail=1
    fi
done
for public in registry_init_production specialist_admit; do
    if ! nm -D --defined-only "$lib" | grep -Eq "[[:space:]]${public}$"; then
        echo "ADMISSION_ABI_AUDIT_FAIL: authority symbol missing: $public" >&2
        fail=1
    fi
done

[ "$fail" -eq 0 ] || exit 1
echo "ADMISSION_ABI_AUDIT_PASS"
