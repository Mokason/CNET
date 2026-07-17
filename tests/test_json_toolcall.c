/* Closed-set JSON tool-call spine: mine → certify → CNB → SoulHost serve.
 * make json_toolcall → JSON_TOOLCALL_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/json_toolcall.h"
#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/soul_host.h"
#include "../include/specialist.h"
#include "../include/nn.h"
#include "../include/router.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork *student = NULL;
    double feat[CNET_JTC_N_FEAT], tool[CNET_JTC_N_TOOL];
    const double *out;
    int i;
    const char *base_path = "tmp_json_toolcall.cnb";
    SoulHost *host = NULL;

    remove(base_path);
    remove("tmp_json_toolcall.cnb.tmp");
    unsetenv("CNET_RESIDUAL_GGUF");
    unsetenv("CNET_SOUL_RESIDUAL_HERMETIC");

    printf("== json toolcall v0 (closed-set) ==\n");
    registry_init(&reg);

    /* Encode / teacher exactness */
    for (i = 0; i < CNET_JTC_N_TOOL; i++) {
        const char *ex = cnet_jtc_example_json(i);
        check(ex && ex[0], "example json present");
        check(cnet_jtc_encode(ex, feat) == 0, "encode");
        check(cnet_jtc_hermetic_teacher(feat, tool, NULL) == 0 &&
                  cnet_jtc_decode_tool(tool) == i,
              "hermetic teacher maps example → tool");
    }

    /* Unknown open JSON still encodes (host parse elsewhere); teacher defaults */
    check(cnet_jtc_encode("{\"foo\":1}", feat) == 0, "encode open-ish json");
    check(cnet_jtc_hermetic_teacher(feat, tool, NULL) == 0 &&
              cnet_jtc_decode_tool(tool) == 5,
          "unknown shape → final (closed default)");

    check(cnet_jtc_ports_match(cnet_jtc_input_port(), cnet_jtc_output_port()),
          "ports_match spine");

    /* Mine + admit */
    check(cnet_jtc_v0_mine_admit(&reg, 0x4A54435F01ULL, &student, NULL) == 0 &&
              student != NULL,
          "mine+admit json_toolcall_v0");
    check(reg.count >= 1 && reg.entries[0].certified, "student certified");

    for (i = 0; i < CNET_JTC_N_TOOL; i++) {
        cnet_jtc_encode(cnet_jtc_example_json(i), feat);
        out = btn_forward(student, feat);
        check(out && cnet_jtc_decode_tool(out) == i,
              "student matches tool class");
    }

    /* Seal into CNB + SoulHost certified serve */
    {
        CnetBase base;
        Contract c;
        double inputs[CNET_JTC_N_TOOL * CNET_JTC_N_FEAT];
        double targets[CNET_JTC_N_TOOL * CNET_JTC_N_TOOL];
        int reused = 0, ti, j;

        for (ti = 0; ti < CNET_JTC_N_TOOL; ti++) {
            cnet_jtc_encode(cnet_jtc_example_json(ti),
                            inputs + ti * CNET_JTC_N_FEAT);
            for (j = 0; j < CNET_JTC_N_TOOL; j++)
                targets[ti * CNET_JTC_N_TOOL + j] = (j == ti) ? 1.0 : 0.0;
        }
        memset(&c, 0, sizeof c);
        check(contract_init_borrowed(&c, CNET_JTC_UNIT_NAME, student, inputs,
                                     targets, CNET_JTC_N_TOOL) == 0,
              "contract for seal");
        check(btn_certify(student, &c, NULL) == 0, "re-certify for seal");
        cnb_init(&base);
        check(cnb_add_unit(&base, student, &c, &reused) == 0, "cnb_add_unit");
        check(cnb_save(&base, base_path) == 0, "cnb_save");
        contract_free(&c);
        cnb_free(&base);
    }

    check(soul_open(base_path, NULL, &host) == 0 && host, "soul_open sealed CNB");
    for (i = 0; i < CNET_JTC_N_TOOL; i++) {
        double sout[CNET_JTC_N_TOOL];
        int rc;
        cnet_jtc_encode(cnet_jtc_example_json(i), feat);
        memset(sout, 0, sizeof sout);
        rc = soul_request(host, PORT_RAW, CNET_JTC_N_FEAT, 1, "jtc_feat",
                          PORT_ONEHOT, CNET_JTC_N_TOOL, 1, "json_tool",
                          feat, CNET_JTC_N_FEAT, sout, CNET_JTC_N_TOOL);
        check(rc == CNET_JTC_N_TOOL &&
                  soul_last_source(host) == SOUL_SOURCE_CERTIFIED &&
                  cnet_jtc_decode_tool(sout) == i,
              "SoulHost certified serve tool class");
    }
    /* Named run */
    {
        double sout[CNET_JTC_N_TOOL];
        cnet_jtc_encode(cnet_jtc_example_json(0), feat);
        check(soul_run(host, CNET_JTC_UNIT_NAME, feat, sout, CNET_JTC_N_TOOL) ==
                      CNET_JTC_N_TOOL &&
                  cnet_jtc_decode_tool(sout) == 0,
              "soul_run json_toolcall_v0 calculator");
    }

    soul_close(host);

    /* ensure_sealed idempotent on already-sealed base */
    {
        CnetBase b2;
        cnb_init(&b2);
        check(cnb_load(&b2, base_path) == 0, "reload sealed base");
        check(cnet_jtc_ensure_sealed(&b2, NULL) == 1, "ensure_sealed already");
        cnb_free(&b2);
    }

    registry_free(&reg);
    /* student borrowed by registry; free process-owned after free */
    if (student) {
        btn_free(student);
        free(student);
    }
    remove(base_path);
    remove("tmp_json_toolcall.cnb.tmp");

    if (failures) {
        printf("JSON_TOOLCALL_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("JSON_TOOLCALL_PASS checks=%d\n", checks);
    return 0;
}
