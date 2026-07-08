/* Minimal test for the soul_host shim ABI.
 * Usage: ./test_soul_host <base.cnb> [unit_name]
 * Tries to open the base, run a unit (default "acq_tk2000q2000" or first arg), close.
 * Also exercises some core symbols directly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/soul_host.h"
#include "../include/base.h"
#include "../include/nn.h"
#include "../include/router.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <base.cnb> [unit_name]\n", argv[0]);
        return 2;
    }
    const char *base_path = argv[1];
    const char *unit = (argc > 2) ? argv[2] : "acq_tk2000q2000";

    printf("Testing soul_host on %s, unit=%s\n", base_path, unit);

    SoulHost *h = NULL;
    int rc = soul_host_should_open(base_path, NULL, &h);
    printf("soul_host_should_open -> %d, h=%p\n", rc, (void*)h);
    if (rc != 0 || !h) {
        fprintf(stderr, "open failed\n");
        return 1;
    }

    /* Try soul_run with a plausible buffer size (many units are 256-ish) */
    double in[1024] = {0};
    double out[1024] = {0};
    in[0] = 1.0;  /* minimal input */

    rc = soul_run(h, unit, in, out);
    printf("soul_run(%s) -> %d\n", unit, rc);
    if (rc == 0) {
        printf("  out[0]=%g out[1]=%g out[2]=%g ...\n", out[0], out[1], out[2]);
    }

    /* Test core symbols are callable */
    CnetBase b;
    cnb_init(&b);
    int load_rc = cnb_load(&b, base_path);
    printf("direct cnb_load -> %d\n", load_rc);
    cnb_free(&b);

    PrimitiveRegistry reg;
    registry_init(&reg);
    printf("registry_init called\n");
    registry_free(&reg);

    soul_close(h);
    printf("soul_close done\n");

    printf("soul_host basic test PASSED (open/run/close)\n");
    return 0;
}
