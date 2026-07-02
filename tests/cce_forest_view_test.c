/* Forest tier wiring: zero-copy WARM views + copy-on-write to HOT + seal guard.
   Verifies cce_forest_forward materializes a contracted branch as a zero-copy view
   (reusing the mmap pointer), that forward matches the original, that
   promote_to_hot upgrades a view to an owned copy, and that sealing forbids
   further appends. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"

static int checks = 0, fails = 0;
#define CHECK(c, m) do { checks++; if (!(c)) { fails++; printf("  FAIL: %s\n", m); } } while (0)

static void build_cas(cce_cascade* cas, int in, int hid, int out, unsigned seed) {
    srand(seed);
    cce_cascade_init(cas, 4);
    cce_block b0, b1;
    cce_block_init_linear(&b0, in, hid, 0.01f);
    cce_block_init_linear(&b1, hid, out, 0.01f); b1.type = CCE_BLOCK_LINEAR_HEAD;
    cce_cascade_append(cas, &b0);
    cce_cascade_append(cas, &b1);
}

static void fwd_cas(const cce_cascade* c, const float* x, int in, float* out, int outd) {
    cce_tensor tx; int xs[1] = { in }; cce_tensor_alloc(&tx, xs, 1);
    memcpy(tx.data, x, (size_t)in * sizeof(float));
    cce_tensor ty; memset(&ty, 0, sizeof(ty));
    cce_cascade_forward(c, &tx, &ty);
    for (int i = 0; i < outd && (size_t)i < ty.numel; i++) out[i] = ty.data[i];
    cce_tensor_free(&ty); cce_tensor_free(&tx);
}

static int near3(const float* a, const float* b) {
    for (int i = 0; i < 3; i++) if (fabsf(a[i] - b[i]) > 1e-6f) return 0;
    return 1;
}

int main(void) {
    printf("=== cce_forest: zero-copy WARM views + copy-on-write + seal ===\n");
    const int IN = 8, HID = 6, OUT = 3;
    float x[8] = { 0.2f, -0.1f, 0.4f, -0.3f, 0.6f, -0.5f, 0.8f, -0.7f };

    /* two branch cascades; record reference outputs before handing them off */
    cce_cascade c0, c1;
    build_cas(&c0, IN, HID, OUT, 11u);
    build_cas(&c1, IN, HID, OUT, 22u);
    float r0[3], r1[3];
    fwd_cas(&c0, x, IN, r0, OUT);
    fwd_cas(&c1, x, IN, r1, OUT);

    const char* path = "forest_view_test.cce";
    remove(path);
    cce_forest* fr = NULL;
    CHECK(cce_forest_open(&fr, path, 8) == CCE_OK && fr, "forest open");
    CHECK(cce_forest_add_branch(fr, &c0, "a") == CCE_OK, "add branch a");
    CHECK(cce_forest_add_branch(fr, &c1, "b") == CCE_OK, "add branch b");
    /* forest now owns c0/c1 block chains (shallow copy) -- do not free locals */

    CHECK(cce_forest_seal(fr) == CCE_OK, "seal");

    /* seal forbids further appends (would remap mmap -> invalidate views) */
    cce_cascade c2; build_cas(&c2, IN, HID, OUT, 33u);
    CHECK(cce_forest_add_branch(fr, &c2, "c") != CCE_OK, "add after seal is rejected");
    cce_cascade_free(&c2);  /* not consumed by the forest */

    /* contract both to WARM (frees RAM; archive copy remains; no re-append) */
    CHECK(cce_forest_contract_branch(fr, 0) == CCE_OK, "contract a");
    CHECK(cce_forest_contract_branch(fr, 1) == CCE_OK, "contract b");
    CHECK(fr->branches[0].cascade == NULL, "branch a not resident after contract");

    /* forward via the forest -> should materialize a ZERO-COPY view */
    cce_tensor tx; int xs[1] = { IN }; cce_tensor_alloc(&tx, xs, 1);
    memcpy(tx.data, x, (size_t)IN * sizeof(float));

    cce_tensor o0; memset(&o0, 0, sizeof(o0));
    CHECK(cce_forest_forward(fr, 0, &tx, &o0) == CCE_OK, "forest_forward a");
    float g0[3] = {0}; for (int i = 0; i < OUT; i++) g0[i] = o0.data[i];
    CHECK(near3(g0, r0), "branch a view forward == reference");
    cce_tensor_free(&o0);

    CHECK(fr->branches[0].is_view == 1, "branch a materialized as a view");
    CHECK(fr->branches[0].cascade && fr->branches[0].cascade->blocks[0].weights.owns_memory == 0,
          "branch a weights are a zero-copy view (owns_memory==0)");

    cce_tensor o1; memset(&o1, 0, sizeof(o1));
    CHECK(cce_forest_forward(fr, 1, &tx, &o1) == CCE_OK, "forest_forward b");
    float g1[3] = {0}; for (int i = 0; i < OUT; i++) g1[i] = o1.data[i];
    CHECK(near3(g1, r1), "branch b view forward == reference");
    cce_tensor_free(&o1);

    /* recall the same branch again -> pointer reuse (still the same view) */
    cce_cascade* before = fr->branches[0].cascade;
    cce_tensor o0b; memset(&o0b, 0, sizeof(o0b));
    cce_forest_forward(fr, 0, &tx, &o0b);
    cce_tensor_free(&o0b);
    CHECK(fr->branches[0].cascade == before, "repeat recall reuses the same cascade pointer (no reload)");

    /* copy-on-write: promote a view to an owned HOT cascade */
    CHECK(cce_forest_promote_to_hot(fr, 0) == CCE_OK, "promote a to hot");
    CHECK(fr->branches[0].is_view == 0, "branch a is owned after promote");
    CHECK(fr->branches[0].cascade && fr->branches[0].cascade->blocks[0].weights.owns_memory == 1,
          "branch a weights are owned after copy-on-write");
    cce_tensor o0c; memset(&o0c, 0, sizeof(o0c));
    cce_forest_forward(fr, 0, &tx, &o0c);
    float g0c[3] = {0}; for (int i = 0; i < OUT; i++) g0c[i] = o0c.data[i];
    CHECK(near3(g0c, r0), "branch a owned forward still == reference");
    cce_tensor_free(&o0c);

    cce_tensor_free(&tx);
    cce_forest_close(fr);   /* frees views + owned copies; then unmaps */
    remove(path);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
