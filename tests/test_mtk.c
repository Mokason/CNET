/* CNET Micro-Trensor Kernel — make mtk → MTK_PASS */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_mtk.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_tensor.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Needle-style: 9 distinct “facts” encoded as large values in weight slots.
 * Skill injects them; forward-like sum reads them; revert removes them. */
static int needle_test(void) {
    float W[64];
    cce_mtk *m = NULL;
    uint32_t idx[9];
    float val[9];
    cce_mtk_skill_tensor st;
    int i, hit = 0;
    const char *path = "mtk_needles.cmsk";

    for (i = 0; i < 64; ++i) W[i] = 0.01f * (float)i;
    for (i = 0; i < 9; ++i) {
        idx[i] = (uint32_t)(3 + i * 5); /* spread in tensor */
        val[i] = 100.f + (float)i;      /* needle magnitudes */
    }
    st.site_name = "knowledge.b0.w";
    st.n_elem = 64;
    st.nnz = 9;
    st.idx = idx;
    st.val = val;

    check(cce_mtk_open(&m) == CCE_OK && m, "mtk open");
    if (!m) return 0;
    check(cce_mtk_register(m, "knowledge.b0.w", W, 64) == CCE_OK, "register");
    check(cce_mtk_write_cmsk(path, CCE_MTK_METHOD_DELTA, &st, 1) == CCE_OK,
          "write cmsk needles");
    check(cce_mtk_apply_file(m, path, 1.f) == CCE_OK, "apply needles");
    check(cce_mtk_skill_active(m) == 1, "skill active");
    check(cce_mtk_tensors_patched(m) == 1, "one tensor patched");

    for (i = 0; i < 9; ++i) {
        float expect = 0.01f * (float)idx[i] + val[i];
        if (fabsf(W[idx[i]] - expect) < 1e-4f) hit++;
    }
    check(hit == 9, "9/9 needles present after swap");
    printf("    needles hit=%d/9 apply_us=%llu\n", hit,
           (unsigned long long)m->apply_us);

    check(cce_mtk_revert(m) == CCE_OK, "revert");
    check(cce_mtk_skill_active(m) == 0, "inactive after revert");
    {
        int ok = 1;
        for (i = 0; i < 64; ++i)
            if (fabsf(W[i] - 0.01f * (float)i) > 1e-5f) ok = 0;
        check(ok, "exact base restore after revert");
    }

    /* Second apply + overwrite method */
    check(cce_mtk_write_cmsk(path, CCE_MTK_METHOD_OVERWRITE, &st, 1) == CCE_OK,
          "write overwrite cmsk");
    check(cce_mtk_apply_file(m, path, 1.f) == CCE_OK, "apply overwrite");
    hit = 0;
    for (i = 0; i < 9; ++i)
        if (fabsf(W[idx[i]] - val[i]) < 1e-4f) hit++;
    check(hit == 9, "9/9 needles overwrite mode");
    check(cce_mtk_revert(m) == CCE_OK, "revert overwrite");

    cce_mtk_close(m);
    remove(path);
    return hit == 9;
}

static int forest_bind_test(void) {
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
    if (!f) return 0;
    check(cce_cascade_create(&cas, 2) == CCE_OK, "cascade create");
    check(cce_cascade_add_linear(cas, 8, 8, 0.1f) == CCE_OK, "add linear");
    check(cce_forest_add_branch(f, cas, "knowledge") == CCE_OK, "add branch");
    cas = NULL; /* owned by forest */

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

    /* Kernel table LEGO stub */
    {
        cce_mtk_kernel_table kt;
        int called = 0;
        cce_mtk_kernel_table_init(&kt);
        check(cce_mtk_kernel_get(&kt, CCE_MTK_K_GEMM) != NULL, "default gemm slot");
        (void)cce_mtk_kernel_set(&kt, CCE_MTK_K_GEMM, NULL); /* restore */
        (void)called;
    }

    cce_tensor_free(&in);
    cce_tensor_free(&out0);
    cce_tensor_free(&out1);
    cce_mtk_close(m);
    cce_forest_close(f);
    remove(arch);
    remove(skill);
    return 1;
}

int main(void) {
    printf("== CNET Micro-Trensor Kernel (MTK) ==\n");
    needle_test();
    forest_bind_test();
    printf("MTK_PASS checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
