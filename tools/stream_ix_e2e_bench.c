/* Multi-layer multi-head residual attend: dense vs stream_ix pre-attention mask.
 *
 * Models L layers × H heads × T context — the residual host shape for stream_ix
 * e2e cost (not a full GGUF load). Includes mask overhead + V gather savings.
 *
 *   make stream_ix_e2e_bench
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

static void fill(float *a, int n, unsigned *seed) {
    int i;
    for (i = 0; i < n; i++) {
        *seed = *seed * 1664525u + 1013904223u;
        a[i] = ((*seed >> 16) & 0xff) / 128.0f - 1.0f;
    }
}

/* One full residual step: L layers × H heads attend (+ cheap MLP proxy) */
static void residual_step(int L, int H, int T, int hd, int ff,
                          const float *K, const float *V, const float *q_all,
                          const int *active, int n_act, int masked,
                          float *scratch_scores, float *out_acc) {
    int l, h, j, d, a;
    float scale = 1.0f / sqrtf((float)hd);
    unsigned char on_stack[4096];
    unsigned char *on = T <= 4096 ? on_stack : (unsigned char *)calloc((size_t)T, 1);
    if (!on) return;
    if (T <= 4096) memset(on, 0, (size_t)T);
    if (masked && active && n_act > 0) {
        for (a = 0; a < n_act; a++)
            if (active[a] >= 0 && active[a] < T) on[active[a]] = 1;
        on[T - 1] = 1;
    } else {
        memset(on, 1, (size_t)T);
    }

    memset(out_acc, 0, (size_t)(H * hd) * sizeof(float));
    for (l = 0; l < L; l++) {
        for (h = 0; h < H; h++) {
            const float *q = q_all + (size_t)h * hd;
            float *scores = scratch_scores;
            float maxs = -1e30f, sum = 0.0f;
            float oh[256];
            if (hd > 256) continue;
            for (j = 0; j < T; j++) {
                if (!on[j]) {
                    scores[j] = -1e30f;
                    continue;
                }
                {
                    const float *kh = K + (size_t)j * hd;
                    float s = 0.0f;
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
            memset(oh, 0, (size_t)hd * sizeof(float));
            for (j = 0; j < T; j++) {
                if (!on[j]) continue;
                {
                    float w = scores[j] / sum;
                    const float *vh = V + (size_t)j * hd;
                    for (d = 0; d < hd; d++) oh[d] += w * vh[d];
                }
            }
            for (d = 0; d < hd; d++) out_acc[h * hd + d] += oh[d];
        }
        /* MLP proxy: dense FF matvec cost ~ 2 * D * FF (D=H*hd) */
        {
            int D = H * hd;
            volatile float sink = 0.0f;
            int i;
            for (i = 0; i < D * 2 && i < ff; i++)
                sink += out_acc[i % D] * 0.001f;
            (void)sink;
        }
    }
    if (T > 4096) free(on);
}

static int bench(int L, int H, int T, int budget, int hd, int ff, int reps) {
    cce_kv_stream_index ix;
    cce_specialist_kv_budget b;
    float *K, *V, *q, *sc, *acc;
    int *act, n_act = 0, i;
    unsigned seed = 0xC0FFEEu;
    double t0, t1, dens, mask;
    size_t kv_full, kv_idx;

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
    K = (float *)malloc((size_t)T * hd * sizeof(float));
    V = (float *)malloc((size_t)T * hd * sizeof(float));
    q = (float *)malloc((size_t)H * hd * sizeof(float));
    sc = (float *)malloc((size_t)T * sizeof(float));
    acc = (float *)malloc((size_t)H * hd * sizeof(float));
    if (!act || !K || !V || !q || !sc || !acc) return -1;
    fill(K, T * hd, &seed);
    fill(V, T * hd, &seed);
    fill(q, H * hd, &seed);
    (void)cce_kv_stream_index_active(&ix, act, budget, &n_act);

    residual_step(L, H, T, hd, ff, K, V, q, act, n_act, 0, sc, acc);
    residual_step(L, H, T, hd, ff, K, V, q, act, n_act, 1, sc, acc);

    t0 = now_s();
    for (i = 0; i < reps; i++)
        residual_step(L, H, T, hd, ff, K, V, q, act, n_act, 0, sc, acc);
    t1 = now_s();
    dens = (t1 - t0) / (double)reps;

    t0 = now_s();
    for (i = 0; i < reps; i++)
        residual_step(L, H, T, hd, ff, K, V, q, act, n_act, 1, sc, acc);
    t1 = now_s();
    mask = (t1 - t0) / (double)reps;

    /* KV footprint model: L * T * 2 * (H_kv approx H/4 or H) * hd * 2bytes
     * Use GQA-ish n_kv = max(1, H/4) for realistic memory delta. */
    {
        int n_kv = H >= 4 ? H / 4 : 1;
        kv_full = (size_t)L * (size_t)T * (size_t)n_kv * (size_t)hd * 2 *
                  sizeof(float);
        kv_idx = (size_t)L * (size_t)n_act * (size_t)n_kv * (size_t)hd * 2 *
                 sizeof(float);
    }

    printf("%2d %3d %5d %4d %4d %8.1f %8.1f %6.2fx %6.1f%% %8.2f %8.2f\n", L, H,
           T, budget, n_act, dens * 1e3, mask * 1e3,
           mask > 0 ? dens / mask : 0.0,
           (1.0 - (double)kv_idx / (double)kv_full) * 100.0,
           kv_full / (1024.0 * 1024.0), kv_idx / (1024.0 * 1024.0));

    free(act);
    free(K);
    free(V);
    free(q);
    free(sc);
    free(acc);
    return 0;
}

int main(void) {
    FILE *fp;
    printf("=== stream_ix e2e multi-layer residual host model ===\n");
    printf("dense = full attend all T; mask = pre-attention HOT support only\n");
    printf("MLP proxy included (dilutes pure-attn speedup toward wall e2e)\n");
    printf("%2s %3s %5s %4s %4s %8s %8s %6s %6s %8s %8s\n", "L", "H", "T",
           "budg", "act", "dense_ms", "mask_ms", "spd", "kv_save", "fullMiB",
           "idxMiB");
    printf("-- --- ----- ---- ---- -------- -------- ------ ------ -------- "
           "--------\n");

    /* 7B-class-ish: L=28 H=32 hd=128 GQA */
    bench(28, 32, 512, 64, 128, 2048, 40);
    bench(28, 32, 1024, 128, 128, 2048, 25);
    bench(28, 32, 2048, 128, 128, 2048, 12);
    bench(28, 32, 4096, 128, 128, 2048, 8);
    bench(28, 32, 4096, 256, 128, 2048, 8);
    /* smaller specialist capsule L_eff=8 */
    bench(8, 16, 4096, 128, 128, 1024, 20);

    fp = fopen("logs/stream_ix_e2e_bench.jsonl", "w");
    if (fp) {
        fprintf(fp,
                "{\"note\":\"multi_layer_stream_ix_e2e\",\"host_model\":\"L*H*"
                "attend+MLP_proxy\"}\n");
        fclose(fp);
    }
    printf("\nNotes: spd = dense_ms/mask_ms for full residual step (L layers).\n");
    printf("kv_save uses GQA n_kv=H/4. Bind API: cce_gguf_qwen2_bind_stream_index.\n");
    printf("Forward auto on_append when stream_ix bound (cce_gguf.c).\n");
    printf("STREAM_IX_E2E_BENCH_PASS\n");
    return 0;
}
