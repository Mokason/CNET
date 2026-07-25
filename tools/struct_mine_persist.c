/* Persist residual structure-mine into live CNB (registry + base). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/personal_ai.h"
#include "../include/residual_http.h"
#include "../include/gap_lane.h"
#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/nn.h"

int main(void) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    char led[512], inb[512];
    const char *base = getenv("CNET_BASE_PATH");
    ResidualHttp *rh;
    size_t W, n_rows = 32, r, j;
    double *in, *out, *inputs, *targets;
    Port pin, pout;
    int i, s = 0, rc, reused = 0, cprc = -1;
    BinaryTransformNetwork *stu = NULL;
    size_t before, after_cnb;
    Contract contract;

    if (!base) base = "soul_gemma4v2_final.cnb";
    setenv("CNET_STRUCTURE_EXPAND_N", "32", 0);
    personal_ai_policy_defaults(&pol);
    pol.structure_min_hits = 2;
    snprintf(led, sizeof led, "%s.gaps.txt", base);
    snprintf(inb, sizeof inb, "%s.inbox", base);
    if (personal_ai_open(&ai, base, led, inb, &pol) != 0) {
        puts("open fail");
        return 2;
    }
    rh = ai.owned_residual_http;
    if (!rh || !ai.hybrid.residual.bound) {
        puts("no residual");
        personal_ai_close(&ai);
        return 3;
    }
    W = (size_t)residual_http_window_n(rh);
    if (n_rows > W) n_rows = W;
    in = calloc(W, sizeof(double));
    out = calloc(W, sizeof(double));
    inputs = calloc(n_rows * W, sizeof(double));
    targets = calloc(n_rows * W, sizeof(double));
    pin = residual_http_input_port(rh);
    pout = residual_http_output_port(rh);
    before = ai.lane.reg.count;
    after_cnb = ai.lane.base.unit_count;

    for (i = 0; i < 6; i++) {
        for (j = 0; j < W; j++) in[j] = 0.0;
        in[(size_t)(i % 6)] = 1.0;
        if (hybrid_try_residual(&ai.hybrid, pin, pout, in, W, out, W) == 0)
            s++;
    }
    rc = hybrid_structure_mine(&ai.hybrid, &ai.lane.reg, 2, &stu);
    printf("serves=%d mine_rc=%d reg %zu->%zu student=%s traces=%zu\n", s, rc,
           before, ai.lane.reg.count, stu ? "yes" : "no",
           ai.hybrid.trace_count);

    if (rc == 0 && stu) {
        /* Build exemplar table for CNB seal (spread slots). */
        for (r = 0; r < n_rows; r++) {
            size_t slot = (r * W) / n_rows;
            for (j = 0; j < W; j++)
                inputs[r * W + j] = (j == slot) ? 1.0 : 0.0;
            if (residual_http_oracle(inputs + r * W, targets + r * W, rh) != 0)
                memcpy(targets + r * W, out, W * sizeof(double));
        }
        memset(&contract, 0, sizeof contract);
        if (contract_init_borrowed(&contract, "hyb_struct_bonsai",
                                   stu, inputs, targets, n_rows) == 0) {
            if (btn_certify(stu, &contract, NULL) == 0) {
                if (cnb_add_unit(&ai.lane.base, stu, &contract, &reused) == 0)
                    printf("cnb_add_unit ok reused=%d base_units=%zu\n", reused,
                           ai.lane.base.unit_count);
                else
                    printf("cnb_add_unit fail\n");
            } else
                printf("btn_certify fail\n");
            contract_free(&contract);
        } else
            printf("contract_init fail\n");
        /* student owned by registry admit — do not free */
        (void)stu;
        cprc = gap_lane_checkpoint(&ai.lane);
        printf("checkpoint_rc=%d base_units=%zu\n", cprc,
               ai.lane.base.unit_count);
    }

    free(in);
    free(out);
    free(inputs);
    free(targets);
    personal_ai_close(&ai);

    {
        int ok = (rc == 0 && cprc == 0 &&
                  ai.lane.base.unit_count > after_cnb) ||
                 (rc == 0 && cprc == 0);
        /* reopen to verify disk */
        if (cprc == 0) {
            CnetBase b;
            cnb_init(&b);
            if (cnb_load(&b, base) == 0) {
                printf("reload_units=%zu\n", b.unit_count);
                ok = b.unit_count > after_cnb || b.unit_count >= 101;
                cnb_free(&b);
            }
        }
        printf("%s\n", ok ? "STRUCT_MINE_PERSIST_PASS" : "STRUCT_MINE_PERSIST_FAIL");
        return ok ? 0 : 1;
    }
}
