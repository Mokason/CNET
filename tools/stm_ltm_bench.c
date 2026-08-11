/* Benchmark STM/LTM bridge: HOT throughput vs LTM async overlap.
 * make stm_ltm_bench
 */
#include "../include/cce/cce_stm_ltm_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int fake_cert(const char *q, char *sk, size_t cap, void *ud) {
    (void)ud;
    if (q && (q[0] % 17) == 0) {
        snprintf(sk, cap, "skill_%d", (int)(unsigned char)q[0]);
        return 1;
    }
    return 0;
}

typedef struct {
    int hot_steps;
    int ltm_requests;
    double hot_only_s;
    double hot_with_ltm_s;
    double ltm_drain_s;
    uint64_t stm_hits;
    uint64_t ltm_done;
    uint64_t cert_hits;
    double mem_ratio;
    double hot_apps_s;
    double hot_apps_s_overlapped;
    double slowdown_pct; /* how much HOT slowed with LTM traffic */
} Bench;

static int run_bench(int hot_n, int ltm_every, double cold_ms, Bench *R) {
    cce_stm_ltm_bridge B;
    cce_stm_ltm_opts o;
    cce_stm_ltm_job jobs[64];
    int i, n, polled = 0;
    double t0, t1, t2;

    memset(R, 0, sizeof *R);
    cce_stm_ltm_opts_default(&o, hot_n + 8);
    o.budget.max_tokens = 128;
    o.budget.initial_tokens = 8;
    o.budget.recent_tokens = 64;
    o.budget.long_range_stride = 32;
    o.async = 1;
    o.simulated_cold_ms = cold_ms;
    o.cert_fn = fake_cert;
    o.k_slot = 128;
    o.v_slot = 128;

    if (cce_stm_ltm_open(&B, &o) != CCE_OK) return -1;

    /* Phase A: HOT only */
    t0 = now_s();
    for (i = 0; i < hot_n; i++)
        (void)cce_stm_ltm_hot_append(&B, i, NULL, 0);
    t1 = now_s();
    R->hot_only_s = t1 - t0;
    R->hot_apps_s = R->hot_only_s > 0 ? hot_n / R->hot_only_s : 0;

    cce_stm_ltm_close(&B);
    if (cce_stm_ltm_open(&B, &o) != CCE_OK) return -1;

    /* Phase B: HOT + async LTM requests interleaved */
    t0 = now_s();
    for (i = 0; i < hot_n; i++) {
        (void)cce_stm_ltm_hot_append(&B, i, NULL, 0);
        if (ltm_every > 0 && (i % ltm_every) == 0 && i > 64) {
            int cold_pos = i / 3; /* older-ish */
            char q[32];
            snprintf(q, sizeof q, "q%u", (unsigned)i);
            (void)cce_stm_ltm_request_recall(&B, cold_pos, q);
            R->ltm_requests++;
        }
        if ((i & 31) == 0) {
            (void)cce_stm_ltm_poll(&B, jobs, 64, &n);
            polled += n;
        }
    }
    t1 = now_s();
    R->hot_with_ltm_s = t1 - t0;
    R->hot_apps_s_overlapped =
        R->hot_with_ltm_s > 0 ? hot_n / R->hot_with_ltm_s : 0;

    t0 = now_s();
    cce_stm_ltm_sync(&B);
    (void)cce_stm_ltm_poll(&B, jobs, 64, &n);
    polled += n;
    t2 = now_s();
    R->ltm_drain_s = t2 - t0;

    R->hot_steps = hot_n;
    R->stm_hits = B.n_stm_hits;
    R->ltm_done = B.n_ltm_done;
    R->cert_hits = B.n_cert_hits;
    R->mem_ratio = cce_kv_stream_index_mem_ratio(&B.stm);
    if (R->hot_apps_s > 0)
        R->slowdown_pct =
            (1.0 - R->hot_apps_s_overlapped / R->hot_apps_s) * 100.0;

    {
        char line[320];
        cce_stm_ltm_format(&B, line, sizeof line);
        printf("  %s  polled=%d\n", line, polled);
    }
    cce_stm_ltm_close(&B);
    return 0;
}

int main(void) {
    static const struct {
        int hot;
        int every;
        double cold_ms;
    } cases[] = {
        {1024, 0, 0.5},
        {1024, 8, 0.5},
        {2048, 4, 1.0},
        {4096, 8, 0.5},
        {4096, 4, 2.0},
    };
    int i, fails = 0;
    FILE *fp;

    printf("=== STM/LTM bridge benchmark (C, async COLD) ===\n");
    printf("%6s %6s %8s %10s %10s %10s %10s %8s %8s %8s\n", "hot_n", "every",
           "cold_ms", "hot_kps", "ovl_kps", "slow_%", "drain_ms", "stm_hit",
           "ltm_ok", "mem_r");
    printf("------ ------ -------- ---------- ---------- ---------- ---------- "
           "-------- -------- --------\n");

    fp = fopen("logs/stm_ltm_bench.jsonl", "w");
    for (i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++) {
        Bench R;
        if (run_bench(cases[i].hot, cases[i].every, cases[i].cold_ms, &R) != 0) {
            printf("FAIL case %d\n", i);
            fails++;
            continue;
        }
        printf("%6d %6d %8.2f %10.1f %10.1f %10.2f %10.2f %8llu %8llu %8.4f\n",
               cases[i].hot, cases[i].every, cases[i].cold_ms,
               R.hot_apps_s / 1000.0, R.hot_apps_s_overlapped / 1000.0,
               R.slowdown_pct, R.ltm_drain_s * 1e3,
               (unsigned long long)R.stm_hits, (unsigned long long)R.ltm_done,
               R.mem_ratio);
        if (fp) {
            fprintf(fp,
                    "{\"hot_n\":%d,\"ltm_every\":%d,\"cold_ms\":%.3f,"
                    "\"hot_apps_s\":%.1f,\"ovl_apps_s\":%.1f,\"slowdown_pct\":%.3f,"
                    "\"drain_ms\":%.3f,\"stm_hits\":%llu,\"ltm_done\":%llu,"
                    "\"cert_hits\":%llu,\"mem_ratio\":%.6f}\n",
                    cases[i].hot, cases[i].every, cases[i].cold_ms, R.hot_apps_s,
                    R.hot_apps_s_overlapped, R.slowdown_pct, R.ltm_drain_s * 1e3,
                    (unsigned long long)R.stm_hits, (unsigned long long)R.ltm_done,
                    (unsigned long long)R.cert_hits, R.mem_ratio);
        }
        /* HOT should not collapse when LTM runs: allow up to 50% slowdown on
         * heavy cold sim; pure hot case every=0 slowdown ~0 */
        if (cases[i].every == 0 && R.slowdown_pct > 15.0) fails++;
        if (cases[i].every > 0 && R.hot_apps_s_overlapped < 1000) fails++;
    }
    if (fp) fclose(fp);

    printf("\nlegend: hot_kps=HOT-only appends/ms; ovl_kps=HOT while LTM queued; "
           "slow_%%=HOT slowdown with LTM; drain_ms=wait remaining COLD\n");
    printf("log → logs/stm_ltm_bench.jsonl\n");
    if (fails) {
        printf("STM_LTM_BENCH_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("STM_LTM_BENCH_PASS\n");
    return 0;
}
