#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
stage="/tmp/cnet-install-$$"
dist="/tmp/cnet-dist-$$"
consumer_c="/tmp/cnet-consumer-$$.c"
consumer_bin="/tmp/cnet-consumer-$$"
trap 'rm -rf "$stage" "$dist" "$consumer_c" "$consumer_bin"' EXIT INT TERM

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

make --no-print-directory PORTABLE=1 DESTDIR="$stage" PREFIX=/usr install
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

mkdir -p "$dist"
make --no-print-directory DIST_DIR="$dist" dist
test -f "$dist/cnet-$version.tar.gz"
tar -tzf "$dist/cnet-$version.tar.gz" | grep -q "^cnet-$version/VERSION$"
tar -tzf "$dist/cnet-$version.tar.gz" | grep -q "^cnet-$version/.github/workflows/ci.yml$"

make --no-print-directory DESTDIR="$stage" PREFIX=/usr uninstall
test ! -e "$libdir/libcnet.so"
test ! -e "$incdir"
printf 'RELEASE_PACKAGE_PASS version=%s\n' "$version"
