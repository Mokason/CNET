/* Microbench: dense attend vs pre-attention stream_ix mask (GGUF residual model).
 *
 * Models one head step:
 *   score fill [jmin..t] + optional -inf mask + softmax + V gather
 * Does not load a full GGUF (isolates mask overhead / savings).
 *
 *   make stream_attend_bench
 */
#include "../include/cce/cce_sparse_kv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void fill_qv(float *q, float *K, float *V, int T, int hd, unsigned seed) {
    int i, d;
    for (d = 0; d < hd; d++) {
        seed = seed * 1664525u + 1013904223u;
        q[d] = ((seed >> 16) & 0xff) / 128.0f - 1.0f;
    }
    for (i = 0; i < T; i++) {
        for (d = 0; d < hd; d++) {
            seed = seed * 1664525u + 1013904223u;
            K[i * hd + d] = ((seed >> 16) & 0xff) / 128.0f - 1.0f;
            V[i * hd + d] = ((seed >> 8) & 0xff) / 128.0f - 1.0f;
        }
    }
}

/* Dense: all j in [0,T) */
static void attend_dense(const float *q, const float *K, const float *V, int T,
                         int hd, float *out, float *scores) {
    int j, d;
    float scale = 1.0f / sqrtf((float)hd);
    float maxs = -1e30f, sum = 0.0f;
    for (j = 0; j < T; j++) {
        float s = 0.0f;
        const float *kh = K + j * hd;
        for (d = 0; d < hd; d++) s += q[d] * kh[d];
        scores[j] = s * scale;
        if (scores[j] > maxs) maxs = scores[j];
    }
    for (j = 0; j < T; j++) {
        scores[j] = expf(scores[j] - maxs);
        sum += scores[j];
    }
    if (sum < 1e-20f) sum = 1e-20f;
    for (j = 0; j < T; j++) scores[j] /= sum;
    memset(out, 0, (size_t)hd * sizeof(float));
    for (j = 0; j < T; j++) {
        const float *vh = V + j * hd;
        float w = scores[j];
        for (d = 0; d < hd; d++) out[d] += w * vh[d];
    }
}

/* Pre-attention mask: only active[] (+ last token) get finite scores */
static void attend_masked(const float *q, const float *K, const float *V, int T,
                          int hd, const int *active, int n_act, float *out,
                          float *scores) {
    int j, d, a;
    float scale = 1.0f / sqrtf((float)hd);
    float maxs = -1e30f, sum = 0.0f;
    unsigned char *on = (unsigned char *)calloc((size_t)T, 1);
    if (!on) return;
    for (a = 0; a < n_act; a++) {
        int p = active[a];
        if (p >= 0 && p < T) on[p] = 1;
    }
    on[T - 1] = 1; /* current token */
    for (j = 0; j < T; j++) {
        if (!on[j]) {
            scores[j] = -1e30f;
            continue;
        }
        {
            float s = 0.0f;
            const float *kh = K + j * hd;
            for (d = 0; d < hd; d++) s += q[d] * kh[d];
            scores[j] = s * scale;
            if (scores[j] > maxs) maxs = scores[j];
        }
    }
    for (j = 0; j < T; j++) {
        scores[j] = expf(scores[j] - maxs);
        sum += scores[j];
    }
    if (sum < 1e-20f) sum = 1e-20f;
    for (j = 0; j < T; j++) scores[j] /= sum;
    memset(out, 0, (size_t)hd * sizeof(float));
    for (j = 0; j < T; j++) {
        if (!on[j]) continue;
        {
            const float *vh = V + j * hd;
            float w = scores[j];
            for (d = 0; d < hd; d++) out[d] += w * vh[d];
        }
    }
    free(on);
}

static int bench_case(int T, int budget, int hd, int reps) {
    cce_kv_stream_index ix;
    cce_specialist_kv_budget b;
    float *q, *K, *V, *out_d, *out_m, *sc;
    int *act, n_act = 0, i;
    double t0, t1, dense_s, mask_s;
    size_t bytes_full, bytes_idx;

    memset(&b, 0, sizeof b);
    b.max_tokens = budget;
    b.initial_tokens = 4;
    b.recent_tokens = budget / 2 > 8 ? budget / 2 : 8;
    b.long_range_stride = 32;
    if (cce_kv_stream_index_init(&ix, &b, T) != CCE_OK) return -1;
    cce_kv_stream_index_set_slot_sizes(&ix, hd, hd);
    for (i = 0; i < T; i++)
        (void)cce_kv_stream_index_on_append(&ix, i, NULL, 0);

    act = (int *)malloc((size_t)budget * sizeof(int));
    q = (float *)malloc((size_t)hd * sizeof(float));
    K = (float *)malloc((size_t)T * hd * sizeof(float));
    V = (float *)malloc((size_t)T * hd * sizeof(float));
    out_d = (float *)malloc((size_t)hd * sizeof(float));
    out_m = (float *)malloc((size_t)hd * sizeof(float));
    sc = (float *)malloc((size_t)T * sizeof(float));
    if (!act || !q || !K || !V || !out_d || !out_m || !sc) return -1;
    fill_qv(q, K, V, T, hd, 0xA5A5u);
    (void)cce_kv_stream_index_active(&ix, act, budget, &n_act);

    /* warmup */
    attend_dense(q, K, V, T, hd, out_d, sc);
    attend_masked(q, K, V, T, hd, act, n_act, out_m, sc);

    t0 = now_s();
    for (i = 0; i < reps; i++) attend_dense(q, K, V, T, hd, out_d, sc);
    t1 = now_s();
    dense_s = (t1 - t0) / (double)reps;

    t0 = now_s();
    for (i = 0; i < reps; i++)
        attend_masked(q, K, V, T, hd, act, n_act, out_m, sc);
    t1 = now_s();
    mask_s = (t1 - t0) / (double)reps;

    bytes_full = (size_t)T * (size_t)(hd + hd) * sizeof(float);
    bytes_idx = (size_t)n_act * (size_t)(hd + hd) * sizeof(float);

    printf("%6d %6d %6d %10.3f %10.3f %8.2fx %8.2f%% %10.2f %10.2f\n", T,
           budget, n_act, dense_s * 1e6, mask_s * 1e6,
           mask_s > 0 ? dense_s / mask_s : 0.0,
           (1.0 - (double)bytes_idx / (double)bytes_full) * 100.0,
           bytes_full / (1024.0 * 1024.0), bytes_idx / (1024.0 * 1024.0));

    free(act);
    free(q);
    free(K);
    free(V);
    free(out_d);
    free(out_m);
    free(sc);
    return 0;
}

int main(void) {
    static const int Ts[] = {512, 1024, 2048, 4096};
    static const int Bs[] = {32, 64, 128, 256};
    const int hd = 128;
    int ti, bi, fails = 0;
    FILE *fp;

    printf("=== Stream pre-attention mask vs dense (1 head residual model) ===\n");
    printf("head_dim=%d  K+V bytes modeled as 2*hd*float per position\n", hd);
    printf("%6s %6s %6s %10s %10s %8s %8s %10s %10s\n", "T", "budget", "active",
           "dense_us", "mask_us", "speedup", "mem_save", "full_MiB", "idx_MiB");
    printf("------ ------ ------ ---------- ---------- -------- -------- ---------- "
           "----------\n");

    fp = fopen("logs/stream_attend_bench.jsonl", "w");
    for (ti = 0; ti < (int)(sizeof Ts / sizeof Ts[0]); ti++) {
        for (bi = 0; bi < (int)(sizeof Bs / sizeof Bs[0]); bi++) {
            int T = Ts[ti], B = Bs[bi];
            int reps;
            if (B > T) continue;
            reps = T >= 2048 ? 400 : 1200;
            if (bench_case(T, B, hd, reps) != 0) fails++;
            if (fp) {
                /* re-run once for json fields via stream index only */
                cce_kv_stream_index ix;
                cce_specialist_kv_budget b;
                memset(&b, 0, sizeof b);
                b.max_tokens = B;
                b.initial_tokens = 4;
                b.recent_tokens = B / 2;
                b.long_range_stride = 32;
                cce_kv_stream_index_init(&ix, &b, T);
                cce_kv_stream_index_set_slot_sizes(&ix, hd, hd);
                {
                    int i;
                    for (i = 0; i < T; i++)
                        cce_kv_stream_index_on_append(&ix, i, NULL, 0);
                }
                fprintf(fp,
                        "{\"T\":%d,\"budget\":%d,\"active\":%d,\"mem_ratio\":%.6f,"
                        "\"mem_save_pct\":%.2f,\"full_MiB\":%.4f,\"idx_MiB\":%.4f}\n",
                        T, B, ix.active_n, cce_kv_stream_index_mem_ratio(&ix),
                        (1.0 - cce_kv_stream_index_mem_ratio(&ix)) * 100.0,
                        ix.bytes_full_kv / (1024.0 * 1024.0),
                        ix.bytes_index_kv / (1024.0 * 1024.0));
            }
        }
    }
    if (fp) fclose(fp);

    printf("\nNotes:\n");
    printf("  dense_us / mask_us = one head attention step (score+softmax+V)\n");
    printf("  speedup = dense_us/mask_us (compute proxy; full model also has MLP)\n");
    printf("  mem_save = 1 - (active/T) on K+V footprint for this head dim\n");
    printf("  GGUF multi-layer multi-head scales roughly with L*H_heads for compute\n");
    printf("log → logs/stream_attend_bench.jsonl\n");
    if (fails) {
        printf("STREAM_ATTEND_BENCH_FAIL\n");
        return 1;
    }
    printf("STREAM_ATTEND_BENCH_PASS\n");
    return 0;
}
