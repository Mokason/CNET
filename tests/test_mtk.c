/* CNET Micro-Trensor Kernel phases 1–5 — make mtk → MTK_PASS */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_mtk.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_block.h"

static int failures, checks;
static int gemm_hook_calls;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void gemm_probe(void *ctx) {
    (void)ctx;
    gemm_hook_calls++;
}

static void dummy_kv_flush(void *ctx) {
    int *c = (int *)ctx;
    if (c) (*c)++;
}

/* Phase 1: 9/9 needles CMSK */
static void needle_cmsk(void) {
    float W[64];
    cce_mtk *m = NULL;
    uint32_t idx[9];
    float val[9];
    cce_mtk_skill_tensor st;
    int i, hit = 0;
    const char *path = "mtk_needles.cmsk";
    int flush_n = 0;

    for (i = 0; i < 64; ++i) W[i] = 0.01f * (float)i;
    for (i = 0; i < 9; ++i) {
        idx[i] = (uint32_t)(3 + i * 5);
        val[i] = 100.f + (float)i;
    }
    st.site_name = "knowledge.b0.w";
    st.n_elem = 64;
    st.nnz = 9;
    st.idx = idx;
    st.val = val;

    check(cce_mtk_open(&m) == CCE_OK && m, "mtk open");
    if (!m) return;
    cce_mtk_set_kv_flush(m, dummy_kv_flush, &flush_n);
    check(cce_mtk_register(m, "knowledge.b0.w", W, 64) == CCE_OK, "register");
    check(cce_mtk_write_cmsk(path, CCE_MTK_METHOD_DELTA, &st, 1) == CCE_OK,
          "write cmsk needles");
    check(cce_mtk_apply_file(m, path, 1.f) == CCE_OK, "apply needles");
    check(flush_n >= 1, "kv flush on apply");
    for (i = 0; i < 9; ++i) {
        float expect = 0.01f * (float)idx[i] + val[i];
        if (fabsf(W[idx[i]] - expect) < 1e-4f) hit++;
    }
    check(hit == 9, "9/9 needles present after swap");
    printf("    needles hit=%d/9 apply_us=%llu\n", hit,
           (unsigned long long)m->apply_us);
    check(cce_mtk_revert(m) == CCE_OK, "revert");
    check(flush_n >= 2, "kv flush on revert");
    {
        int ok = 1;
        for (i = 0; i < 64; ++i)
            if (fabsf(W[i] - 0.01f * (float)i) > 1e-5f) ok = 0;
        check(ok, "exact base restore after revert");
    }
    cce_mtk_close(m);
    remove(path);
}

/* Phase 2: MTSK ternary */
static void mtsk_ternary(void) {
    float W[32];
    cce_mtk *m = NULL;
    uint32_t idx[4] = {1, 5, 10, 20};
    float val[4] = {1.f, -1.f, 1.f, -1.f};
    cce_mtk_skill_tensor st;
    const char *path = "mtk_ternary.tskill";
    int i, hit = 0;

    for (i = 0; i < 32; ++i) W[i] = 1.0f;
    st.site_name = "layer.w";
    st.n_elem = 32;
    st.nnz = 4;
    st.idx = idx;
    st.val = val;

    check(cce_mtk_open(&m) == CCE_OK, "mtsk open");
    if (!m) return;
    check(cce_mtk_register(m, "layer.w", W, 32) == CCE_OK, "mtsk register");
    check(cce_mtk_write_mtsk(path, &st, 1) == CCE_OK, "write mtsk");
    check(cce_mtk_apply_file(m, path, 0.1f) == CCE_OK, "apply mtsk via auto");
    check(cce_mtk_tensors_patched(m) == 1, "mtsk patched 1");
    for (i = 0; i < 4; ++i) {
        float expect = 1.0f + 0.1f * val[i];
        if (fabsf(W[idx[i]] - expect) < 1e-4f) hit++;
    }
    check(hit == 4, "mtsk ternary soft-overwrite values");
    check(cce_mtk_revert(m) == CCE_OK, "mtsk revert");
    {
        int ok = 1;
        for (i = 0; i < 32; ++i)
            if (fabsf(W[i] - 1.0f) > 1e-5f) ok = 0;
        check(ok, "mtsk exact restore");
    }
    cce_mtk_close(m);
    remove(path);
}

/* Phase 3: kernel vtable + block hook */
static void kernel_hook(void) {
    cce_cascade *cas = NULL;
    cce_tensor in = {0}, out = {0};
    int sh[1] = {4};
    cce_mtk_kernel_table *kt = cce_mtk_global_kernels();

    gemm_hook_calls = 0;
    cce_mtk_kernel_set(kt, CCE_MTK_K_GEMM, gemm_probe);
    cce_mtk_install_block_hook();

    check(cce_cascade_create(&cas, 1) == CCE_OK, "hook cascade");
    check(cce_cascade_add_linear(cas, 4, 4, 0.05f) == CCE_OK, "hook linear");
    check(cce_tensor_alloc(&in, sh, 1) == CCE_OK, "hook in");
    {
        int i;
        for (i = 0; i < 4; ++i) in.data[i] = 1.f;
    }
    check(cce_cascade_forward(cas, &in, &out) == CCE_OK, "hook forward");
    check(gemm_hook_calls >= 1, "GEMM kernel slot invoked from block_forward");
    check(out.data != NULL, "hook still produced output (fall-through)");

    cce_mtk_uninstall_block_hook();
    cce_mtk_kernel_set(kt, CCE_MTK_K_GEMM, NULL);
    cce_tensor_free(&in);
    cce_tensor_free(&out);
    cce_cascade_destroy(cas);
}

/* Phase 4: GGUF-style registry (tok_emb + forest) without real GGUF file */
static void gguf_registry(void) {
    cce_gguf_qwen2 model;
    cce_mtk *m = NULL;
    cce_forest *f = NULL;
    cce_cascade *cas = NULL;
    float emb[16];
    int i;
    const char *arch = "mtk_gguf_reg.cce";

    memset(&model, 0, sizeof model);
    for (i = 0; i < 16; ++i) emb[i] = (float)i;
    model.tok_emb.data = emb;
    model.tok_emb.numel = 16;
    model.n_layer = 0;
    model.max_ctx = 8;
    model.k_slot_floats = 4;
    model.v_slot_floats = 4;
    model.k_cache = (float *)calloc(8 * 4, sizeof(float));
    model.v_cache = (float *)calloc(8 * 4, sizeof(float));
    model.cur_pos = 3;
    if (model.k_cache) model.k_cache[0] = 99.f;

    remove(arch);
    check(cce_forest_open(&f, arch, 4) == CCE_OK, "gguf reg forest");
    check(cce_cascade_create(&cas, 1) == CCE_OK, "gguf reg cas");
    check(cce_cascade_add_linear(cas, 4, 4, 0.1f) == CCE_OK, "gguf reg lin");
    check(cce_forest_add_branch(f, cas, "blk0_q") == CCE_OK, "gguf reg branch");
    cas = NULL;
    model.forest = f;

    check(cce_mtk_open(&m) == CCE_OK, "gguf mtk open");
    cce_mtk_set_kv_flush(m, cce_mtk_gguf_kv_flush, &model);
    check(cce_mtk_bind_gguf(m, &model) == CCE_OK, "bind_gguf");
    check(cce_mtk_n_sites(m) >= 2, "tok_emb + forest sites");
    {
        /* apply any skill to trigger flush */
        uint32_t idx[1] = {0};
        float val[1] = {0.5f};
        cce_mtk_skill_tensor st = {"tok_emb", 16, 1, idx, val};
        check(cce_mtk_write_cmsk("mtk_emb.cmsk", CCE_MTK_METHOD_DELTA, &st, 1) ==
                  CCE_OK,
              "write emb skill");
        check(cce_mtk_apply_file(m, "mtk_emb.cmsk", 1.f) == CCE_OK,
              "apply emb skill");
        check(model.cur_pos == 0, "gguf kv flush zeroed cur_pos");
        check(model.k_cache && model.k_cache[0] == 0.f, "gguf k_cache cleared");
    }
    cce_mtk_close(m);
    cce_forest_close(f);
    free(model.k_cache);
    free(model.v_cache);
    remove(arch);
    remove("mtk_emb.cmsk");
}

/* Phase 5: auto-router */
static void auto_router(void) {
    float W[16];
    cce_mtk *m = NULL;
    uint32_t idx[1] = {2};
    float val[1] = {7.f};
    cce_mtk_skill_tensor st = {"router.w", 16, 1, idx, val};
    FILE *rf;
    int i;

    for (i = 0; i < 16; ++i) W[i] = 0.f;
    check(cce_mtk_write_cmsk("skill_code.cmsk", CCE_MTK_METHOD_DELTA, &st, 1) ==
              CCE_OK,
          "router skill file");
    rf = fopen("mtk_routes.txt", "w");
    check(rf != NULL, "routes file create");
    if (rf) {
        fprintf(rf, "# MTK routes\n");
        fprintf(rf, "skill_code.cmsk  code,python,function\n");
        fprintf(rf, "missing.cmsk  nevermatch\n");
        fclose(rf);
    }

    check(cce_mtk_open(&m) == CCE_OK, "router mtk open");
    if (!m) return;
    check(cce_mtk_register(m, "router.w", W, 16) == CCE_OK, "router register");
    check(cce_mtk_router_load(m, "mtk_routes.txt") == CCE_OK, "router load");
    check(cce_mtk_router_n_routes(m) >= 1, "router has routes");
    check(cce_mtk_router_apply(m, "Please write a python function", 1.f) ==
              CCE_OK,
          "router apply on python prompt");
    check(fabsf(W[2] - 7.f) < 1e-4f, "router skill applied to weights");
    check(cce_mtk_router_apply(m, "hello weather today", 1.f) ==
              CCE_ERR_NOT_FOUND,
          "router no match");
    cce_mtk_close(m);
    remove("skill_code.cmsk");
    remove("mtk_routes.txt");
}

/* Forest forward identity (phase 1 retained) */
static void forest_fwd(void) {
    cce_forest *f = NULL;
    cce_cascade *cas = NULL;
    cce_mtk *m = NULL;
    cce_tensor in = {0}, out0 = {0}, out1 = {0};
    float base0 = 0, skill0 = 0;
    uint32_t idx[1] = {0};
    float val[1] = {5.0f};
    cce_mtk_skill_tensor st;
    const char *arch = "mtk_forest_test.cce";
    const char *skill = "mtk_forest_skill.cmsk";
    int sh[1] = {8};

    remove(arch);
    check(cce_forest_open(&f, arch, 8) == CCE_OK && f, "forest open");
    if (!f) return;
    check(cce_cascade_create(&cas, 2) == CCE_OK, "cascade create");
    check(cce_cascade_add_linear(cas, 8, 8, 0.1f) == CCE_OK, "add linear");
    check(cce_forest_add_branch(f, cas, "knowledge") == CCE_OK, "add branch");
    cas = NULL;

    check(cce_mtk_open(&m) == CCE_OK, "mtk open forest");
    check(cce_mtk_bind_forest(m, f) == CCE_OK, "bind forest");
    check(cce_mtk_n_sites(m) >= 1, "sites registered from forest");

    st.site_name = "knowledge.b0.w";
    st.n_elem = 8 * 8;
    st.nnz = 1;
    st.idx = idx;
    st.val = val;
    check(cce_mtk_write_cmsk(skill, CCE_MTK_METHOD_DELTA, &st, 1) == CCE_OK,
          "write forest skill");
    check(cce_tensor_alloc(&in, sh, 1) == CCE_OK, "alloc in");
    {
        int i;
        for (i = 0; i < 8; ++i) in.data[i] = 1.f;
    }
    {
        cce_cascade *c = cce_forest_get_resident(f, "knowledge");
        check(c != NULL, "resident knowledge");
        if (c) {
            check(cce_cascade_forward(c, &in, &out0) == CCE_OK, "fwd base");
            base0 = out0.data ? out0.data[0] : 0.f;
        }
    }
    check(cce_mtk_apply_file(m, skill, 1.f) == CCE_OK, "apply forest skill");
    {
        cce_cascade *c = cce_forest_get_resident(f, "knowledge");
        if (c) {
            cce_tensor_free(&out1);
            check(cce_cascade_forward(c, &in, &out1) == CCE_OK, "fwd skill");
            skill0 = out1.data ? out1.data[0] : 0.f;
        }
    }
    check(fabsf(skill0 - base0) > 1e-4f, "skill changes forward output");
    check(cce_mtk_revert(m) == CCE_OK, "revert forest skill");
    {
        cce_cascade *c = cce_forest_get_resident(f, "knowledge");
        float back = 0.f;
        if (c) {
            cce_tensor_free(&out0);
            check(cce_cascade_forward(c, &in, &out0) == CCE_OK, "fwd restored");
            back = out0.data ? out0.data[0] : 0.f;
        }
        check(fabsf(back - base0) < 1e-4f, "forward matches base after revert");
    }
    cce_tensor_free(&in);
    cce_tensor_free(&out0);
    cce_tensor_free(&out1);
    cce_mtk_close(m);
    cce_forest_close(f);
    remove(arch);
    remove(skill);
}

int main(void) {
    printf("== CNET Micro-Trensor Kernel (MTK) phases 1–5 ==\n");
    needle_cmsk();
    mtsk_ternary();
    kernel_hook();
    gguf_registry();
    auto_router();
    forest_fwd();
    printf("MTK_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
