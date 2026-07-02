/* cce_cascade_view_from_archive: zero-copy WARM load.
   Verifies a viewed cascade (weights point into the mmap) produces bit-identical
   forward output to the copying load, is actually zero-copy (owns_memory==0), and
   frees cleanly (no double-free / no touching the mmap). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_archive.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

static void forward3(const cce_cascade* cas, const float* x, int in_dim, float* out, int out_dim) {
    cce_tensor tx; int xs[1] = { in_dim };
    cce_tensor_alloc(&tx, xs, 1);
    memcpy(tx.data, x, (size_t)in_dim * sizeof(float));
    cce_tensor ty; memset(&ty, 0, sizeof(ty));
    cce_cascade_forward(cas, &tx, &ty);
    for (int i = 0; i < out_dim && (size_t)i < ty.numel; i++) out[i] = ty.data[i];
    cce_tensor_free(&ty);
    cce_tensor_free(&tx);
}

int main(void) {
    srand(123);
    printf("=== cce_cascade_view_from_archive (zero-copy WARM) ===\n");

    const int IN = 8, HID = 6, OUT = 3;
    float x[8] = { 0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f };

    /* build a 2-block cascade (linear hidden + linear head) */
    cce_cascade cas; cce_cascade_init(&cas, 4);
    cce_block b0, b1;
    cce_block_init_linear(&b0, IN, HID, 0.01f);
    cce_block_init_linear(&b1, HID, OUT, 0.01f); b1.type = CCE_BLOCK_LINEAR_HEAD;
    cce_cascade_append(&cas, &b0);
    cce_cascade_append(&cas, &b1);

    float o_ref[3]; forward3(&cas, x, IN, o_ref, OUT);
    printf("reference out: [% .6f % .6f % .6f]\n", o_ref[0], o_ref[1], o_ref[2]);

    /* persist to a fresh archive (kept open so offset-0 data is intact) */
    const char* path = "cce_view_test.cce";
    remove(path);
    cce_archive* ar = NULL;
    CHECK(cce_archive_open(&ar, path) == CCE_OK && ar != NULL, "archive open");
    size_t off = 0;
    CHECK(cce_cascade_save_to_archive(&cas, ar, "cas", &off) == CCE_OK, "save to archive");

    /* (1) copying load */
    cce_cascade copy; memset(&copy, 0, sizeof(copy));
    CHECK(cce_cascade_load_from_archive(&copy, ar, off) == CCE_OK, "load (copy)");
    float o_copy[3]; forward3(&copy, x, IN, o_copy, OUT);
    int copy_ok = 1;
    for (int i = 0; i < OUT; i++) if (fabsf(o_copy[i] - o_ref[i]) > 1e-6f) copy_ok = 0;
    CHECK(copy_ok, "copy-load forward == reference");

    /* (2) zero-copy view load */
    cce_cascade view; memset(&view, 0, sizeof(view));
    CHECK(cce_cascade_view_from_archive(&view, ar, off) == CCE_OK, "view from archive");
    CHECK(view.num_blocks == 2, "view has 2 blocks");
    float o_view[3]; forward3(&view, x, IN, o_view, OUT);
    int view_ok = 1;
    for (int i = 0; i < OUT; i++) if (fabsf(o_view[i] - o_ref[i]) > 1e-6f) view_ok = 0;
    CHECK(view_ok, "view-load forward == reference (bit-for-bit weights)");
    printf("view out:      [% .6f % .6f % .6f]\n", o_view[0], o_view[1], o_view[2]);

    /* (3) confirm it is actually a zero-copy view (mmap-backed) */
    int n_view = 0, n_own = 0;
    for (int i = 0; i < view.num_blocks; i++) {
        if (view.blocks[i].weights.owns_memory == 0) n_view++; else n_own++;
    }
    printf("view tiers: %d zero-copy weight tensors, %d copy-fallback\n", n_view, n_own);
    CHECK(n_view >= 1, "at least one weight tensor is a zero-copy view (owns_memory==0)");

    /* (4) free everything: views must NOT free the mmap; copies must free cleanly */
    cce_cascade_free(&view);   /* frees nothing mmap-backed */
    cce_cascade_free(&copy);   /* frees owned copies */
    cce_cascade_free(&cas);
    cce_archive_close(ar);     /* now the mmap goes away -- after the views are gone */
    remove(path);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
