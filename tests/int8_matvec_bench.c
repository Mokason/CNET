/* int8 oracle matvec gate — make int8_matvec_bench → INT8_MATVEC_BENCH_PASS
 *
 * Guards the autovectorized inner loop in cce_block_forward's int8 path
 * (output[o] += a * (float)qrow[o]). That loop carries the oracle head, and a
 * build that loses vectorisation costs ~3x when the call is thread-constrained
 * — a regression no other gate here can see, because cce_train_bench is a
 * float32 cascade running threaded, and past ~8 threads this loop is
 * bandwidth-bound and the ISA stops mattering at all.
 *
 * Two hard gates, both host-independent by construction:
 *
 *   1. Bit-identity. The vectorised result must equal, bit for bit, a
 *      reference computed with vectorisation disabled in this same binary.
 *      This is the "each output keeps its own i-ascending accumulation
 *      regardless of SIMD width" claim in the Makefile, checked rather than
 *      asserted. Comparing against an in-process reference (not a pinned
 *      checksum) keeps it valid on any host and toolchain.
 *
 *   2. Vectorisation ratio. Single-threaded, the real path must beat the
 *      no-vectorize reference by CNET_INT8_MIN_SPEEDUP (default 2.5x). This is
 *      a *relative* measure taken on the same host in the same process, so it
 *      needs no absolute ms/call floor — the thing an ISA regression actually
 *      destroys is the ratio, and a wall-clock floor would only encode the
 *      machine it was written on.
 *
 * Absolute throughput is measured and printed for humans, never gated.
 */
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _OPENMP
#include <omp.h>
#endif

/* Oracle-shaped: cce_block.c cites hidden 3840 and an out_dim>=4096 head.
   This shape is ~15 MB of int8 weights, so it stays compute-bound in cache
   where the ISA is what decides — the DRAM-streaming shape is memory-bound
   and would measure the memory controller instead. */
#define IN_DIM  3840
#define OUT_DIM 4096
#define ITERS   60

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* Reference with vectorisation off. It mirrors the real kernel's loop nest
   exactly -- same 512-output tiling, same i-outer/o-inner unit-stride walk,
   same i-ascending accumulation per output -- so the ONLY difference is the
   disabled vectoriser. An o-outer reference would instead stride by OUT_DIM
   and measure cache misses, making the ratio meaninglessly large and the gate
   unable to tell a good build from a bad one. */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("no-tree-vectorize")))
#endif
static void reference_matvec(const cce_block* blk, const float* x, float* out) {
    for (int ob = 0; ob < OUT_DIM; ob += 512) {
        int oe = (ob + 512 < OUT_DIM) ? ob + 512 : OUT_DIM;
        for (int o = ob; o < oe; ++o) out[o] = 0.0f;
        for (int i = 0; i < IN_DIM; ++i) {
            const float a = x[i];
            const signed char* qrow = &blk->w_q[(size_t)i * OUT_DIM];
            for (int o = ob; o < oe; ++o) out[o] += a * (float)qrow[o];
        }
        for (int o = ob; o < oe; ++o)
            out[o] = blk->bias.data[o] + blk->w_scale[o] * out[o];
    }
}

int main(void) {
    int failures = 0;
    printf("== int8 oracle matvec gate (%dx%d) ==\n", IN_DIM, OUT_DIM);

    cce_block blk;
    if (cce_block_init_linear(&blk, IN_DIM, OUT_DIM, 0.01f) != CCE_OK) {
        printf("FAIL: block init\n"); return 1;
    }
    /* LINEAR_HEAD: raw logits, so the gate covers the matvec itself and does
       not fold in libm's expf, whose last bit is not ours to promise. */
    blk.type = CCE_BLOCK_LINEAR_HEAD;

    for (size_t i = 0; i < blk.weights.numel; ++i)
        blk.weights.data[i] = (float)(((i * 2654435761u) >> 8) % 2001 - 1000) / 1000.0f;
    for (int o = 0; o < OUT_DIM; ++o) blk.bias.data[o] = 0.01f;

    if (cce_block_quantize_int8(&blk) != CCE_OK) {
        printf("FAIL: quantize_int8\n"); cce_block_free(&blk); return 1;
    }

    cce_tensor x, y;
    int xs[1] = {IN_DIM}, ys[1] = {OUT_DIM};
    cce_tensor_alloc(&x, xs, 1);
    cce_tensor_alloc(&y, ys, 1);
    for (int i = 0; i < IN_DIM; ++i)
        x.data[i] = (float)(((i * 40503u) >> 4) % 200 - 100) / 100.0f;

    float* ref = (float*)malloc(sizeof(float) * OUT_DIM);
    if (!ref) { printf("FAIL: alloc\n"); return 1; }

    /* --- Gate 1: bit-identity against the unvectorised reference --- */
    cce_block_forward(&blk, &x, &y);
    reference_matvec(&blk, x.data, ref);
    int mismatches = 0;
    for (int o = 0; o < OUT_DIM; ++o) {
        unsigned int a, b;
        memcpy(&a, &y.data[o], 4);
        memcpy(&b, &ref[o], 4);
        if (a != b && ++mismatches <= 3)
            printf("  bit mismatch at %d: got %.9g want %.9g\n", o, y.data[o], ref[o]);
    }
    if (mismatches) {
        printf("FAIL: vectorised result differs from scalar reference in %d/%d outputs\n",
               mismatches, OUT_DIM);
        failures++;
    } else {
        printf("  bit-identical to no-vectorize reference        %d/%d outputs\n",
               OUT_DIM, OUT_DIM);
    }

    /* --- Gate 2: single-threaded vectorisation ratio --- */
#ifdef _OPENMP
    int saved = omp_get_max_threads();
    omp_set_num_threads(1);
#endif
    /* Best-of-N on both sides. A shared CI runner can lose a slice to another
       tenant mid-measurement; the minimum is the run that was not interrupted,
       and taking it on both sides keeps the ratio meaningful. */
    double vec_ms = 1e30, ref_ms = 1e30;
    for (int w = 0; w < 3; ++w) cce_block_forward(&blk, &x, &y);
    for (int rep = 0; rep < 3; ++rep) {
        double t0 = now_ms();
        for (int it = 0; it < ITERS; ++it) cce_block_forward(&blk, &x, &y);
        double ms = (now_ms() - t0) / ITERS;
        if (ms < vec_ms) vec_ms = ms;
    }
    reference_matvec(&blk, x.data, ref);          /* warm */
    for (int rep = 0; rep < 3; ++rep) {
        double t0 = now_ms();
        for (int it = 0; it < 4; ++it) reference_matvec(&blk, x.data, ref);
        double ms = (now_ms() - t0) / 4;
        if (ms < ref_ms) ref_ms = ms;
    }
#ifdef _OPENMP
    omp_set_num_threads(saved);
#endif

    double speedup = ref_ms / vec_ms;
    double bytes = (double)IN_DIM * OUT_DIM;

    /* The floor tracks the ISA this binary was actually compiled for, so the
       gate asks "did you get the width your flags promised?" rather than one
       blanket number. A single floor cannot work here: CI builds PORTABLE=1
       while the shipped artefacts are v3, so any floor strict enough to catch
       an ISA downgrade would fail the portable lane. Measured on Zen 5 over
       repeated runs: baseline 4.55-4.78x, AVX2 5.14-6.10x, AVX-512 7.76-9.01x.
       Floors sit ~1.3x below each observed minimum. Raise or relax with
       CNET_INT8_MIN_SPEEDUP if a host's memory subsystem shifts the ratios. */
#if defined(__AVX512F__)
    const char* isa = "AVX-512"; double floor_x = 6.0;
#elif defined(__AVX2__)
    const char* isa = "AVX2";    double floor_x = 4.0;
#else
    const char* isa = "baseline"; double floor_x = 2.5;
#endif
    const char* env = getenv("CNET_INT8_MIN_SPEEDUP");
    if (env && *env) floor_x = atof(env);

    printf("  1 thread: vectorised %.3f ms | scalar ref %.3f ms | speedup %.2fx"
           " (%s floor %.2fx)\n", vec_ms, ref_ms, speedup, isa, floor_x);
    if (speedup < floor_x) {
        printf("FAIL: int8 matvec is slower than its %s build should be"
               " (%.2fx < %.2fx). Either vectorisation was lost or ARCH_CFLAGS"
               " no longer match the compiled ISA.\n", isa, speedup, floor_x);
        failures++;
    }

    /* --- Reported, never gated: absolute throughput is a property of the
           host's memory subsystem, not of this commit. --- */
    for (int w = 0; w < 3; ++w) cce_block_forward(&blk, &x, &y);
    double t_all = now_ms();
    for (int it = 0; it < ITERS; ++it) cce_block_forward(&blk, &x, &y);
    double all_ms = (now_ms() - t_all) / ITERS;
    printf("  all threads: %.3f ms/call | %.2f GB/s int8 weights (reported, not gated)\n",
           all_ms, bytes / (all_ms / 1000.0) / 1e9);

    free(ref);
    cce_tensor_free(&x);
    cce_tensor_free(&y);
    cce_block_free(&blk);

    if (failures) { printf("%d FAILURES\n", failures); return 1; }
    printf("INT8_MATVEC_BENCH_PASS\n");
    return 0;
}
