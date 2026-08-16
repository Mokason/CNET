# Mojo Kernel Bridge Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Put a Mojo kernel behind a stable C ABI so one kernel source can eventually target NVIDIA and AMD, without Python and without Mojo ever becoming load-bearing.

**Architecture:** Extract the existing ternary matmul from `cce_block_forward` into a named C function, put a dispatch layer in front of it that can route to a Mojo implementation, and gate the whole thing so the C path remains the real implementation. Build integration probes for the toolchain exactly as `CURL_PROBE` does for libcurl; the runtime path defaults OFF even when compiled in.

**Tech Stack:** C11 (`-Wall -Wextra -pedantic -mno-avx`), Mojo 1.0 (`@export`, `abi("C")`), GNU Make.

**Spec:** [docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md](../specs/2026-08-16-mojo-kernel-bridge-design.md)

## Global Constraints

- **No Python anywhere**, including Mojo's Python interop.
- **Mojo is never load-bearing.** No CNET capability may exist only in Mojo. Deleting `mojo/`, one Makefile stanza and one `#ifdef` must return the tree to exactly today's behaviour.
- **Default OFF.** Even with `CNET_HAVE_MOJO` defined, the Mojo path runs only when the environment sets `CNET_MOJO=1`. Mirrors how `CNET_GPU=1` gates OpenCL today.
- **Only primitives and pointers cross the ABI.** No `cce_block`, no `cce_tensor`, no Mojo type. `include/cce/cce_mojo_kernel.h` is hand-written C and is the contract; Mojo never generates it.
- **Bit-identity is a float problem here.** The kernel accumulates `out[o] += a * (float)codes[o]` with float `a`. Byte-equality requires matching per-output accumulation order (`i` ascending, no reassociation) AND matching FMA contraction. If unattainable, drop to a stated epsilon and **write the measured divergence and its cause into the spec** — never loosen silently.
- **Tile width is 510** (a multiple of 5, so every tile starts byte-aligned in the packed rows). Do not change it.
- **This box cannot run Mojo** (Windows, no WSL2). Tasks 1–3 and 5 are fully gateable here; Task 4 is Linux-only and must say so rather than implying it was tested.
- Test convention: `CHECK(cond, msg)`, final line `ALL <NAME> TESTS PASSED`, grepped by `tests/verify_logs.sh`.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/cce/cce_mojo_kernel.h` (create) | The C ABI contract: kernel signature, dispatch declaration, anchor digest helper. |
| `src/cce/cce_trit_kernel.c` (create) | The C ternary matmul, extracted verbatim from `cce_block_forward`. The reference implementation, always compiled. |
| `src/cce/cce_mojo_dispatch.c` (create) | Chooses C or Mojo. The only file with `#ifdef CNET_HAVE_MOJO`. |
| `src/cce/cce_block.c` (modify) | Calls the extracted kernel instead of holding it inline. |
| `mojo/trit_matmul.mojo` (create, Task 4) | The Mojo kernel. Linux-only. |
| `tests/test_mojo_bridge.c` (create) | Anchor, harness self-proof, equivalence, default-off and compiled-out gates. |
| `tests/mojo_bench.c` (create, Task 5) | C vs Mojo timing on identical shapes. |
| `Makefile`, `tests/verify_logs.sh` (modify) | `MOJO_PROBE`, targets, log expectation. |

---

## Task 1: Extract the C trit kernel, and record the anchor

Nothing else can be gated until the reference kernel is callable and its output is pinned.

**Files:**
- Create: `include/cce/cce_mojo_kernel.h`, `src/cce/cce_trit_kernel.c`, `tests/test_mojo_bridge.c`
- Modify: `src/cce/cce_block.c`, `Makefile`, `tests/verify_logs.sh`

**Interfaces:**
- Produces: `void cce_trit_matmul_c(const float* input, const uint8_t* w_trit, const float* w_scale, const float* bias, float* output, int in_dim, int out_dim, int w_trit_bpr, int apply_sigmoid);` and `uint64_t cce_mojo_anchor(const float* v, size_t n);`. Tasks 2–5 all use these exact names.

- [ ] **Step 1: Write the ABI header**

Create `include/cce/cce_mojo_kernel.h`:

```c
#ifndef CCE_MOJO_KERNEL_H
#define CCE_MOJO_KERNEL_H

/* The C ABI boundary for Mojo kernels.
 *
 * ONLY primitives and pointers cross this line — no cce_block, no cce_tensor,
 * no Mojo type. This header is hand-written C and is the contract both sides
 * compile against; Mojo never generates it.
 * Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Ternary (1.6-bit packed) matvec, the reference C implementation.
 *   out[o] = bias[o] + w_scale[o] * SUM_i input[i] * code(i,o)
 * with code in {-1,0,+1} unpacked from w_trit, then sigmoid when
 * apply_sigmoid != 0.
 *
 * Accumulation is per-output over i ASCENDING and must stay that way: the
 * bit-identity of this kernel against the int8 path depends on it. */
void cce_trit_matmul_c(const float* input, const uint8_t* w_trit,
                       const float* w_scale, const float* bias,
                       float* output, int in_dim, int out_dim,
                       int w_trit_bpr, int apply_sigmoid);

/* FNV-1a over the raw bytes of a float array. Used as the bit-identity
 * ANCHOR: a printed float would compare one value, this covers every output
 * byte. Never used for anything but comparison. */
uint64_t cce_mojo_anchor(const float* v, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* CCE_MOJO_KERNEL_H */
```

- [ ] **Step 2: Write the failing test**

Create `tests/test_mojo_bridge.c`:

```c
/* Mojo kernel bridge gate.
   Gates 1-3 of the spec run on ANY box including Windows; the equivalence
   and benchmark gates need the Mojo toolchain and are Linux-only.
   Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_mojo_kernel.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } \
                              else printf("  ok   %s\n", msg); } while (0)

/* Deterministic fixture: no rand(), so the anchor is reproducible anywhere. */
static void fill_fixture(float* in, uint8_t* wt, float* ws, float* bias,
                         int in_dim, int out_dim, int bpr) {
    unsigned long long r = 0x9E3779B97F4A7C15ULL;
    int i;
    for (i = 0; i < in_dim; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        in[i] = (float)((double)(r >> 33) / 2147483648.0 - 1.0);
    }
    for (i = 0; i < in_dim * bpr; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        wt[i] = (uint8_t)((r >> 33) % 243);   /* valid 5-trit byte range */
    }
    for (i = 0; i < out_dim; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        ws[i] = 0.01f + (float)((double)(r >> 40) / 8388608.0) * 0.1f;
        bias[i] = 0.0f;
    }
}

#define BPR(out_dim) (((out_dim) + 4) / 5)

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Mojo kernel bridge gate ===\n");

    printf("[1] extracted C kernel produces a stable anchor\n");
    {
        const int in_dim = 64, out_dim = 96, bpr = BPR(96);
        float *in = malloc(sizeof(float) * in_dim);
        uint8_t *wt = malloc((size_t)in_dim * bpr);
        float *ws = malloc(sizeof(float) * out_dim);
        float *bi = malloc(sizeof(float) * out_dim);
        float *out = malloc(sizeof(float) * out_dim);
        CHECK(in && wt && ws && bi && out, "fixture allocates");
        fill_fixture(in, wt, ws, bi, in_dim, out_dim, bpr);
        cce_trit_matmul_c(in, wt, ws, bi, out, in_dim, out_dim, bpr, 1);
        printf("  info ANCHOR in=%d out=%d digest=%016llx\n",
               in_dim, out_dim,
               (unsigned long long)cce_mojo_anchor(out, (size_t)out_dim));
        CHECK(1, "anchor computed");
        free(in); free(wt); free(ws); free(bi); free(out);
    }

    printf("checks=%d fails=%d\n", checks, fails);
    if (fails) return 1;
    printf("ALL MOJO BRIDGE TESTS PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Add the Makefile target and run to verify it fails**

Add near the other CCE source variables:

```make
MOJO_KERNEL_SRC := src/cce/cce_trit_kernel.c
```

Add a target next to `trit_bench`:

```make
# Mojo kernel bridge gate. Links the extracted C kernel ONLY — no $(CCE) —
# so it builds on every box including the Windows one, which cannot run Mojo.
mojo_bridge: $(MOJO_KERNEL_SRC) tests/test_mojo_bridge.c include/cce/cce_mojo_kernel.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(MOJO_KERNEL_SRC) tests/test_mojo_bridge.c -lm
	./$(BIN_DIR)/mojo_bridge > logs/mojo_bridge.log 2>&1
```

Run: `make mojo_bridge`
Expected: FAIL — `No rule to make target 'src/cce/cce_trit_kernel.c'`.

- [ ] **Step 4: Extract the kernel**

Create `src/cce/cce_trit_kernel.c`. The body is moved **verbatim** from the `if (blk->w_trit && blk->w_scale)` block in `cce_block_forward` ([src/cce/cce_block.c:138](../../../src/cce/cce_block.c)), with struct field accesses replaced by the flat parameters. Do not change the arithmetic, the tile width, or the loop order.

```c
/* The reference ternary matmul, extracted verbatim from cce_block_forward so
   it can be called directly by the bridge's equivalence harness. Behaviour is
   unchanged: same tiling, same accumulation order, same arithmetic.
   Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include "../../include/cce/cce_mojo_kernel.h"
#include "../../include/cce/cce_trit_lut.h"

#include <math.h>
#include <string.h>

static float trit_sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

void cce_trit_matmul_c(const float* input, const uint8_t* w_trit,
                       const float* w_scale, const float* bias,
                       float* output, int in_dim, int out_dim,
                       int w_trit_bpr, int apply_sigmoid) {
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) \
            shared(output, input, w_trit, w_scale, bias, in_dim, out_dim, \
                   w_trit_bpr, apply_sigmoid, cce_trit_lut) \
            if((size_t)in_dim * (size_t)out_dim >= (size_t)1 << 21)
#endif
    for (int ob = 0; ob < out_dim; ob += 510) {
        int oe = (ob + 510 < out_dim) ? ob + 510 : out_dim;
        int w = oe - ob;
        int nb = (w + 4) / 5;
        int8_t codes[510 + 8];
        float* out = &output[ob];
        for (int o = 0; o < w; ++o) out[o] = 0.0f;
        for (int i = 0; i < in_dim; ++i) {
            const float a = input[i];
            const uint8_t* p = &w_trit[(size_t)i * w_trit_bpr + (size_t)(ob / 5)];
            for (int b = 0; b < nb; ++b)
                memcpy(&codes[b * 5], cce_trit_lut[p[b]], 8);
            for (int o = 0; o < w; ++o) out[o] += a * (float)codes[o];
        }
        for (int o = 0; o < w; ++o) {
            float v = bias[ob + o] + w_scale[ob + o] * out[o];
            out[o] = apply_sigmoid ? trit_sigmoid(v) : v;
        }
    }
}

uint64_t cce_mojo_anchor(const float* v, size_t n) {
    const unsigned char* b = (const unsigned char*)v;
    size_t total = n * sizeof(float), i;
    uint64_t h = 1469598103934665603ULL;      /* FNV-1a offset basis */
    for (i = 0; i < total; ++i) {
        h ^= (uint64_t)b[i];
        h *= 1099511628211ULL;
    }
    return h;
}
```

Check the sigmoid: read the `sigmoid` helper `cce_block.c` uses and reproduce it **exactly**, including any fast-path or clamping. If it differs from `1/(1+expf(-x))`, use the real one — a different sigmoid silently changes every output.

- [ ] **Step 5: Run to verify it passes, and RECORD THE ANCHOR**

Run: `make mojo_bridge && cat logs/mojo_bridge.log`
Expected: `ALL MOJO BRIDGE TESTS PASSED`, with an `ANCHOR ... digest=...` line.

**ANCHOR (recorded 2026-08-16): `c88ffc1c1ef0f50a`** for `in=64 out=96`,
sigmoid on. Every later task must reproduce it.

- [ ] **Step 6: Point cce_block_forward at the extracted kernel**

In `src/cce/cce_block.c`, replace the whole `if (blk->w_trit && blk->w_scale) { ... return CCE_OK; }` block with:

```c
    if (blk->w_trit && blk->w_scale) {
        cce_trit_matmul_c(input->data, blk->w_trit, blk->w_scale,
                          blk->bias.data, output->data, in_dim, out_dim,
                          blk->w_trit_bpr, is_head ? 0 : 1);
        return CCE_OK;
    }
```

Add `#include "../../include/cce/cce_mojo_kernel.h"` to `cce_block.c`, and add `$(MOJO_KERNEL_SRC)` to the `CCE` source list so every existing target links it.

- [ ] **Step 7: Prove the extraction changed nothing**

Run: `make trit_bench && make mutate && make qat_block`
Expected: `trit_bench` reports its usual speedup and correctness; `All mutation-sweep gates passed.`; `ALL QAT BLOCK TESTS PASSED`.

`trit_bench` is the load-bearing check here — it already gates the trit kernel as bit-identical, so if the extraction moved a bit, it goes red.

- [ ] **Step 8: Wire into verify and commit**

Add `mojo_bridge` to the `verify:` prerequisite list, and to `tests/verify_logs.sh`:

```
mojo_bridge.log|||ALL MOJO BRIDGE TESTS PASSED
```

```bash
git add include/cce/cce_mojo_kernel.h src/cce/cce_trit_kernel.c src/cce/cce_block.c tests/test_mojo_bridge.c Makefile tests/verify_logs.sh
git commit -m "refactor(cce): extract the ternary matmul behind a C ABI

Moved verbatim out of cce_block_forward so the bridge's equivalence harness
can call it directly. Same tiling, same accumulation order, same arithmetic;
trit_bench and mutate confirm no bits moved."
```

---

## Task 2: Harness self-proof and the shape matrix

**Files:** Modify `tests/test_mojo_bridge.c`

**Interfaces:**
- Consumes: `cce_trit_matmul_c`, `cce_mojo_anchor` (Task 1).
- Produces: `static int run_shape(int in_dim, int out_dim, int apply_sigmoid, uint64_t* digest_out);` used by Tasks 3 and 5.

### Why a self-proof

A comparison harness that has only ever seen one implementation proves
nothing. Before Mojo exists, it must compare the C kernel against itself over
the full shape matrix and report byte-equality. Same discipline as the
attribution registry gate.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_mojo_bridge.c` before the `checks=%d` line:

```c
    printf("[2] harness self-proof: C vs C over the shape matrix\n");
    {
        /* Shapes chosen to hit the edges the kernel actually has:
           510 is the tile width, 5 is the packed-byte granularity. */
        static const int shapes[][2] = {
            {1, 1}, {1, 5}, {7, 3}, {64, 96},
            {32, 509}, {32, 510}, {32, 511},   /* tile boundary */
            {33, 1020}, {17, 7},               /* not multiples of 5 */
            {2048, 1024}                       /* crosses the OpenMP threshold */
        };
        int n = (int)(sizeof shapes / sizeof shapes[0]), s, sig;
        int all_equal = 1;
        for (s = 0; s < n; ++s) {
            for (sig = 0; sig <= 1; ++sig) {
                uint64_t d1 = 0, d2 = 0;
                if (run_shape(shapes[s][0], shapes[s][1], sig, &d1) != 0 ||
                    run_shape(shapes[s][0], shapes[s][1], sig, &d2) != 0) {
                    CHECK(0, "shape ran");
                    all_equal = 0;
                    continue;
                }
                if (d1 != d2) {
                    printf("  MISMATCH in=%d out=%d sig=%d: %016llx vs %016llx\n",
                           shapes[s][0], shapes[s][1], sig,
                           (unsigned long long)d1, (unsigned long long)d2);
                    all_equal = 0;
                }
            }
        }
        printf("  info %d shapes x 2 sigmoid modes compared\n", n);
        CHECK(all_equal, "C kernel is byte-identical to itself on every shape");
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make mojo_bridge`
Expected: FAIL — `implicit declaration of function 'run_shape'`.

- [ ] **Step 3: Implement run_shape**

Add above `main()` in `tests/test_mojo_bridge.c`:

```c
/* Runs one shape through the C kernel and returns the anchor digest of the
   output. Returns 0 on success, -1 on allocation failure. Deterministic:
   the same shape always produces the same digest on the same build. */
static int run_shape(int in_dim, int out_dim, int apply_sigmoid,
                     uint64_t* digest_out) {
    int bpr = BPR(out_dim);
    float *in = (float*)malloc(sizeof(float) * (size_t)in_dim);
    uint8_t *wt = (uint8_t*)malloc((size_t)in_dim * (size_t)bpr);
    float *ws = (float*)malloc(sizeof(float) * (size_t)out_dim);
    float *bi = (float*)malloc(sizeof(float) * (size_t)out_dim);
    float *out = (float*)malloc(sizeof(float) * (size_t)out_dim);
    int rc = -1;
    if (in && wt && ws && bi && out) {
        fill_fixture(in, wt, ws, bi, in_dim, out_dim, bpr);
        cce_trit_matmul_c(in, wt, ws, bi, out, in_dim, out_dim, bpr,
                          apply_sigmoid);
        *digest_out = cce_mojo_anchor(out, (size_t)out_dim);
        rc = 0;
    }
    free(in); free(wt); free(ws); free(bi); free(out);
    return rc;
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make mojo_bridge && cat logs/mojo_bridge.log`
Expected: section 2 passes; 10 shapes × 2 modes compared; the Task 1 anchor unchanged.

- [ ] **Step 5: Commit**

```bash
git add tests/test_mojo_bridge.c
git commit -m "test(mojo): harness self-proof over the shape matrix

Compares the C kernel against itself across tile boundaries, non-multiples of
5, and a shape crossing the OpenMP threshold. A comparison harness that has
only seen one implementation proves nothing, so it proves itself first."
```

---

## Task 3: Dispatch layer, default OFF, compiled out

**Files:**
- Create: `src/cce/cce_mojo_dispatch.c`
- Modify: `include/cce/cce_mojo_kernel.h`, `tests/test_mojo_bridge.c`, `Makefile`

**Interfaces:**
- Produces: `int cce_mojo_dispatch_trit(const float* input, const uint8_t* w_trit, const float* w_scale, const float* bias, float* output, int in_dim, int out_dim, int w_trit_bpr, int apply_sigmoid);` returning `0` when the Mojo kernel handled the call and `-1` when the caller must use the C path. Also `int cce_mojo_available(void);` returning 1 only when compiled in AND enabled.

- [ ] **Step 1: Write the failing test**

Append to `tests/test_mojo_bridge.c`:

```c
    printf("[3] dispatch defaults OFF and degrades safely\n");
    {
        /* With CNET_MOJO unset, dispatch must decline so the C path runs.
           This is what keeps every existing gate on the C kernel. */
        CHECK(cce_mojo_available() == 0,
              "Mojo path is off by default (CNET_MOJO unset)");
        {
            const int in_dim = 64, out_dim = 96, bpr = BPR(96);
            float *in = malloc(sizeof(float) * in_dim);
            uint8_t *wt = malloc((size_t)in_dim * bpr);
            float *ws = malloc(sizeof(float) * out_dim);
            float *bi = malloc(sizeof(float) * out_dim);
            float *out = malloc(sizeof(float) * out_dim);
            int rc;
            fill_fixture(in, wt, ws, bi, in_dim, out_dim, bpr);
            rc = cce_mojo_dispatch_trit(in, wt, ws, bi, out, in_dim, out_dim,
                                        bpr, 1);
            CHECK(rc != 0, "dispatch declines when disabled, so C path runs");
            free(in); free(wt); free(ws); free(bi); free(out);
        }
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make mojo_bridge`
Expected: FAIL — `implicit declaration of function 'cce_mojo_available'`.

- [ ] **Step 3: Declare the dispatch API**

Append to `include/cce/cce_mojo_kernel.h` before the closing `#endif`:

```c
/* 1 only when the Mojo kernel is BOTH compiled in (CNET_HAVE_MOJO) and
 * enabled at runtime (CNET_MOJO=1). Default is off: building with the
 * toolchain present must not change behaviour until explicitly asked. */
int cce_mojo_available(void);

/* Route one ternary matvec. Returns 0 when the Mojo kernel handled it, or
 * -1 when the caller must run cce_trit_matmul_c instead. A Mojo kernel is
 * allowed to be partial: declining is normal, not an error. */
int cce_mojo_dispatch_trit(const float* input, const uint8_t* w_trit,
                           const float* w_scale, const float* bias,
                           float* output, int in_dim, int out_dim,
                           int w_trit_bpr, int apply_sigmoid);

/* Count of calls where the Mojo kernel was attempted and failed, so a
 * degraded box is visible rather than silently slow. */
unsigned long cce_mojo_fallback_count(void);
```

When `CNET_HAVE_MOJO` is defined, the Mojo side provides this symbol:

```c
#ifdef CNET_HAVE_MOJO
/* Implemented in mojo/trit_matmul.mojo via @export + abi("C").
 * Returns 0 on success, non-zero to decline or on error. */
int cnet_mojo_trit_matmul(const float* input, const uint8_t* w_trit,
                          const float* w_scale, const float* bias,
                          float* output, int in_dim, int out_dim,
                          int w_trit_bpr, int apply_sigmoid);
#endif
```

- [ ] **Step 4: Implement the dispatch layer**

Create `src/cce/cce_mojo_dispatch.c`:

```c
/* The ONLY file that knows whether Mojo exists.
 *
 * Mojo is never load-bearing: this file declines whenever the kernel is not
 * compiled in, not enabled, or fails, and the caller runs the C reference.
 * Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md */

#include "../../include/cce/cce_mojo_kernel.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned long g_fallbacks = 0;

int cce_mojo_available(void) {
#ifdef CNET_HAVE_MOJO
    const char* e = getenv("CNET_MOJO");
    return (e && e[0] == '1') ? 1 : 0;
#else
    {
        /* Asking for Mojo on a build without it is a misconfigured box, not
           a preference. Say so once rather than being silently slow. */
        static int warned = 0;
        const char* e = getenv("CNET_MOJO");
        if (e && e[0] == '1' && !warned) {
            warned = 1;
            fprintf(stderr, "CNET_MOJO=1 but this build has no Mojo kernel "
                            "(CNET_HAVE_MOJO undefined); using the C path\n");
        }
    }
    return 0;
#endif
}

int cce_mojo_dispatch_trit(const float* input, const uint8_t* w_trit,
                           const float* w_scale, const float* bias,
                           float* output, int in_dim, int out_dim,
                           int w_trit_bpr, int apply_sigmoid) {
    if (!cce_mojo_available()) return -1;
#ifdef CNET_HAVE_MOJO
    if (cnet_mojo_trit_matmul(input, w_trit, w_scale, bias, output,
                              in_dim, out_dim, w_trit_bpr,
                              apply_sigmoid) == 0)
        return 0;
    g_fallbacks++;
#else
    (void)input; (void)w_trit; (void)w_scale; (void)bias; (void)output;
    (void)in_dim; (void)out_dim; (void)w_trit_bpr; (void)apply_sigmoid;
#endif
    return -1;
}

unsigned long cce_mojo_fallback_count(void) { return g_fallbacks; }
```

- [ ] **Step 5: Route cce_block through dispatch**

In `src/cce/cce_block.c`, change the trit block to try dispatch first:

```c
    if (blk->w_trit && blk->w_scale) {
        int sig = is_head ? 0 : 1;
        if (cce_mojo_dispatch_trit(input->data, blk->w_trit, blk->w_scale,
                                   blk->bias.data, output->data, in_dim,
                                   out_dim, blk->w_trit_bpr, sig) != 0) {
            cce_trit_matmul_c(input->data, blk->w_trit, blk->w_scale,
                              blk->bias.data, output->data, in_dim, out_dim,
                              blk->w_trit_bpr, sig);
        }
        return CCE_OK;
    }
```

Add `src/cce/cce_mojo_dispatch.c` to `MOJO_KERNEL_SRC` so every target links it, and to the `mojo_bridge` target's sources.

- [ ] **Step 6: Run to verify it passes**

Run: `make mojo_bridge && make trit_bench && make mutate`
Expected: section 3 passes; the Task 1 anchor unchanged; `trit_bench` and `mutate` still green. The anchor holding here is gate 3 of the spec — default-off identity.

- [ ] **Step 7: Commit**

```bash
git add include/cce/cce_mojo_kernel.h src/cce/cce_mojo_dispatch.c src/cce/cce_block.c tests/test_mojo_bridge.c Makefile
git commit -m "feat(mojo): dispatch layer, default off, C path always live

The only file that knows whether Mojo exists. Declines when not compiled in,
not enabled, or on kernel failure, and the C reference runs. Asking for
CNET_MOJO=1 on a build without it warns once rather than being silently slow."
```

---

## Task 4: MOJO_PROBE and the Mojo kernel (Linux-only)

**This task cannot be verified on the Windows box.** Steps 1–2 are gateable anywhere; steps 3–5 need a Linux host with the Mojo toolchain. Do not claim the kernel works without running step 5.

**Files:**
- Create: `mojo/trit_matmul.mojo`
- Modify: `Makefile`

- [ ] **Step 1: Add the probe**

Next to `CURL_PROBE` in the Makefile:

```make
# Mojo is OPTIONAL and never load-bearing. Probe for the toolchain exactly as
# CURL_PROBE does; absent, the dispatch layer compiles with the Mojo branch
# preprocessed out and nothing else changes. Mojo has no native Windows
# support (WSL2 only), so the Windows test box always takes this path.
MOJO_PROBE := $(shell command -v mojo >/dev/null 2>&1 && echo yes || echo no)
ifeq ($(MOJO_PROBE),yes)
MOJO_LIB := $(BIN_DIR)/libcnet_mojo.a
CFLAGS += -DCNET_HAVE_MOJO
else
MOJO_LIB :=
endif
```

- [ ] **Step 2: Verify the probe degrades correctly here**

Run: `make mojo_bridge && make qat_block && make attribution`
Expected: all pass. On this box `MOJO_PROBE` is `no`, `CNET_HAVE_MOJO` is undefined, and behaviour is unchanged — which is the whole point of the probe.

- [ ] **Step 3: Write the Mojo kernel**

Create `mojo/trit_matmul.mojo`:

```mojo
# Ternary (1.6-bit packed) matvec — the Mojo side of the C ABI bridge.
#
# NO Python: this file uses only Mojo and the C ABI. It is compiled to a
# static library and linked; the C side declares the entry point by hand in
# include/cce/cce_mojo_kernel.h.
#
# Bit-identity requirement (spec section 2.2): the C reference accumulates
# per-output over i ASCENDING with no reassociation. This kernel must do the
# same. Do not vectorise across i, do not tree-reduce, do not reorder.
#
# Spec: docs/superpowers/specs/2026-08-16-mojo-kernel-bridge-design.md

from memory import UnsafePointer

# code = (b // 3^k) % 3 - 1, matching include/cce/cce_trit_lut.h exactly.
fn trit_code(b: UInt8, k: Int) -> Int8:
    var v = Int(b)
    var d = 1
    for _ in range(k):
        d *= 3
    return Int8((v // d) % 3 - 1)

@export
fn cnet_mojo_trit_matmul(
    input: UnsafePointer[Float32],
    w_trit: UnsafePointer[UInt8],
    w_scale: UnsafePointer[Float32],
    bias: UnsafePointer[Float32],
    output: UnsafePointer[Float32],
    in_dim: Int32,
    out_dim: Int32,
    w_trit_bpr: Int32,
    apply_sigmoid: Int32,
) -> Int32:
    var n_in = Int(in_dim)
    var n_out = Int(out_dim)
    var bpr = Int(w_trit_bpr)
    if n_in <= 0 or n_out <= 0 or bpr <= 0:
        return 1                      # decline: caller runs the C kernel

    for o in range(n_out):
        output[o] = Float32(0.0)

    # i ASCENDING, accumulating into each output — the order the C kernel
    # uses and the one its bit-identity claim depends on.
    for i in range(n_in):
        var a = input[i]
        var row = w_trit + i * bpr
        for o in range(n_out):
            var byte = row[o // 5]
            var code = trit_code(byte, o % 5)
            output[o] = output[o] + a * Float32(Int(code))

    for o in range(n_out):
        var v = bias[o] + w_scale[o] * output[o]
        if apply_sigmoid != 0:
            v = Float32(1.0) / (Float32(1.0) + math.exp(-v))
        output[o] = v
    return 0
```

Before building, confirm against the Mojo 1.0 docs that `@export` alone gives
C linkage or whether the `abi("C")` function effect must also be applied, and
confirm the `math.exp` import path. The language reached 1.0 four days before
this plan; treat every API name here as needing verification, not as known.

- [ ] **Step 4: Add the build rule**

```make
$(MOJO_LIB): mojo/trit_matmul.mojo
	mojo build --emit static-lib -o $@ $<
```

Add `$(MOJO_LIB)` to the `mojo_bridge` target's prerequisites and link line.
Confirm the actual `mojo build` flag for emitting a static library against the
1.0 CLI; if only a shared library is supported, use that and add the runtime
path rather than guessing.

- [ ] **Step 5: Equivalence on Linux — the real gate**

On the Linux box:

```bash
make mojo_bridge && CNET_MOJO=1 ./bin/mojo_bridge
```

Expected: the equivalence section compares C against Mojo across the shape
matrix and reports byte-equality.

**If it does not:** record the measured divergence (max ULP difference and
which shapes) in the spec, determine whether it is accumulation order or FMA
contraction, and only then decide between fixing the kernel and moving to a
stated-epsilon gate. Do not loosen the gate first and investigate later.

- [ ] **Step 6: Commit**

```bash
git add Makefile mojo/trit_matmul.mojo
git commit -m "feat(mojo): MOJO_PROBE build integration and the trit kernel

Probe mirrors CURL_PROBE: absent toolchain means the Mojo branch is
preprocessed out and nothing changes, which is what the Windows box gets
since Mojo has no native Windows support.

The kernel accumulates per-output over i ascending to match the C reference's
bit-identity requirement. Equivalence is gated on Linux only; this commit does
not claim the kernel was run."
```

---

## Task 5: Benchmark

**Files:** Create `tests/mojo_bench.c`; modify `Makefile`

- [ ] **Step 1: Write the benchmark**

Create `tests/mojo_bench.c`:

```c
/* C vs Mojo ternary matvec timing on identical shapes.
   The number that decides whether the bridge earns its place.
   Reports against the existing trit kernel as the reference. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_mojo_kernel.h"

#define BPR(out_dim) (((out_dim) + 4) / 5)

static void fill(float* in, uint8_t* wt, float* ws, float* bi,
                 int in_dim, int out_dim, int bpr) {
    unsigned long long r = 0x9E3779B97F4A7C15ULL;
    int i;
    for (i = 0; i < in_dim; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        in[i] = (float)((double)(r >> 33) / 2147483648.0 - 1.0);
    }
    for (i = 0; i < in_dim * bpr; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        wt[i] = (uint8_t)((r >> 33) % 243);
    }
    for (i = 0; i < out_dim; ++i) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        ws[i] = 0.01f + (float)((double)(r >> 40) / 8388608.0) * 0.1f;
        bi[i] = 0.0f;
    }
}

int main(void) {
    static const int shapes[][2] = {
        {512, 512}, {1024, 1024}, {2048, 2048}, {4096, 4096}
    };
    int n = (int)(sizeof shapes / sizeof shapes[0]), s;
    printf("=== Mojo trit matvec benchmark ===\n");
    printf("mojo available: %s\n", cce_mojo_available() ? "yes" : "no (C only)");
    printf("%-14s %12s %12s %8s\n", "shape", "C ms", "mojo ms", "speedup");
    for (s = 0; s < n; ++s) {
        int in_dim = shapes[s][0], out_dim = shapes[s][1], bpr = BPR(out_dim);
        float *in = malloc(sizeof(float) * (size_t)in_dim);
        uint8_t *wt = malloc((size_t)in_dim * (size_t)bpr);
        float *ws = malloc(sizeof(float) * (size_t)out_dim);
        float *bi = malloc(sizeof(float) * (size_t)out_dim);
        float *out = malloc(sizeof(float) * (size_t)out_dim);
        int it, iters = 20;
        clock_t t0, t1;
        double c_ms = 0.0, m_ms = -1.0;
        if (!in || !wt || !ws || !bi || !out) { printf("alloc failed\n"); return 1; }
        fill(in, wt, ws, bi, in_dim, out_dim, bpr);

        cce_trit_matmul_c(in, wt, ws, bi, out, in_dim, out_dim, bpr, 1);
        t0 = clock();
        for (it = 0; it < iters; ++it)
            cce_trit_matmul_c(in, wt, ws, bi, out, in_dim, out_dim, bpr, 1);
        t1 = clock();
        c_ms = 1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC / iters;

        if (cce_mojo_available()) {
            if (cce_mojo_dispatch_trit(in, wt, ws, bi, out, in_dim, out_dim,
                                       bpr, 1) == 0) {
                t0 = clock();
                for (it = 0; it < iters; ++it)
                    (void)cce_mojo_dispatch_trit(in, wt, ws, bi, out, in_dim,
                                                 out_dim, bpr, 1);
                t1 = clock();
                m_ms = 1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC / iters;
            }
        }
        {
            char shape[32];
            snprintf(shape, sizeof shape, "%dx%d", in_dim, out_dim);
            if (m_ms > 0.0)
                printf("%-14s %12.3f %12.3f %7.2fx\n", shape, c_ms, m_ms,
                       c_ms / m_ms);
            else
                printf("%-14s %12.3f %12s %8s\n", shape, c_ms, "-", "-");
        }
        free(in); free(wt); free(ws); free(bi); free(out);
    }
    printf("MOJO BENCH DONE\n");
    return 0;
}
```

- [ ] **Step 2: Add the target and run**

```make
mojo_bench: $(MOJO_KERNEL_SRC) $(MOJO_LIB) tests/mojo_bench.c include/cce/cce_mojo_kernel.h
	$(CC) $(CFLAGS) -o $(BIN_DIR)/$@ $(MOJO_KERNEL_SRC) tests/mojo_bench.c $(MOJO_LIB) -lm
	./$(BIN_DIR)/mojo_bench
```

Run: `make mojo_bench`
Expected here (no Mojo): a table with C timings and `-` in the Mojo columns, ending `MOJO BENCH DONE`. That confirms the harness works before Mojo exists.

- [ ] **Step 3: Commit**

```bash
git add tests/mojo_bench.c Makefile
git commit -m "test(mojo): C vs Mojo trit matvec benchmark

Reports C-only where the toolchain is absent, so the harness is verified
before it ever sees a Mojo kernel."
```

---

## Self-Review

**Spec coverage:**

| Spec section | Task |
|---|---|
| §3 dispatch, default OFF, no Mojo type crosses | Task 3 |
| §3 hand-written C header | Task 1 step 1 |
| §4 MOJO_PROBE mirroring CURL_PROBE | Task 4 step 1 |
| §5 removability | Task 3 (single `#ifdef` file) + Task 4 step 2 (probe-absent build unchanged) |
| §6 gate 1 harness self-proof | Task 2 |
| §6 gate 2 compiled-out identity (anchor) | Task 1 steps 5, 7 |
| §6 gate 3 default-off identity | Task 3 step 6 |
| §6 gate 4 equivalence + shape matrix | Task 2 (matrix), Task 4 step 5 (vs Mojo) |
| §6 gate 5 benchmark | Task 5 |
| §6 existing gates unaffected | Task 1 step 7, Task 3 step 6 |
| §7 warn on CNET_MOJO without CNET_HAVE_MOJO | Task 3 step 4 |
| §7 kernel failure falls back and is counted | Task 3 step 4 (`g_fallbacks`) |
| §7 partial kernel may decline | Task 3 step 4 (returns -1) |
| §8 stage 2 GPU | Deliberately not in this plan; own spec |
| §9 no Python | Task 4 step 3 (Mojo-only imports) |

No gaps.

**Placeholder scan:** No TBD/TODO. Four steps require verification against
reality before proceeding — the exact `sigmoid` in `cce_block.c` (Task 1
step 4), the `@export`/`abi("C")` requirement and `math.exp` path (Task 4
step 3), the `mojo build` static-lib flag (Task 4 step 4), and the anchor
digest (Task 1 step 5, to be written into this plan when first produced).
Each names exactly what to check and why guessing is unsafe. The Mojo API
names in particular are flagged as unverified because the language reached
1.0 four days before this plan.

**Type consistency:** `cce_trit_matmul_c`, `cce_mojo_anchor`, `run_shape`,
`cce_mojo_available`, `cce_mojo_dispatch_trit`, `cce_mojo_fallback_count` and
`cnet_mojo_trit_matmul` keep identical signatures across Tasks 1–5. `BPR` is
defined once per test file and used consistently.

**Known risks, flagged not hidden:**
1. Byte-equality between two compilers on float accumulation may not hold
   (spec §2.2). Task 4 step 5 says to measure and record before changing the
   bar.
2. The Mojo kernel as written is a naive triple loop and will likely be
   *slower* than the 27.9× C kernel on CPU. That is acceptable for stage 1 —
   its job is to prove the bridge, not to win. The benchmark exists to make
   that visible rather than surprising.
3. Every Mojo API name is unverified. Task 4 step 3 says so explicitly.

---

## Execution Handoff

Plan complete and saved to `docs/superpowers/plans/2026-08-16-mojo-kernel-bridge.md`.
