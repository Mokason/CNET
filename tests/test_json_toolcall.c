/* Closed-set JSON tool-call spine: mine → certify → CNB → SoulHost serve.
 * make json_toolcall → JSON_TOOLCALL_PASS
 */
#include <stdio.h>
#include "../include/cnet_platform.h"
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

static int tool_id(const char *name) {
    int i;
    for (i = 0; i < CNET_JTC_N_TOOL; ++i)
        if (strcmp(cnet_jtc_tool_names()[i], name) == 0) return i;
    return -1;
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
    cnet_unsetenv("CNET_RESIDUAL_GGUF");
    cnet_unsetenv("CNET_SOUL_RESIDUAL_HERMETIC");

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
    {
        int final_id = -1;
        for (i = 0; i < CNET_JTC_N_TOOL; i++)
            if (strcmp(cnet_jtc_tool_names()[i], "final") == 0) final_id = i;
        check(final_id >= 0, "final tool present in alphabet");
        check(cnet_jtc_encode("{\"foo\":1}", feat) == 0, "encode open-ish json");
        check(cnet_jtc_hermetic_teacher(feat, tool, NULL) == 0 &&
                  cnet_jtc_decode_tool(tool) == final_id,
              "unknown shape → final (closed default)");
    }

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

    /* Fixed held-out surface set. None of these JSON strings is used by
       cnet_jtc_v0_mine_admit or the seal contract above/below. The explicit
       cases measure syntax/order/case robustness; implicit cases measure the
       learned argument-feature path rather than the literal tool-name bit. */
    {
        static const struct { const char *json, *expect; } heldout[] = {
            {"{ \"args\": {\"expr\":\"17*3\"}, \"tool\":\"CALCULATOR\" }", "calculator"},
            {"{\"tool\":\"calculator\",\"args\":{\"expr\":\"sqrt(81)\"},\"id\":9}", "calculator"},
            {"{\"args\":{\"key\":\"timezone\",\"value\":\"Riga\"},\"tool\":\"MEMORY_STORE\"}", "memory_store"},
            {"{\"tool\":\"memory_store\",\"args\":{\"value\":\"blue\",\"key\":\"color\"}}", "memory_store"},
            {"{\"args\":{\"key\":\"project\"},\"tool\":\"MEMORY_RECALL\"}", "memory_recall"},
            {"{\"tool\":\"memory_recall\",\"args\":{\"key\":\"hardware\"},\"trace\":true}", "memory_recall"},
            {"{\"args\":{\"path\":\"docs/README.md\"},\"tool\":\"FILE_READ\"}", "file_read"},
            {"{\"tool\":\"file_read\",\"args\":{\"path\":\"LICENSE\"},\"id\":3}", "file_read"},
            {"{\"args\":{\"cond\":1,\"current\":0},\"tool\":\"CNET_RECALL\"}", "cnet_recall"},
            {"{\"tool\":\"cnet_recall\",\"args\":{\"current\":1,\"cond\":0},\"meta\":{}}", "cnet_recall"},
            {"{\"args\":{\"query\":\"bounded oracle\"},\"tool\":\"WEB_SEARCH\"}", "web_search"},
            {"{\"tool\":\"web_search\",\"args\":{\"query\":\"CNET runtime\"},\"limit\":4}", "web_search"},
            {"{\"args\":{\"query\":\"Ada Lovelace\"},\"tool\":\"WIKI_LOOKUP\"}", "wiki_lookup"},
            {"{\"tool\":\"wiki_lookup\",\"args\":{\"query\":\"ternary computing\"},\"lang\":\"en\"}", "wiki_lookup"},
            {"{\"final\":\"done\",\"tool\":\"FINAL\"}", "final"},
            {"{\"tool\":\"final\",\"answer\":\"complete\",\"ok\":true}", "final"},
            {"{\"args\":{\"expr\":\"9+8\"}}", "calculator"},
            {"{\"args\":{\"key\":\"k\",\"value\":\"v\"}}", "memory_store"},
            {"{\"args\":{\"query\":\"only-local-memory\"}}", "memory_recall"},
            {"{\"args\":{\"path\":\"docs/guide.md\"}}", "file_read"},
            {"{\"args\":{\"cond\":1,\"current\":1}}", "cnet_recall"},
            {"{\"answer\":\"all done\"}", "final"},
            {"{\"args\":{\"query\":\"recall this phrase\"}}", "memory_recall"},
            {"{\"final\":\"finished\"}", "final"}
        };
        int exact = 0;
        size_t hi;
        for (hi = 0; hi < sizeof heldout / sizeof heldout[0]; ++hi) {
            int expected_id = tool_id(heldout[hi].expect);
            cnet_jtc_encode(heldout[hi].json, feat);
            out = btn_forward(student, feat);
            if (out && cnet_jtc_decode_tool(out) == expected_id) {
                exact++;
            } else {
                int got_id = out ? cnet_jtc_decode_tool(out) : -1;
                printf("  JTC_HELDOUT_MISS index=%zu expected=%s got=%s\n", hi,
                       heldout[hi].expect,
                       got_id >= 0 ? cnet_jtc_tool_names()[got_id] : "none");
            }
        }
        printf("JTC_HELDOUT exact=%d total=%zu accuracy=%.4f exact_json_overlap=0\n",
               exact, sizeof heldout / sizeof heldout[0],
               (double)exact / (double)(sizeof heldout / sizeof heldout[0]));
        check(exact == 24, "held-out JSON tool-call accuracy = 24/24");
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
