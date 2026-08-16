/* Benchmark: streaming-aware KV index vs full-context attend cost model.
 *
 *   make kv_stream_bench && ./bin/kv_stream_bench
 * Pure C. No Python.
 */
#include "../include/cce/cce_sparse_kv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void fill_scores(float *s, int n, unsigned seed) {
    int i;
    for (i = 0; i < n; ++i) {
        seed = seed * 1664525u + 1013904223u;
        s[i] = (float)((seed >> 16) & 0x3ff) * (1.0f / 1024.0f);
    }
    if (n > 10) s[n / 3] = 8.0f;
    if (n > 20) s[n / 2] = 9.5f;
    if (n > 5) s[n - 3] = 7.0f;
}

typedef struct {
    int ctx;
    int budget;
    int k_slot;
    int v_slot;
    double stream_s;
    double rebuild_s;
    double select_once_s;
    int active_n;
    size_t bytes_full;
    size_t bytes_idx;
    float mem_ratio;
    double flops_full;
    double flops_idx;
    double speedup_flops;
    double appends_per_s;
} BenchRow;

static int bench_one(int ctx, int budget, int k_slot, int v_slot, BenchRow *R) {
    cce_kv_stream_index ix;
    cce_specialist_kv_budget b;
    float *scores;
    int i, reps, warm;
    double t0, t1;
    int out[4096], n = 0;

    if (ctx <= 0 || budget <= 0 || !R) return -1;
    if (ctx > CCE_KV_STREAM_IDX_MAX) ctx = CCE_KV_STREAM_IDX_MAX;

    memset(R, 0, sizeof *R);
    R->ctx = ctx;
    R->budget = budget;
    R->k_slot = k_slot;
    R->v_slot = v_slot;

    memset(&b, 0, sizeof b);
    b.max_tokens = budget;
    b.initial_tokens = budget < 8 ? budget / 2 + 1 : 4;
    b.recent_tokens = budget < 16 ? budget / 2 : 32;
    if (b.recent_tokens > budget) b.recent_tokens = budget;
    b.long_range_stride = ctx > 256 ? 64 : 16;
    b.heavy_hitter_fraction = 1.0f;

    scores = (float *)malloc((size_t)ctx * sizeof(float));
    if (!scores) return -1;
    fill_scores(scores, ctx, 0xC0FFEEu);

    if (cce_kv_stream_index_init(&ix, &b, ctx) != CCE_OK) {
        free(scores);
        return -1;
    }
    cce_kv_stream_index_set_slot_sizes(&ix, k_slot, v_slot);

    warm = ctx > 64 ? 8 : 2;
    for (i = 0; i < warm && i < ctx; ++i)
        (void)cce_kv_stream_index_on_append(&ix, i, scores, i + 1);
    cce_kv_stream_index_clear(&ix);

    t0 = now_s();
    for (i = 0; i < ctx; ++i)
        (void)cce_kv_stream_index_on_append(&ix, i, scores, i + 1);
    t1 = now_s();
    R->stream_s = t1 - t0;
    R->appends_per_s = R->stream_s > 0 ? (double)ctx / R->stream_s : 0.0;
    R->active_n = ix.active_n;
    R->bytes_full = ix.bytes_full_kv;
    R->bytes_idx = ix.bytes_index_kv;
    R->mem_ratio = cce_kv_stream_index_mem_ratio(&ix);

    reps = ctx >= 2048 ? 200 : (ctx >= 512 ? 500 : 2000);
    t0 = now_s();
    for (i = 0; i < reps; ++i)
        (void)cce_kv_stream_index_rebuild(&ix, scores, ctx);
    t1 = now_s();
    R->rebuild_s = (t1 - t0) / (double)reps;

    reps = ctx >= 2048 ? 200 : (ctx >= 512 ? 500 : 2000);
    t0 = now_s();
    for (i = 0; i < reps; ++i)
        (void)cce_specialist_select_kv_tokens(scores, ctx, &b, out, budget, &n);
    t1 = now_s();
    R->select_once_s = (t1 - t0) / (double)reps;

    {
        double slot = (double)(k_slot + v_slot);
        R->flops_full = (double)ctx * slot;
        R->flops_idx = (double)R->active_n * slot;
        R->speedup_flops =
            R->flops_idx > 0.0 ? R->flops_full / R->flops_idx : 0.0;
    }

    free(scores);
    return 0;
}

static void print_row(const BenchRow *R) {
    printf("%7d %7d %8d %10.3f %10.3f %10.3f %10.0f %8.4f %10.2fx %12.2f\n",
           R->ctx, R->budget, R->active_n, R->rebuild_s * 1e6,
           R->select_once_s * 1e6, R->stream_s * 1e3, R->appends_per_s,
           (double)R->mem_ratio, R->speedup_flops,
           (double)R->bytes_full / (1024.0 * 1024.0));
}

int main(void) {
    static const int ctxs[] = {128, 512, 1024, 2048, 4096};
    static const int budgets[] = {32, 64, 128, 256};
    const int k_slot = 128;
    const int v_slot = 128;
    int ci, bi, fails = 0;
    BenchRow R;
    double t_all0, t_all1;
    FILE *json;

    printf("=== KV stream index benchmark (C) ===\n");
    printf("host slot k=%d v=%d floats (%.2f KiB/token dense)\n", k_slot, v_slot,
           (k_slot + v_slot) * 4.0 / 1024.0);
    printf("%7s %7s %8s %10s %10s %10s %10s %8s %10s %12s\n", "ctx", "budget",
           "active", "rebuild_us", "select_us", "stream_ms", "app_per_s", "mem_r",
           "attn_x", "full_MiB");
    printf("------- ------- -------- ---------- ---------- ---------- ---------- "
           "-------- ---------- ------------\n");

    t_all0 = now_s();
    json = fopen("logs/kv_stream_bench.jsonl", "w");
    for (ci = 0; ci < (int)(sizeof ctxs / sizeof ctxs[0]); ++ci) {
        for (bi = 0; bi < (int)(sizeof budgets / sizeof budgets[0]); ++bi) {
            int ctx = ctxs[ci];
            int bud = budgets[bi];
            if (bud > ctx) continue;
            if (bench_one(ctx, bud, k_slot, v_slot, &R) != 0) {
                printf("FAIL ctx=%d budget=%d\n", ctx, bud);
                fails++;
                continue;
            }
            print_row(&R);
            if (json) {
                fprintf(json,
                        "{\"ctx\":%d,\"budget\":%d,\"active\":%d,"
                        "\"rebuild_us\":%.4f,\"select_us\":%.4f,\"stream_ms\":%.4f,"
                        "\"app_per_s\":%.1f,\"mem_ratio\":%.6f,\"attn_x\":%.3f,"
                        "\"full_MiB\":%.4f,\"idx_MiB\":%.4f}\n",
                        R.ctx, R.budget, R.active_n, R.rebuild_s * 1e6,
                        R.select_once_s * 1e6, R.stream_s * 1e3, R.appends_per_s,
                        (double)R.mem_ratio, R.speedup_flops,
                        (double)R.bytes_full / (1024.0 * 1024.0),
                        (double)R.bytes_idx / (1024.0 * 1024.0));
            }
            if (R.active_n > bud || R.mem_ratio > 1.0001f || R.active_n <= 0)
                fails++;
        }
    }
    if (json) fclose(json);
    t_all1 = now_s();

    printf("\n--- legend ---\n");
    printf("rebuild_us  mean index rebuild at full ctx\n");
    printf("select_us   mean one-shot sparse selector\n");
    printf("stream_ms   wall time to append 0..ctx-1 (rebuild each step)\n");
    printf("mem_r       index_KV_bytes / full_KV_bytes\n");
    printf("attn_x      ctx/active  (proxy if attention only touches active)\n");
    printf("full_MiB    dense K+V footprint at ctx\n");

    if (bench_one(4096, 128, k_slot, v_slot, &R) == 0) {
        printf("\n=== spotlight ctx=4096 budget=128 ===\n");
        printf("active=%d  mem_ratio=%.4f  save=%.1f%%  attn_proxy=%.1fx\n",
               R.active_n, (double)R.mem_ratio, (1.0 - (double)R.mem_ratio) * 100.0,
               R.speedup_flops);
        printf("rebuild=%.2f us  select=%.2f us  full_stream=%.2f ms  (%.0f app/s)\n",
               R.rebuild_s * 1e6, R.select_once_s * 1e6, R.stream_s * 1e3,
               R.appends_per_s);
        printf("dense=%.2f MiB  indexed=%.2f MiB\n",
               (double)R.bytes_full / (1024.0 * 1024.0),
               (double)R.bytes_idx / (1024.0 * 1024.0));
    }

    printf("\nwall_total=%.3fs failures=%d\n", t_all1 - t_all0, fails);
    printf("log → logs/kv_stream_bench.jsonl\n");
    if (fails) {
        printf("KV_STREAM_BENCH_FAIL\n");
        return 1;
    }
    printf("KV_STREAM_BENCH_PASS\n");
    return 0;
}
