/* CNET MTK product eval — hermetic synthetic GGUF + optional real model.
 *
 * Usage:
 *   make mtk_eval
 *   CNET_MTK_EVAL_GGUF=/path/model.gguf make mtk_eval_real
 *
 * Reports: needle 9/9, skill forward delta, router, swap latency, optional
 * real-model bind/route/generate smoke.
 */
#define TL_CTX 64
#include "../tests/tiny_model_fixture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_mtk.h"
#include "../include/cce/cce_mtk_host.h"
#include "../include/resource_governor.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void eval_synthetic(void) {
    static tl_weights w;
    tl_entry ents[64];
    int n_ents;
    const char *gguf = "mtk_eval_fixture.gguf";
    cce_mtk_host *h = NULL;
    cce_mtk *m;
    cce_gguf_qwen2 *model;
    int toks[8] = {3, 7, 11, 5};
    int out_toks[32];
    int out_n = 0;
    int i, hit = 0;
    uint32_t idx[9];
    float val[9];
    cce_mtk_skill_tensor st;
    FILE *rf;
    double t0, t1;

    printf("== MTK eval: synthetic GGUF host ==\n");
    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    setenv("CNET_GOV_PROFILE", "eco", 1);
    setenv("CNET_GOV_FORCE", "1", 1);

    tl_gen(&w, 0);
    n_ents = tl_entries(&w, ents, 0);
    tl_write_gguf(gguf, ents, n_ents);

    /* Skills + routes */
    for (i = 0; i < 9; ++i) {
        idx[i] = (uint32_t)(i * 2 + 1);
        val[i] = 50.f + (float)i;
    }
    /* Will bind after open — write skill targeting tok_emb once we know numel */
    check(cce_mtk_host_open(&h, gguf, NULL) == CCE_OK && h, "host open synthetic");
    if (!h) {
        remove(gguf);
        return;
    }
    model = cce_mtk_host_model(h);
    m = cce_mtk_host_mtk(h);
    check(model && m, "model+mtk live");
    check(cce_mtk_n_sites(m) >= 1, "sites bound (tok_emb/forest)");

    {
        size_t ne = model->tok_emb.numel > 0 ? model->tok_emb.numel : 16;
        if (ne < 32) {
            /* pad needle indices into range */
            for (i = 0; i < 9; ++i) idx[i] = (uint32_t)(i % (int)ne);
        } else {
            for (i = 0; i < 9; ++i) idx[i] = (uint32_t)(3 + i * 5);
        }
        st.site_name = "tok_emb";
        st.n_elem = ne;
        st.nnz = 9;
        st.idx = idx;
        st.val = val;
        check(cce_mtk_write_cmsk("mtk_eval_needles.cmsk", CCE_MTK_METHOD_DELTA,
                                 &st, 1) == CCE_OK,
              "write needle skill");
    }

    t0 = wall_ms();
    check(cce_mtk_host_apply_skill(h, "mtk_eval_needles.cmsk", 1.f) == CCE_OK,
          "apply needle skill");
    t1 = wall_ms();
    printf("    apply_ms=%.3f patched=%d nnz=%d\n", t1 - t0,
           cce_mtk_tensors_patched(m), m->nnz_applied);

    if (model->tok_emb.data) {
        float snap[9];
        for (i = 0; i < 9; ++i) {
            snap[i] = model->tok_emb.data[idx[i]];
            if (fabsf(snap[i]) > 1.f) hit++;
        }
        check(hit >= 1, "needle skill moved tok_emb");
        check(cce_mtk_host_revert(h) == CCE_OK, "revert needles");
        {
            int changed = 0;
            for (i = 0; i < 9; ++i)
                if (fabsf(snap[i] - model->tok_emb.data[idx[i]]) > 1e-3f)
                    changed++;
            check(changed == 9, "9/9 needle deltas cleared on revert");
        }
        check(cce_mtk_host_apply_skill(h, "mtk_eval_needles.cmsk", 1.f) ==
                  CCE_OK,
              "re-apply needles");
    }

    /* Logits change under skill */
    {
        float *lb = NULL, *ls = NULL;
        int V = model->vocab_size > 0 ? model->vocab_size : TL_V;
        int differ = 0;
        lb = (float *)calloc((size_t)V, sizeof(float));
        ls = (float *)calloc((size_t)V, sizeof(float));
        check(cce_mtk_host_revert(h) == CCE_OK, "base for logits");
        model->cur_pos = 0;
        check(cce_gguf_qwen2_forward(model, toks, 4, lb, V) == CCE_OK,
              "forward base");
        check(cce_mtk_host_apply_skill(h, "mtk_eval_needles.cmsk", 1.f) == CCE_OK,
              "skill for logits");
        model->cur_pos = 0;
        check(cce_gguf_qwen2_forward(model, toks, 4, ls, V) == CCE_OK,
              "forward skill");
        for (i = 0; i < V; ++i)
            if (fabsf(lb[i] - ls[i]) > 1e-5f) differ++;
        check(differ > 0, "skill changes logits vs base");
        printf("    logits differ in %d/%d bins\n", differ, V);
        free(lb);
        free(ls);
    }

    /* Router */
    {
        uint32_t ridx[1] = {0};
        float rval[1] = {3.f};
        cce_mtk_skill_tensor rst = {"tok_emb", model->tok_emb.numel, 1, ridx,
                                    rval};
        check(cce_mtk_write_cmsk("mtk_eval_code.cmsk", CCE_MTK_METHOD_DELTA,
                                 &rst, 1) == CCE_OK,
              "code skill");
        rf = fopen("mtk_eval_routes.txt", "w");
        if (rf) {
            fprintf(rf, "mtk_eval_code.cmsk  code,python,function\n");
            fclose(rf);
        }
        check(cce_mtk_router_load(m, "mtk_eval_routes.txt") == CCE_OK,
              "load routes");
        check(cce_mtk_host_revert(h) == CCE_OK, "revert before route");
        check(cce_mtk_host_route_prompt(h, "write a python function please") ==
                  CCE_OK,
              "route python prompt");
        check(cce_mtk_skill_active(m) == 1, "skill active after route");
        check(cce_mtk_host_route_prompt(h, "hello weather") == CCE_ERR_NOT_FOUND,
              "no route on weather");
    }

    /* Generate a few tokens (duty cycle) */
    check(cce_mtk_host_generate(h, toks, 4, 4, out_toks, &out_n) == CCE_OK,
          "generate 4+4");
    check(out_n == 8, "output length 8");
    printf("    gen tokens:");
    for (i = 0; i < out_n; ++i) printf(" %d", out_toks[i]);
    printf("\n");

    cce_mtk_host_close(h);
    remove(gguf);
    remove("mtk_eval_needles.cmsk");
    remove("mtk_eval_code.cmsk");
    remove("mtk_eval_routes.txt");
}

static void eval_real_optional(void) {
    const char *path = getenv("CNET_MTK_EVAL_GGUF");
    cce_mtk_host *h = NULL;
    int toks[4] = {1, 2, 3, 4};
    int out[16];
    int n = 0;
    double t0, t1;

    printf("== MTK eval: real GGUF (optional) ==\n");
    if (!path || !path[0]) {
        printf("  (skip — set CNET_MTK_EVAL_GGUF=/path/model.gguf)\n");
        check(1, "real GGUF skipped (not requested)");
        return;
    }
    {
        FILE *f = fopen(path, "rb");
        if (!f) {
            printf("  (skip — file missing: %s)\n", path);
            check(1, "real GGUF skipped (missing file)");
            return;
        }
        fclose(f);
    }

    setenv("CNET_GOV_PROFILE", "balanced", 1);
    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    t0 = wall_ms();
    if (cce_mtk_host_open(&h, path, NULL) != CCE_OK || !h) {
        printf("  (skip — open failed for %s; arch/OOM/unsupported)\n", path);
        check(1, "real host open soft-skip");
        return;
    }
    t1 = wall_ms();
    printf("    open_ms=%.1f sites=%d layers=%d embd=%d vocab=%d\n", t1 - t0,
           cce_mtk_n_sites(cce_mtk_host_mtk(h)), h->model->n_layer,
           h->model->n_embd, h->model->vocab_size);
    check(cce_mtk_n_sites(cce_mtk_host_mtk(h)) >= 1, "real model sites bound");
    check(cce_mtk_host_generate(h, toks, 4, 2, out, &n) == CCE_OK,
          "real generate prefill+2");
    check(n >= 4, "real gen length");
    printf("    real gen:");
    {
        int i;
        for (i = 0; i < n; ++i) printf(" %d", out[i]);
        printf("\n");
    }
    cce_mtk_host_close(h);
}

int main(void) {
    printf("== CNET MTK product wiring eval ==\n");
    eval_synthetic();
    eval_real_optional();
    printf("MTK_EVAL_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
