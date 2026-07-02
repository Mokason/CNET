/* Round-trip + loss regression gate for cce_model save/load. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_model.h"
#include "../include/cce/cce_dataset.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

/* Build a model with one forest holding one 2-block cascade. Returns model + the
   forest path used (caller removes it). The forest OWNS the cascade after add_branch
   (shallow copy transfers block ownership) -- do NOT free the local cascade. */
static cce_model* build_model(const char* forest_path, int IN, int HID, int OUT) {
    cce_cascade cas; cce_cascade_init(&cas, 4);
    cce_block b0, b1;
    cce_block_init_linear(&b0, IN, HID, 0.01f);
    cce_block_init_linear(&b1, HID, OUT, 0.01f); b1.type = CCE_BLOCK_LINEAR_HEAD;
    cce_cascade_append(&cas, &b0);
    cce_cascade_append(&cas, &b1);

    cce_forest* f = NULL;
    remove(forest_path);
    if (cce_forest_open(&f, forest_path, 4) != CCE_OK) return NULL;
    if (cce_forest_add_branch(f, &cas, "b0") != CCE_OK) return NULL;
    /* forest owns the cascade blocks now; do not free `cas`. */

    cce_model* m = NULL;
    if (cce_model_create(&m, "savetest") != CCE_OK) return NULL;
    if (cce_model_add_forest(m, f, "f0") != CCE_OK) return NULL;
    return m;
}

int main(void) {
    printf("=== cce_model save/load round-trip ===\n");
    const int IN = 8, HID = 6, OUT = 3;
    float x[8] = { 0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f };
    const char* forest_path = "test_model_forest.cce";
    const char* bundle_path = "test_model_bundle.cce";

    cce_model* m = build_model(forest_path, IN, HID, OUT);
    CHECK(m != NULL, "build model");

    int lbl = -99; float conf = -1.0f;
    CHECK(cce_model_infer(m, x, IN, &lbl, &conf) == CCE_OK, "reference infer ok");

    float raw[3] = {0};
    int raw_dim = 0;
    CHECK(cce_model_forward(m, x, IN, raw, OUT, &raw_dim) == CCE_OK, "reference forward ok");
    CHECK(raw_dim == OUT, "reference forward dim == OUT");
    CHECK(raw[0] == raw[0] && raw[1] == raw[1] && raw[2] == raw[2], "reference forward finite");
    float bx[2 * 8];
    memcpy(bx, x, sizeof(x));
    memcpy(bx + 8, x, sizeof(x));
    cce_batch fb = { bx, NULL, 2, IN, OUT, 0 };
    float bout[2 * 3] = {0};
    int bout_dim = 0;
    CHECK(cce_model_forward_batch(m, &fb, bout, OUT, &bout_dim) == CCE_OK, "reference forward_batch ok");
    CHECK(bout_dim == OUT, "reference forward_batch dim == OUT");
    CHECK(fabsf(bout[0] - raw[0]) < 1e-5f &&
          fabsf(bout[1] - raw[1]) < 1e-5f &&
          fabsf(bout[2] - raw[2]) < 1e-5f, "forward_batch first row == forward");

    remove(bundle_path);
    CHECK(cce_model_save(m, bundle_path) == CCE_OK, "model save ok");

    cce_model* m2 = NULL;
    CHECK(cce_model_create(&m2, "loadtest") == CCE_OK, "create dest model");
    CHECK(cce_model_load(m2, bundle_path) == CCE_OK, "model load ok");

    int lbl2 = -99; float conf2 = -1.0f;
    CHECK(cce_model_infer(m2, x, IN, &lbl2, &conf2) == CCE_OK, "loaded infer ok");
    CHECK(lbl2 == lbl, "loaded label == reference label");
    CHECK(fabsf(conf2 - conf) < 1e-5f, "loaded confidence == reference confidence");
    float raw2[3] = {0};
    int raw_dim2 = 0;
    CHECK(cce_model_forward(m2, x, IN, raw2, OUT, &raw_dim2) == CCE_OK, "loaded forward ok");
    CHECK(raw_dim2 == raw_dim, "loaded forward dim == reference dim");
    CHECK(fabsf(raw2[0] - raw[0]) < 1e-5f &&
          fabsf(raw2[1] - raw[1]) < 1e-5f &&
          fabsf(raw2[2] - raw[2]) < 1e-5f, "loaded forward vector == reference vector");

    cce_model_destroy(m);
    cce_model_destroy(m2);   /* must free owned forest/cascade cleanly, no double-free */
    remove(forest_path);
    remove(bundle_path);

    /* --- branch accessors --- */
    {
        const char* fp = "test_accessor_forest.cce";
        cce_cascade c2; cce_cascade_init(&c2, 4);
        cce_block bb0, bb1;
        cce_block_init_linear(&bb0, IN, HID, 0.01f);
        cce_block_init_linear(&bb1, HID, OUT, 0.01f); bb1.type = CCE_BLOCK_LINEAR_HEAD;
        cce_cascade_append(&c2, &bb0); cce_cascade_append(&c2, &bb1);
        cce_forest* f2 = NULL; remove(fp);
        cce_forest_open(&f2, fp, 4);
        cce_forest_add_branch(f2, &c2, "alpha");
        CHECK(cce_forest_branch_count(f2) == 1, "branch_count == 1");
        char nm[64] = {0};
        CHECK(cce_forest_branch_name(f2, 0, nm, sizeof(nm)) == CCE_OK, "branch_name ok");
        CHECK(strcmp(nm, "alpha") == 0, "branch_name == alpha");
        cce_forest_close(f2);
        remove(fp);
    }

    /* --- composition-first branch factory --- */
    {
        const char* fp = "test_factory_forest.cce";
        cce_forest* ff = NULL; remove(fp);
        CHECK(cce_forest_open(&ff, fp, 4) == CCE_OK, "factory forest open");
        int branch_idx = -1;
        CHECK(cce_forest_add_linear_branch(ff, "factory_branch", IN, HID, OUT, 0.01f, &branch_idx) == CCE_OK,
              "factory add linear branch ok");
        CHECK(branch_idx == 0, "factory branch index == 0");
        CHECK(cce_forest_branch_count(ff) == 1, "factory branch_count == 1");
        char nm[64] = {0};
        CHECK(cce_forest_branch_name(ff, 0, nm, sizeof(nm)) == CCE_OK, "factory branch_name ok");
        CHECK(strcmp(nm, "factory_branch") == 0, "factory branch_name == factory_branch");
        int block_types[4] = {0};
        int block_inputs[4] = {0};
        int block_outputs[4] = {0};
        int block_count = 0;
        CHECK(cce_forest_get_branch_blocks(ff, 0, block_types, block_inputs, block_outputs, &block_count, 4) == CCE_OK,
              "factory branch block summary ok");
        CHECK(block_count == 2, "factory branch block count == 2");
        CHECK(block_types[0] == CCE_BLOCK_LINEAR && block_inputs[0] == IN && block_outputs[0] == HID,
              "factory first block summary == linear IN->HID");
        CHECK(block_types[1] == CCE_BLOCK_LINEAR_HEAD && block_inputs[1] == HID && block_outputs[1] == OUT,
              "factory second block summary == head HID->OUT");

        cce_model* fm = NULL;
        CHECK(cce_model_create(&fm, "factorymodel") == CCE_OK, "factory model create");
        CHECK(cce_model_add_forest(fm, ff, "factory_forest") == CCE_OK, "factory model add forest");
        float factory_raw[3] = {0};
        int factory_dim = 0;
        CHECK(cce_model_forward(fm, x, IN, factory_raw, OUT, &factory_dim) == CCE_OK, "factory model forward ok");
        CHECK(factory_dim == OUT, "factory model forward dim == OUT");
        CHECK(factory_raw[0] == factory_raw[0] &&
              factory_raw[1] == factory_raw[1] &&
              factory_raw[2] == factory_raw[2], "factory model forward finite");
        cce_model_destroy(fm);
        cce_forest_close(ff);
        remove(fp);
    }

    /* --- patch branch factory + model persistence --- */
    {
        const char* fp = "test_patch_factory_forest.cce";
        const char* bp = "test_patch_factory_bundle.cce";
        cce_forest* pf = NULL; remove(fp); remove(bp);
        CHECK(cce_forest_open(&pf, fp, 4) == CCE_OK, "patch factory forest open");
        int patch_idx = -1;
        CHECK(cce_forest_add_patch_branch(pf, "patch_branch", 2, 1, 1, 5, OUT, 0.01f, &patch_idx) == CCE_OK,
              "patch factory add branch ok");
        CHECK(patch_idx == 0, "patch branch index == 0");

        cce_model* pm = NULL;
        CHECK(cce_model_create(&pm, "patchmodel") == CCE_OK, "patch model create");
        CHECK(cce_model_add_forest(pm, pf, "patch_forest") == CCE_OK, "patch model add forest");
        float patch_x[4] = {1.0f, 0.25f, -0.5f, 0.75f};
        float patch_raw[3] = {0};
        int patch_dim = 0;
        CHECK(cce_model_forward(pm, patch_x, 4, patch_raw, OUT, &patch_dim) == CCE_OK, "patch model forward ok");
        CHECK(patch_dim == OUT, "patch model forward dim == OUT");
        CHECK(cce_model_save(pm, bp) == CCE_OK, "patch model save ok");

        cce_model* pm2 = NULL;
        CHECK(cce_model_create(&pm2, "patchload") == CCE_OK, "patch load model create");
        CHECK(cce_model_load(pm2, bp) == CCE_OK, "patch model load ok");
        float patch_raw2[3] = {0};
        int patch_dim2 = 0;
        CHECK(cce_model_forward(pm2, patch_x, 4, patch_raw2, OUT, &patch_dim2) == CCE_OK, "patch loaded forward ok");
        CHECK(patch_dim2 == OUT, "patch loaded forward dim == OUT");
        CHECK(fabsf(patch_raw2[0] - patch_raw[0]) < 1e-5f &&
              fabsf(patch_raw2[1] - patch_raw[1]) < 1e-5f &&
              fabsf(patch_raw2[2] - patch_raw[2]) < 1e-5f, "patch loaded forward vector == reference vector");

        cce_model_destroy(pm);
        cce_model_destroy(pm2);
        cce_forest_close(pf);
        remove(fp); remove(bp);
    }

    /* --- compositional cascade builder API --- */
    {
        const char* fp = "test_builder_forest.cce";
        cce_forest* bf = NULL; remove(fp);
        CHECK(cce_forest_open(&bf, fp, 4) == CCE_OK, "builder forest open");

        cce_cascade* built = NULL;
        CHECK(cce_cascade_create(&built, 3) == CCE_OK, "builder cascade create");
        CHECK(cce_cascade_add_patch(built, 2, 1, 1) == CCE_OK, "builder add patch");
        CHECK(cce_cascade_add_linear(built, 4, 5, 0.01f) == CCE_OK, "builder add linear");
        CHECK(cce_cascade_add_linear_head(built, 5, OUT, 0.01f) == CCE_OK, "builder add linear head");

        int built_idx = -1;
        CHECK(cce_forest_add_cascade_branch(bf, built, "built_patch_branch", &built_idx) == CCE_OK,
              "builder add cascade branch ok");
        CHECK(built_idx == 0, "builder branch index == 0");
        cce_cascade_destroy(built);  /* moved branch must make destroying the builder safe */

        cce_model* bm = NULL;
        CHECK(cce_model_create(&bm, "buildermodel") == CCE_OK, "builder model create");
        CHECK(cce_model_add_forest(bm, bf, "builder_forest") == CCE_OK, "builder model add forest");
        float patch_x[4] = {0.2f, -0.1f, 0.7f, 0.4f};
        float builder_raw[3] = {0};
        int builder_dim = 0;
        CHECK(cce_model_forward(bm, patch_x, 4, builder_raw, OUT, &builder_dim) == CCE_OK,
              "builder model forward ok");
        CHECK(builder_dim == OUT, "builder model forward dim == OUT");
        CHECK(builder_raw[0] == builder_raw[0] &&
              builder_raw[1] == builder_raw[1] &&
              builder_raw[2] == builder_raw[2], "builder model forward finite");

        cce_model_destroy(bm);
        cce_forest_close(bf);
        remove(fp);
    }

    /* --- real loss + loss-type wiring --- */
    {
        const char* fp = "test_loss_forest.cce";
        cce_cascade c3; cce_cascade_init(&c3, 4);
        cce_block d0, d1;
        cce_block_init_linear(&d0, IN, HID, 0.05f);
        cce_block_init_linear(&d1, HID, OUT, 0.05f); d1.type = CCE_BLOCK_LINEAR_HEAD;
        cce_cascade_append(&c3, &d0); cce_cascade_append(&c3, &d1);
        cce_forest* f3 = NULL; remove(fp); cce_forest_open(&f3, fp, 4);
        cce_forest_add_branch(f3, &c3, "b");
        cce_model* lm = NULL; cce_model_create(&lm, "lossmodel");
        cce_model_add_forest(lm, f3, "f");

        /* tiny dataset: 4 samples, one-hot targets */
        float xs[4*8] = {0}; float ys[4*3] = {0};
        for (int i = 0; i < 4; i++) { xs[i*8 + (i%8)] = 1.0f; ys[i*3 + (i%3)] = 1.0f; }
        cce_dataset* ds = NULL;
        cce_dataset_from_arrays(&ds, xs, ys, 4, 8, 3, 2);

        cce_model_set_loss(lm, 0);                 /* MSE */
        double mse = cce_model_train(lm, ds, 5, 0.0f);
        CHECK(mse == mse && mse >= 0.0, "MSE loss is finite and >= 0");

        cce_dataset_reset(ds);
        cce_model_set_loss(lm, 1);                 /* cross-entropy */
        double ce = cce_model_train(lm, ds, 5, 0.0f);
        CHECK(ce == ce && ce >= 0.0, "CE loss is finite and >= 0");
        CHECK(fabs(ce - mse) > 1e-9, "CE and MSE produce different loss (paths wired)");

        cce_model_set_learn_params(lm, 0.6f, 0.5f, 1.0f);  /* must not crash */
        cce_dataset_reset(ds);
        double l2 = cce_model_train(lm, ds, 2, 0.0f);
        CHECK(l2 == l2 && l2 >= 0.0, "train with custom knobs is finite");

        cce_dataset_destroy(ds);
        cce_model_destroy(lm);
        remove(fp);
    }

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
