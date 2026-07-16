#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stage="/tmp/cnet-install-$$"
dist_a="/tmp/cnet-dist-a-$$"
dist_b="/tmp/cnet-dist-b-$$"
extract="/tmp/cnet-extract-$$"
consumer_c="/tmp/cnet-consumer-$$.c"
consumer_bin="/tmp/cnet-consumer-$$"
trap 'rm -rf "$stage" "$dist_a" "$dist_b" "$extract" "$consumer_c" "$consumer_bin"' EXIT INT TERM

cd "$root"
test -f VERSION || { echo 'RELEASE_PACKAGE_FAIL: VERSION missing' >&2; exit 1; }
version=$(tr -d '[:space:]' < VERSION)
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || {
    echo 'RELEASE_PACKAGE_FAIL: VERSION is not semantic' >&2; exit 1;
}

config=$(make --no-print-directory PORTABLE=1 print-config)
printf '%s\n' "$config" | grep -q 'PORTABLE=1'
if printf '%s\n' "$config" | grep -q -- '-march=native'; then
    echo 'RELEASE_PACKAGE_FAIL: portable build retains -march=native' >&2
    exit 1
fi

# The published source object is reproducible, not just present.
mkdir -p "$dist_a" "$dist_b" "$extract"
make --no-print-directory DIST_DIR="$dist_a" dist
make --no-print-directory DIST_DIR="$dist_b" dist
archive_a="$dist_a/cnet-$version.tar.gz"
archive_b="$dist_b/cnet-$version.tar.gz"
test -f "$archive_a" && test -f "$archive_b"
sha_a=$(sha256sum "$archive_a" | cut -d' ' -f1)
sha_b=$(sha256sum "$archive_b" | cut -d' ' -f1)
test "$sha_a" = "$sha_b" || {
    echo 'RELEASE_PACKAGE_FAIL: source archive is not reproducible' >&2
    exit 1
}
tar -tzf "$archive_a" | grep -q "^cnet-$version/VERSION$"
tar -tzf "$archive_a" | grep -q "^cnet-$version/.github/workflows/ci.yml$"
if tar -tzf "$archive_a" | grep -Eq '\.(so|dll)$'; then
    echo 'RELEASE_PACKAGE_FAIL: source archive contains a compiled library' >&2
    exit 1
fi

# Prove the archive, not the checkout: clean extraction -> native build ->
# staged install -> pkg-config compile/link -> execution.
tar -xzf "$archive_a" -C "$extract"
archive_root="$extract/cnet-$version"
test -f "$archive_root/Makefile"
make --no-print-directory -C "$archive_root" PORTABLE=1 cnet_dll
make --no-print-directory -C "$archive_root" PORTABLE=1 \
    DESTDIR="$stage" PREFIX=/usr install

libdir="$stage/usr/lib"
incdir="$stage/usr/include/cnet"
pcdir="$libdir/pkgconfig"
test -f "$libdir/libcnet.so.$version"
test -L "$libdir/libcnet.so"
test -f "$incdir/cnet_version.h"
test -f "$incdir/cce/cce_tensor.h"
test -f "$pcdir/cnet.pc"

pc_version=$(PKG_CONFIG_PATH="$pcdir" PKG_CONFIG_SYSROOT_DIR="$stage" pkg-config --modversion cnet)
test "$pc_version" = "$version"
cat > "$consumer_c" <<'EOF'
#include <cnet_version.h>
#include <cce/cce_tensor.h>
#include <stdio.h>
int main(void) {
    cce_tensor t;
    int shape[1] = {4};
    if (cce_tensor_alloc(&t, shape, 1) != CCE_OK) return 1;
    cce_tensor_zero(&t);
    cce_tensor_free(&t);
    puts(CNET_VERSION_STRING);
    return 0;
}
EOF
PKG_CONFIG_PATH="$pcdir" PKG_CONFIG_SYSROOT_DIR="$stage" \
    ${CC:-cc} "$consumer_c" -o "$consumer_bin" \
    $(PKG_CONFIG_PATH="$pcdir" PKG_CONFIG_SYSROOT_DIR="$stage" \
        pkg-config --cflags --libs cnet)
test "$(LD_LIBRARY_PATH="$libdir" "$consumer_bin")" = "$version"

make --no-print-directory -C "$archive_root" \
    DESTDIR="$stage" PREFIX=/usr uninstall
test ! -e "$libdir/libcnet.so"
test ! -e "$incdir"
printf 'RELEASE_PACKAGE_PASS version=%s sha256=%s archive_self_build=1\n' \
    "$version" "$sha_a"
