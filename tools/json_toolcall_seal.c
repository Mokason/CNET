/* Seal closed-set json_toolcall unit (v1 alphabet) into an existing CNB.
 *
 * Usage:
 *   json_toolcall_seal <base.cnb> [--force]
 *
 * --force is accepted for CLI compatibility; identical unit is always
 * idempotent via cnb_add_unit. Different bytes under same name still refuse.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/json_toolcall.h"
#include "../include/base.h"
#include "../include/nn.h"
#include "../include/soul_host.h"

static int verify_soul(const char *base_path) {
    SoulHost *host = NULL;
    double feat[CNET_JTC_N_FEAT], out[CNET_JTC_N_TOOL];
    int i, ok = 1;

    if (soul_open(base_path, NULL, &host) != 0 || !host) {
        fprintf(stderr, "json_toolcall_seal: soul_open failed\n");
        return -1;
    }
    for (i = 0; i < CNET_JTC_N_TOOL; i++) {
        int rc;
        cnet_jtc_encode(cnet_jtc_example_json(i), feat);
        memset(out, 0, sizeof out);
        rc = soul_request(host, PORT_RAW, CNET_JTC_N_FEAT, 1, "jtc_feat",
                          PORT_ONEHOT, CNET_JTC_N_TOOL, 1, "json_tool",
                          feat, CNET_JTC_N_FEAT, out, CNET_JTC_N_TOOL);
        if (rc != CNET_JTC_N_TOOL ||
            soul_last_source(host) != SOUL_SOURCE_CERTIFIED ||
            cnet_jtc_decode_tool(out) != i) {
            fprintf(stderr, "json_toolcall_seal: verify fail tool=%d rc=%d src=%d\n",
                    i, rc, soul_last_source(host));
            ok = 0;
            break;
        }
    }
    soul_close(host);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    const char *base_path = NULL;
    int force = 0, ai, erc;
    CnetBase base;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--force") == 0)
            force = 1;
        else if (argv[ai][0] != '-')
            base_path = argv[ai];
        else {
            fprintf(stderr, "usage: %s <base.cnb> [--force]\n", argv[0]);
            return 2;
        }
    }
    if (!base_path) {
        fprintf(stderr, "usage: %s <base.cnb> [--force]\n", argv[0]);
        return 2;
    }
    (void)force;

    cnb_init(&base);
    if (access(base_path, F_OK) == 0) {
        if (cnb_load(&base, base_path) != 0) {
            fprintf(stderr, "json_toolcall_seal: cannot load %s\n", base_path);
            return 1;
        }
        printf("json_toolcall_seal: loaded %s units=%lu\n", base_path,
               (unsigned long)base.unit_count);
    } else {
        printf("json_toolcall_seal: creating new base %s\n", base_path);
    }

    erc = cnet_jtc_ensure_sealed(&base, NULL);
    if (erc < 0) {
        fprintf(stderr, "json_toolcall_seal: ensure_sealed failed rc=%d\n", erc);
        cnb_free(&base);
        return 1;
    }
    if (erc == 0) {
        if (cnb_save(&base, base_path) != 0) {
            fprintf(stderr, "json_toolcall_seal: cnb_save failed\n");
            cnb_free(&base);
            return 1;
        }
        printf("json_toolcall_seal: %s sealed units=%lu\n", CNET_JTC_UNIT_NAME,
               (unsigned long)base.unit_count);
    } else {
        printf("json_toolcall_seal: unit %s already present\n", CNET_JTC_UNIT_NAME);
    }
    cnb_free(&base);

    if (verify_soul(base_path) != 0) {
        fprintf(stderr, "json_toolcall_seal: post-seal SoulHost verify FAILED\n");
        return 1;
    }
    printf("JSON_TOOLCALL_SEAL_OK base=%s unit=%s\n", base_path, CNET_JTC_UNIT_NAME);
    return 0;
}
