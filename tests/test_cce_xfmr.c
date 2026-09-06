#include "cce/cce_xfmr.h"
#ifndef CCE_XFMR_CPU_ONLY
#include "cce/cce_amdmath.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    printf("cce_xfmr %s\n", cce_xfmr_version());
    cce_xfmr_config cfg = {
        .n_layer = 2,
        .n_embd = 32,
        .n_head = 4,
        .n_hidden = 64,
        .vocab = 16,
        .block_size = 8,
        .seed = 7u,
    };
    cce_xfmr *m = cce_xfmr_create(&cfg);
    if (!m) {
        printf("FAIL create\n");
        return 1;
    }
#ifndef CCE_XFMR_CPU_ONLY
    cce_amdmath *g = NULL;
    char name[256];
    g = cce_amdmath_open(name, sizeof name);
    if (g) {
        cce_xfmr_attach_gpu(m, g);
        printf("gpu %s\n", name);
    } else {
        printf("gpu none (CPU GEMM)\n");
    }
#endif
    printf("params=%d\n", cce_xfmr_param_count(m));
    int toks[8] = {1, 2, 3, 1, 2, 3, 1, 2};
    float *logits = malloc(sizeof(float) * 8 * 16);
    if (cce_xfmr_forward(m, toks, 8, logits) != 0) {
        printf("FAIL forward\n");
        return 1;
    }
    printf("ok   forward\n");
    double l0 = cce_xfmr_train_ce(m, toks, 8, 0.05f);
    double l1 = l0;
    for (int i = 0; i < 40; i++)
        l1 = cce_xfmr_train_ce(m, toks, 8, 0.05f);
    printf("loss %.4f -> %.4f\n", l0, l1);
    int ok = (l0 > 0) && (l1 < l0);

    /* joint QAT: ternary forward + STE into FP shadow (not post-hoc) */
    cce_xfmr *q = cce_xfmr_create(&cfg);
    if (!q) {
        printf("FAIL qat create\n");
        return 1;
    }
    cce_xfmr_set_ternary(q, 1);
#ifndef CCE_XFMR_CPU_ONLY
    if (g)
        cce_xfmr_attach_gpu(q, g);
#endif
    double q0 = cce_xfmr_train_ce(q, toks, 8, 0.05f);
    double q1 = q0;
    for (int i = 0; i < 40; i++)
        q1 = cce_xfmr_train_ce(q, toks, 8, 0.05f);
    printf("qat  %.4f -> %.4f  ternary=%d\n", q0, q1, cce_xfmr_ternary(q));
    int8_t *codes = malloc((size_t)16 * 32);
    float *gamma = malloc(sizeof(float) * 16);
    float Wsm[8];
    for (int i = 0; i < 8; i++)
        Wsm[i] = (i - 3) * 0.1f;
    if (cce_xfmr_pack_linear(Wsm, 2, 4, codes, gamma) != 0) {
        printf("FAIL pack\n");
        return 1;
    }
    int in_range = 1;
    for (int i = 0; i < 8; i++)
        if (codes[i] < -1 || codes[i] > 1)
            in_range = 0;
    printf("ok   pack codes in {-1,0,1}  g0=%.4f\n", gamma[0]);
    char pdir[256];
    if (cce_xfmr_propose(q, "xfmr_head_toy", "/tmp/cnet_xfmr_inbox", pdir, sizeof pdir) !=
        0) {
        printf("FAIL propose\n");
        return 1;
    }
    char chk[320];
    snprintf(chk, sizeof chk, "%s/PROPOSE.json", pdir);
    FILE *pf = fopen(chk, "r");
    if (!pf) {
        printf("FAIL propose json missing\n");
        return 1;
    }
    char buf[128];
    int saw_no_cert = 0, saw_pending = 0;
    while (fgets(buf, sizeof buf, pf)) {
        if (strstr(buf, "\"auto_cert\": false"))
            saw_no_cert = 1;
        if (strstr(buf, "pending_verify"))
            saw_pending = 1;
    }
    fclose(pf);
    snprintf(chk, sizeof chk, "%s/unit.cnb", pdir);
    FILE *cnb = fopen(chk, "r");
    int no_cnb = (cnb == NULL);
    if (cnb)
        fclose(cnb);
    printf("ok   propose %s  no_cnb=%d auto_cert=0 pending=%d\n", pdir, no_cnb,
           saw_pending);
    ok = ok && (q1 < q0) && in_range && saw_no_cert && saw_pending && no_cnb;
    cce_xfmr_free(q);
    free(codes);
    free(gamma);
    cce_xfmr_free(m);
#ifndef CCE_XFMR_CPU_ONLY
    if (g)
        cce_amdmath_close(g);
#endif
    free(logits);
    if (!ok) {
        printf("XFMR_FAIL loss did not drop\n");
        return 1;
    }
    printf("XFMR_PASS\n");
    return 0;
}
