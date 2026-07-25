/* Direct residual→structure-mine (bypass Tier A route steal). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/hybrid_ai.h"
#include "../include/residual_http.h"
#include "../include/router.h"
#include "../include/base.h"
#include "../include/nn.h"

int main(void) {
    ResidualHttp *rh = NULL;
    HybridAi h;
    PrimitiveRegistry reg;
    const char *url = getenv("CNET_RESIDUAL_HTTP");
    const char *win = getenv("CNET_RESIDUAL_WINDOW");
    const char *base = getenv("CNET_BASE_PATH");
    size_t W;
    double *in, *out;
    Port pin, pout;
    int i, served = 0, rc;
    BinaryTransformNetwork *stu = NULL;
    CnetBase cnb;

    if (!url) url = "http://127.0.0.1:8080";
    if (!base) base = "soul_gemma4v2_final.cnb";
    setenv("CNET_STRUCTURE_EXPAND_N", "32", 0);

    if (residual_http_open(&rh, url, win, 32) != 0 || !rh) {
        fprintf(stderr, "http open fail\n");
        return 2;
    }
    hybrid_ai_init(&h);
    if (hybrid_bind_residual(&h, "residual_http", residual_http_oracle, rh) !=
        0) {
        fprintf(stderr, "bind fail\n");
        return 3;
    }
    /* Live registry so mine_admit can add a unit */
    cnb_init(&cnb);
    if (cnb_load(&cnb, base) != 0) {
        fprintf(stderr, "cnb_load fail\n");
        return 4;
    }
    memset(&reg, 0, sizeof reg);
    /* empty reg for clean admit — still works */
    registry_init(&reg);

    W = (size_t)residual_http_window_n(rh);
    in = calloc(W, sizeof(double));
    out = calloc(W, sizeof(double));
    pin = residual_http_input_port(rh);
    pout = residual_http_output_port(rh);
    printf("DIRECT_MINE W=%zu\n", W);
    for (i = 0; i < 6; i++) {
        size_t j;
        for (j = 0; j < W; j++) in[j] = 0.0;
        in[(size_t)(i % 6)] = 1.0;
        if (hybrid_try_residual(&h, pin, pout, in, W, out, W) == 0) served++;
    }
    printf("direct_residual_serves=%d traces=%zu\n", served, h.trace_count);
    for (i = 0; i < (int)h.trace_count && i < 4; i++)
        printf("  trace hits=%llu dim=%zu\n",
               (unsigned long long)h.traces[i].hits, h.traces[i].in_dim);

    rc = hybrid_structure_mine(&h, &reg, 2, &stu);
    printf("mine_rc=%d student=%s reg_units=%zu\n", rc, stu ? "yes" : "no",
           reg.count);
    if (stu) {
        printf("student in=%zu out=%zu\n", stu->input_count, stu->output_count);
        /* try admit already done inside mine */
        btn_free(stu);
        free(stu);
    }
    registry_free(&reg);
    cnb_free(&cnb);
    hybrid_ai_free(&h);
    residual_http_close(rh);
    printf("%s\n", (rc == 0 && served >= 2) ? "STRUCT_MINE_LARGE_PASS"
                                            : "STRUCT_MINE_LARGE_FAIL");
    return rc == 0 ? 0 : 1;
}
