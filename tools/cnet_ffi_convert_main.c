/* CLI for residual FFI converter.
 *   ./bin/cnet_ffi_convert --test
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_ffi_convert.h"

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--test") == 0) return cnet_ffi_selftest();
    }
    fprintf(stderr, "usage: %s --test\n", argv[0]);
    return 2;
}
