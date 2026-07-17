/* Seal closed-set json_toolcall_v0 into an existing CNB (or create if absent).
 *
 * Usage:
 *   json_toolcall_seal <base.cnb> [--force]
 *
 * Exit 0 + JSON_TOOLCALL_SEAL_OK when unit is present and SoulHost serves it.
 * --force re-mines and refuses only if cnb_add_unit rejects different bytes
 * under the same name (then reports SEAL_REFUSED).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/json_toolcall.h"
#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/soul_host.h"
#include "../include/specialist.h"

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

static int mine_and_seal(CnetBase *base, int force) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork *student = NULL;
    Contract c;
    double inputs[CNET_JTC_N_TOOL * CNET_JTC_N_FEAT];
    double targets[CNET_JTC_N_TOOL * CNET_JTC_N_TOOL];
    int ti, j, reused = 0, rc;

    if (cnb_has_unit(base, CNET_JTC_UNIT_NAME) && !force) {
        printf("json_toolcall_seal: unit %s already present (use --force to remine)\n",
               CNET_JTC_UNIT_NAME);
        return 1; /* already sealed */
    }

    registry_init(&reg);
    if (cnet_jtc_v0_mine_admit(&reg, 0x4A54435F5345414CULL, &student, NULL) != 0 ||
        !student) {
        fprintf(stderr, "json_toolcall_seal: mine+admit failed\n");
        registry_free(&reg);
        return -1;
    }

    for (ti = 0; ti < CNET_JTC_N_TOOL; ti++) {
        cnet_jtc_encode(cnet_jtc_example_json(ti),
                        inputs + ti * CNET_JTC_N_FEAT);
        for (j = 0; j < CNET_JTC_N_TOOL; j++)
            targets[ti * CNET_JTC_N_TOOL + j] = (j == ti) ? 1.0 : 0.0;
    }
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, CNET_JTC_UNIT_NAME, student, inputs, targets,
                               CNET_JTC_N_TOOL) != 0 ||
        btn_certify(student, &c, NULL) != 0) {
        fprintf(stderr, "json_toolcall_seal: contract/certify failed\n");
        contract_free(&c);
        registry_free(&reg);
        return -1;
    }

    if (force && cnb_has_unit(base, CNET_JTC_UNIT_NAME)) {
        /* Same-name different bytes is refused by cnb_add_unit; report clearly. */
        printf("json_toolcall_seal: --force: attempting re-add (idempotent if identical)\n");
    }

    rc = cnb_add_unit(base, student, &c, &reused);
    contract_free(&c);
    if (rc != 0) {
        fprintf(stderr,
                "json_toolcall_seal: cnb_add_unit refused (name collision with different bytes?)\n");
        registry_free(&reg);
        return -2;
    }
    printf("json_toolcall_seal: %s %s\n", CNET_JTC_UNIT_NAME,
           reused ? "reused (identical)" : "sealed");
    /* student owned by process; base copies blob. free student after add. */
    btn_free(student);
    free(student);
    registry_free(&reg);
    return 0;
}

int main(int argc, char **argv) {
    const char *base_path = NULL;
    int force = 0, ai, rc;
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

    rc = mine_and_seal(&base, force);
    if (rc < 0) {
        cnb_free(&base);
        return 1;
    }
    if (rc == 0) {
        if (cnb_save(&base, base_path) != 0) {
            fprintf(stderr, "json_toolcall_seal: cnb_save failed\n");
            cnb_free(&base);
            return 1;
        }
        printf("json_toolcall_seal: saved %s units=%lu\n", base_path,
               (unsigned long)base.unit_count);
    }
    cnb_free(&base);

    if (verify_soul(base_path) != 0) {
        fprintf(stderr, "json_toolcall_seal: post-seal SoulHost verify FAILED\n");
        return 1;
    }
    printf("JSON_TOOLCALL_SEAL_OK base=%s unit=%s\n", base_path, CNET_JTC_UNIT_NAME);
    return 0;
}
