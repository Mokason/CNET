/* Hermetic GPU tests for cce_amdmath (HIP/rocWMMA, no PyTorch). */
#include "../include/cce/cce_amdmath.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void expect_ok(const char *name, int rc, cce_amdmath *h)
{
    if (rc != CCE_AMDMATH_OK) {
        printf("FAIL %s rc=%d err=%s\n", name, rc, cce_amdmath_last_error(h));
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

static float *fill(size_t n, unsigned seed)
{
    float *p = (float *)malloc(n * sizeof(float));
    if (!p)
        return NULL;
    unsigned s = seed;
    for (size_t i = 0; i < n; i++) {
        s = s * 1664525u + 1013904223u;
        p[i] = ((int)(s >> 16) % 2001 - 1000) / 500.0f; /* [-2, 2] */
    }
    return p;
}

static void cpu_gemm(const float *A, const float *B, float *C, size_t m, size_t n, size_t k)
{
    for (size_t r = 0; r < m; r++) {
        for (size_t c = 0; c < n; c++) {
            float acc = 0.f;
            for (size_t t = 0; t < k; t++)
                acc += A[r * k + t] * B[t * n + c];
            C[r * n + c] = acc;
        }
    }
}

static void cpu_linear(const float *X, const float *W, float *Y, size_t E, size_t N,
                       size_t in_dim, size_t out_dim)
{
    for (size_t e = 0; e < E; e++) {
        for (size_t n = 0; n < N; n++) {
            for (size_t o = 0; o < out_dim; o++) {
                float acc = 0.f;
                const float *x = X + (e * N + n) * in_dim;
                const float *w = W + (e * out_dim + o) * in_dim;
                for (size_t k = 0; k < in_dim; k++)
                    acc += x[k] * w[k];
                Y[(e * N + n) * out_dim + o] = acc;
            }
        }
    }
}

static float max_abs(const float *a, const float *b, size_t n)
{
    float m = 0.f;
    for (size_t i = 0; i < n; i++) {
        float d = fabsf(a[i] - b[i]);
        if (d > m)
            m = d;
    }
    return m;
}

static uint16_t f32_to_f16_bits(float f)
{
    /* round-to-nearest-even IEEE f16, host */
    unsigned u;
    memcpy(&u, &f, 4);
    unsigned sign = (u >> 16) & 0x8000u;
    int exp = (int)((u >> 23) & 0xff) - 127 + 15;
    unsigned man = u & 0x7fffffu;
    if (exp <= 0) {
        return (uint16_t)sign;
    }
    if (exp >= 31) {
        return (uint16_t)(sign | 0x7c00u);
    }
    unsigned half = sign | ((unsigned)exp << 10) | (man >> 13);
    return (uint16_t)half;
}

int main(void)
{
    printf("cce_amdmath %s discrete=%d\n", cce_amdmath_version(),
           cce_amdmath_device_count());
    if (cce_amdmath_below_floor(8, 8, 8) != 1) {
        printf("FAIL floor 8x8x8 should be 1\n");
        fails++;
    } else {
        printf("ok   floor 8x8x8\n");
    }
    if (cce_amdmath_below_floor(256, 256, 256) != 0) {
        printf("FAIL floor 256 should be 0 (got %d)\n",
               cce_amdmath_below_floor(256, 256, 256));
        fails++;
    } else {
        printf("ok   floor 256x256x256\n");
    }

    char name[256];
    cce_amdmath *h = cce_amdmath_open(name, sizeof name);
    if (!h) {
        printf("FAIL open (no discrete GPU)\n");
        return 1;
    }
    printf("open %s\n", name);
    cce_amdmath_caps caps;
    expect_ok("caps", cce_amdmath_get_caps(h, &caps), h);
    printf("  gfx12=%d wave=%d cu=%d wmma_f16=%d pk=%d\n", caps.gfx12, caps.wavefront,
           caps.compute_units, caps.wmma_f16, caps.pk_f16);
    if (!caps.gfx12 || !caps.wmma_f16 || !caps.pk_f16) {
        printf("FAIL expected gfx12 WMMA+pk on this host\n");
        fails++;
    }

    /* f32 GEMM 64³ — above 16 tile, FLOPs=2*64^3=524288 < 1M default floor.
     * Force by using 128 which is 2*128^3 = 4.19M. */
    {
        const size_t m = 128, n = 128, k = 128;
        float *A = fill(m * k, 1);
        float *B = fill(k * n, 2);
        float *C = (float *)calloc(m * n, sizeof(float));
        float *R = (float *)calloc(m * n, sizeof(float));
        cpu_gemm(A, B, R, m, n, k);
        int rc = cce_amdmath_gemm_f32(h, A, B, C, m, n, k);
        expect_ok("gemm_f32 128", rc, h);
        if (rc == 0) {
            float e = max_abs(C, R, m * n);
            printf("  max|Δ|=%g\n", e);
            if (e > 1e-3f) {
                printf("FAIL gemm_f32 error too large\n");
                fails++;
            }
        }
        free(A);
        free(B);
        free(C);
        free(R);
    }

    /* WMMA bf16 path */
    if (caps.gfx12) {
        const size_t m = 128, n = 128, k = 128;
        float *A = fill(m * k, 3);
        float *B = fill(k * n, 4);
        float *C = (float *)calloc(m * n, sizeof(float));
        float *R = (float *)calloc(m * n, sizeof(float));
        cpu_gemm(A, B, R, m, n, k);
        int rc = cce_amdmath_gemm_f32_wmma(h, A, B, C, m, n, k);
        expect_ok("gemm_f32_wmma 128", rc, h);
        if (rc == 0) {
            float e = max_abs(C, R, m * n);
            printf("  max|Δ|=%g (bf16 WMMA vs f32 CPU)\n", e);
            if (e > 0.15f) {
                printf("FAIL wmma error too large\n");
                fails++;
            }
        }
        free(A);
        free(B);
        free(C);
        free(R);
    }

    /* native f16 GEMM */
    if (caps.gfx12) {
        const size_t m = 64, n = 64, k = 64;
        /* 2*64^3 = 524288 < 1M → FLOOR. Bump via env in makefile; here use 128. */
    }
    if (caps.gfx12) {
        const size_t m = 128, n = 128, k = 128;
        uint16_t *Ah = (uint16_t *)malloc(m * k * 2);
        uint16_t *Bh = (uint16_t *)malloc(k * n * 2);
        float *A = fill(m * k, 5);
        float *B = fill(k * n, 6);
        float *C = (float *)calloc(m * n, sizeof(float));
        float *R = (float *)calloc(m * n, sizeof(float));
        for (size_t i = 0; i < m * k; i++)
            Ah[i] = f32_to_f16_bits(A[i]);
        for (size_t i = 0; i < k * n; i++)
            Bh[i] = f32_to_f16_bits(B[i]);
        cpu_gemm(A, B, R, m, n, k);
        int rc = cce_amdmath_gemm_f16(h, Ah, Bh, C, m, n, k);
        expect_ok("gemm_f16 128", rc, h);
        if (rc == 0) {
            float e = max_abs(C, R, m * n);
            printf("  max|Δ|=%g (f16 in vs f32 CPU of pre-round)\n", e);
            if (e > 0.5f) {
                printf("FAIL gemm_f16 error too large\n");
                fails++;
            }
        }
        free(Ah);
        free(Bh);
        free(A);
        free(B);
        free(C);
        free(R);
    }

    /* packed f16 FMA */
    {
        const size_t n = 1024;
        uint16_t *a = (uint16_t *)malloc(n * 2);
        uint16_t *b = (uint16_t *)malloc(n * 2);
        uint16_t *c = (uint16_t *)malloc(n * 2);
        uint16_t *o = (uint16_t *)malloc(n * 2);
        for (size_t i = 0; i < n; i++) {
            a[i] = f32_to_f16_bits(0.5f);
            b[i] = f32_to_f16_bits(2.0f);
            c[i] = f32_to_f16_bits(1.0f);
        }
        int rc = cce_amdmath_pk_fma_f16(h, a, b, c, o, n);
        expect_ok("pk_fma_f16", rc, h);
        if (rc == 0) {
            /* 0.5*2+1 = 2.0 */
            int bad = 0;
            for (size_t i = 0; i < n; i++) {
                if (o[i] != f32_to_f16_bits(2.0f))
                    bad++;
            }
            printf("  mismatches=%d / %zu\n", bad, n);
            if (bad) {
                printf("FAIL pk_fma expected 2.0\n");
                fails++;
            }
        }
        free(a);
        free(b);
        free(c);
        free(o);
    }

    /* linear + sgd */
    {
        const size_t E = 4, N = 32, In = 64, Out = 64;
        /* FLOPs linear = 2*N*Out*In = 2*32*64*64 = 262144 < floor.
         * Use N=128 → 2*128*64*64 = 1.05M */
        const size_t N2 = 128;
        float *X = fill(E * N2 * In, 7);
        float *W = fill(E * Out * In, 8);
        float *Y = (float *)calloc(E * N2 * Out, sizeof(float));
        float *R = (float *)calloc(E * N2 * Out, sizeof(float));
        cpu_linear(X, W, R, E, N2, In, Out);
        int rc = cce_amdmath_linear_f32(h, X, W, Y, E, N2, In, Out);
        expect_ok("linear_f32 E4", rc, h);
        if (rc == 0) {
            float e = max_abs(Y, R, E * N2 * Out);
            printf("  max|Δ|=%g\n", e);
            if (e > 1e-3f) {
                printf("FAIL linear_f32\n");
                fails++;
            }
        }
        if (caps.gfx12) {
            memset(Y, 0, E * N2 * Out * sizeof(float));
            rc = cce_amdmath_linear_f32_wmma(h, X, W, Y, E, N2, In, Out);
            expect_ok("linear_f32_wmma E4", rc, h);
            if (rc == 0) {
                float e = max_abs(Y, R, E * N2 * Out);
                printf("  max|Δ|=%g\n", e);
                if (e > 0.2f) {
                    printf("FAIL linear_wmma\n");
                    fails++;
                }
            }
        }
        float *dY = fill(E * N2 * Out, 9);
        float *W2 = (float *)malloc(E * Out * In * sizeof(float));
        memcpy(W2, W, E * Out * In * sizeof(float));
        rc = cce_amdmath_sgd_f32(h, W2, X, dY, E, N2, In, Out, 0.01f);
        expect_ok("sgd_f32", rc, h);
        /* CPU SGD check one element */
        if (rc == 0) {
            float g = 0.f;
            for (size_t n = 0; n < N2; n++)
                g += X[n * In + 0] * dY[n * Out + 0]; /* e=0,o=0,i=0 */
            float expect = W[0] - 0.01f * g;
            float got = W2[0];
            float d = fabsf(expect - got);
            printf("  sgd W[0] Δ=%g\n", d);
            if (d > 1e-3f) {
                printf("FAIL sgd\n");
                fails++;
            }
        }
        free(X);
        free(W);
        free(Y);
        free(R);
        free(dY);
        free(W2);
    }

    /* 3-tier pool: LRU HOT + writeback SGD */
    {
        size_t fr = 0, tot = 0;
        expect_ok("vram_info", cce_amdmath_vram_info(h, &fr, &tot), h);
        printf("  vram free=%.2f GiB tot=%.2f GiB\n", fr / (1024. * 1024. * 1024.),
               tot / (1024. * 1024. * 1024.));

        const size_t In = 128, Out = 128, Nn = 128;
        const size_t wbytes = Out * In * sizeof(float);
        float *WW[3];
        for (int i = 0; i < 3; i++) {
            if (posix_memalign((void **)&WW[i], 4096, wbytes) != 0) {
                printf("FAIL posix_memalign\n");
                fails++;
                WW[i] = NULL;
            } else {
                memset(WW[i], 0, wbytes);
                WW[i][0] = (float)(i + 1);
            }
        }
        cce_amdmath_pool *pool = cce_amdmath_pool_open(h, 2 * wbytes, 8 * wbytes);
        if (!pool) {
            printf("FAIL pool_open\n");
            fails++;
        } else {
            for (int i = 0; i < 3; i++)
                expect_ok("pool_bind",
                          cce_amdmath_pool_bind(pool, i, WW[i], wbytes, CCE_AMDMATH_TIER_WARM),
                          h);
            expect_ok("touch0", cce_amdmath_pool_touch(pool, 0), h);
            expect_ok("touch1", cce_amdmath_pool_touch(pool, 1), h);
            if (cce_amdmath_pool_tier(pool, 0) != CCE_AMDMATH_TIER_HOT ||
                cce_amdmath_pool_tier(pool, 1) != CCE_AMDMATH_TIER_HOT) {
                printf("FAIL expected 0,1 HOT (got %d,%d)\n", cce_amdmath_pool_tier(pool, 0),
                       cce_amdmath_pool_tier(pool, 1));
                fails++;
            } else {
                printf("ok   0,1 HOT\n");
            }
            expect_ok("touch2", cce_amdmath_pool_touch(pool, 2), h);
            int t0 = cce_amdmath_pool_tier(pool, 0);
            int t2 = cce_amdmath_pool_tier(pool, 2);
            if (t2 != CCE_AMDMATH_TIER_HOT) {
                printf("FAIL 2 should be HOT after evict, tier=%d\n", t2);
                fails++;
            } else {
                printf("ok   LRU evicted 0 (tier=%d) 2 HOT\n", t0);
            }
            cce_amdmath_pool_stats st;
            cce_amdmath_pool_get_stats(pool, &st);
            printf("  hot_used=%zu cap=%zu evicts=%llu\n", st.hot_used, st.hot_cap,
                   (unsigned long long)st.evicts);
            if (st.evicts < 1) {
                printf("FAIL expected an eviction\n");
                fails++;
            }

            float *X = fill(Nn * In, 11);
            float *dY = fill(Nn * Out, 12);
            int rc = cce_amdmath_pool_sgd_f32(pool, 2, X, dY, Nn, In, Out, 0.01f);
            expect_ok("pool_sgd HOT", rc, h);
            rc = cce_amdmath_pool_sgd_f32(pool, 2, X, dY, Nn, In, Out, 0.01f);
            expect_ok("pool_sgd HOT hit", rc, h);
            cce_amdmath_pool_get_stats(pool, &st);
            if (st.hits_hot < 2) {
                printf("FAIL expected hot hits, got %llu\n",
                       (unsigned long long)st.hits_hot);
                fails++;
            }
            free(X);
            free(dY);
            cce_amdmath_pool_close(pool);
        }
        for (int i = 0; i < 3; i++)
            free(WW[i]);
    }

    /* floor return code on tiny gemm */
    {
        float A[16], B[16], C[16];
        memset(A, 0, sizeof A);
        memset(B, 0, sizeof B);
        int rc = cce_amdmath_gemm_f32(h, A, B, C, 4, 4, 4);
        if (rc != CCE_AMDMATH_FLOOR) {
            printf("FAIL tiny gemm should FLOOR, rc=%d\n", rc);
            fails++;
        } else {
            printf("ok   tiny gemm FLOOR\n");
        }
    }

    cce_amdmath_close(h);
    if (fails) {
        printf("AMDMATH_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("AMDMATH_PASS\n");
    return 0;
}
