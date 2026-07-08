/* Trit-kernel micro-benchmark: FP vs int8-ternary vs packed 1.6-bit forward
 * on a Supra-head-shaped linear block (in=256, out=50520), plus the packed
 * word-LM predict loop.
 *
 * Gate inside the bench: the trit path must stay BIT-identical to the int8
 * ternary path (same codes, same per-output sum order) — that is the claim
 * the LUT + tiled/threaded restructure must not break. Timing is reported
 * for all three paths so the "smallest, not yet fastest" gap is measurable.
 *
 * Budgeted; NOT part of make test. Build/run: make trit_bench
 * (that target compiles with AVX + OpenMP like the other CCE-only exes).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "../include/cce/cce_block.h"
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_wordlm.h"

#ifdef _OPENMP
#include <omp.h>
static double now_ms(void) { return omp_get_wtime() * 1000.0; }
#else
static double now_ms(void) { return (double)clock() * 1000.0 / CLOCKS_PER_SEC; }
#endif

static unsigned long long g_lcg = 0x2545F4914F6CDD1DULL;
static float frand(void) {
    g_lcg = g_lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((double)(g_lcg >> 33) / 2147483648.0 - 1.0) * 0.05f;
}

static int fails = 0;
#define CHECK(c, msg) do { if (!(c)) { fails++; printf("  FAIL: %s\n", (msg)); } \
                           else printf("  ok   %s\n", (msg)); } while (0)

int main(void) {
    printf("=== trit kernel bench: FP vs int8 vs packed 1.6-bit ===\n");
#ifdef _OPENMP
    printf("openmp: %d threads\n", omp_get_max_threads());
#else
    printf("openmp: OFF (serial build)\n");
#endif

    /* ---- Supra-head-shaped block ---- */
    const int IN = 256, OUT = 50520, REPS = 50;
    cce_block fp, q8, tr;
    if (cce_block_init_linear(&fp, IN, OUT, 0.0f) != CCE_OK ||
        cce_block_init_linear(&q8, IN, OUT, 0.0f) != CCE_OK ||
        cce_block_init_linear(&tr, IN, OUT, 0.0f) != CCE_OK) {
        printf("FAIL: block init\n"); return 1;
    }
    fp.type = q8.type = tr.type = CCE_BLOCK_LINEAR_HEAD;

    size_t nw = (size_t)IN * OUT;
    for (size_t i = 0; i < nw; i++) fp.weights.data[i] = frand();
    memcpy(q8.weights.data, fp.weights.data, nw * sizeof(float));
    memcpy(tr.weights.data, fp.weights.data, nw * sizeof(float));

    CHECK(cce_block_quantize_ternary(&q8) == CCE_OK, "int8-ternary quantize");
    CHECK(cce_block_quantize_ternary(&tr) == CCE_OK &&
          cce_block_pack_trits(&tr) == CCE_OK, "trit pack (1.6-bit)");

    cce_tensor in_t, out_fp, out_q8, out_tr;
    int ishape[1] = { IN }, oshape[1] = { OUT };
    cce_tensor_alloc(&in_t, ishape, 1);
    cce_tensor_alloc(&out_fp, oshape, 1);
    cce_tensor_alloc(&out_q8, oshape, 1);
    cce_tensor_alloc(&out_tr, oshape, 1);
    for (int i = 0; i < IN; i++) in_t.data[i] = frand();

    CHECK(cce_block_forward(&fp, &in_t, &out_fp) == CCE_OK, "fp forward");
    CHECK(cce_block_forward(&q8, &in_t, &out_q8) == CCE_OK, "int8 forward");
    CHECK(cce_block_forward(&tr, &in_t, &out_tr) == CCE_OK, "trit forward");

    /* THE gate: trit path == int8 ternary path, bit for bit */
    CHECK(memcmp(out_q8.data, out_tr.data, (size_t)OUT * sizeof(float)) == 0,
          "trit forward BIT-identical to int8 ternary forward");

    /* argmax sanity across all three (quantized may differ from FP in value,
       but on this synthetic head they should agree at the top) */
    int am_fp = 0, am_q8 = 0, am_tr = 0;
    for (int o = 1; o < OUT; o++) {
        if (out_fp.data[o] > out_fp.data[am_fp]) am_fp = o;
        if (out_q8.data[o] > out_q8.data[am_q8]) am_q8 = o;
        if (out_tr.data[o] > out_tr.data[am_tr]) am_tr = o;
    }
    printf("  info argmax fp=%d int8=%d trit=%d\n", am_fp, am_q8, am_tr);
    CHECK(am_q8 == am_tr, "int8/trit argmax identical");

    /* timing */
    double t0, t1;
    t0 = now_ms(); for (int r = 0; r < REPS; r++) cce_block_forward(&fp, &in_t, &out_fp); t1 = now_ms();
    double ms_fp = (t1 - t0) / REPS;
    t0 = now_ms(); for (int r = 0; r < REPS; r++) cce_block_forward(&q8, &in_t, &out_q8); t1 = now_ms();
    double ms_q8 = (t1 - t0) / REPS;
    t0 = now_ms(); for (int r = 0; r < REPS; r++) cce_block_forward(&tr, &in_t, &out_tr); t1 = now_ms();
    double ms_tr = (t1 - t0) / REPS;
    printf("  head [%dx%d] ms/forward: FP %.3f | int8 %.3f | trit %.3f  (trit = %.2fx of int8)\n",
           IN, OUT, ms_fp, ms_q8, ms_tr, ms_q8 > 0 ? ms_tr / ms_q8 : 0.0);

    cce_block_free(&fp); cce_block_free(&q8); cce_block_free(&tr);
    cce_tensor_free(&in_t); cce_tensor_free(&out_fp);
    cce_tensor_free(&out_q8); cce_tensor_free(&out_tr);

    /* ---- packed word-LM predict loop ---- */
    printf("\n=== packed word-LM predict ===\n");
    const int V = 5000, D = 128, CTX = 4, HID = 256, TOKS = 2000;
    cce_wordlm* m = cce_wordlm_create(V, D, CTX, HID, 1234u);
    if (!m) { printf("FAIL: wordlm create\n"); return 1; }
    cce_wordlm_set_ternary(m, 1);
    cce_wordlm_set_ternary_embed(m, 1);
    const char* tmp = "trit_bench_wlm.bin";
    CHECK(cce_wordlm_export_trits(m, tmp) == 0, "wordlm trit export");
    cce_wordlm_packed* pk = cce_wordlm_packed_load(tmp);
    CHECK(pk != NULL, "wordlm packed load");
    if (pk) {
        int ctx[8] = { 1, 2, 3, 4 };
        t0 = now_ms();
        for (int t = 0; t < TOKS; t++) {
            int w = cce_wordlm_packed_predict(pk, ctx, NULL);
            ctx[0] = ctx[1]; ctx[1] = ctx[2]; ctx[2] = ctx[3]; ctx[3] = w;
        }
        t1 = now_ms();
        printf("  V=%d d=%d ctx=%d hid=%d: %.1f tok/s (%.3f ms/tok)\n",
               V, D, CTX, HID, TOKS * 1000.0 / (t1 - t0), (t1 - t0) / TOKS);
        cce_wordlm_packed_free(pk);
    }
    remove(tmp);
    cce_wordlm_free(m);

    printf("\ntrit_bench: %s\n", fails ? "FAILED" : "all parity gates passed");
    return fails ? 1 : 0;
}
