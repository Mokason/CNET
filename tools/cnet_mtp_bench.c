/* Compare baseline vs Forest MTP speculative tok/s.
 * Usage: cnet_mtp_bench [n_tokens] [mtp_k] [draft_layers]
 * Env: CNET_MTP_PARALLEL=0|1 (default 1 — Medusa-lite multi-head draft)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/cce/cce_ds_runtime.h"

static int run_one(int n, int k, int dl, int parallel, double *base_out,
                   double *mtp_out, cce_ds_mtp_stats *tot_out) {
    cce_ds_hparams hp;
    cce_ds_host_opts o;
    cce_ds_host *h = NULL;
    double tps_base = 0, tps_mtp = 0;
    cce_ds_mtp_stats tot;
    char arch[64];

    snprintf(arch, sizeof arch, "mtp_bench_%d.cce", k);
    if (parallel)
        setenv("CNET_MTP_PARALLEL", "1", 1);
    else
        setenv("CNET_MTP_PARALLEL", "0", 1);

    cce_ds_hparams_default_small(&hp);
    hp.n_layer = 4;
    hp.d_model = 256;
    hp.n_heads = 8;
    hp.qk_nope_head_dim = 32;
    hp.qk_rope_head_dim = 16;
    hp.v_head_dim = 32;
    hp.kv_lora_rank = 64;
    hp.n_expert = 8;
    hp.n_expert_used = 2;
    hp.n_ff_exp = 128;
    hp.vocab = 256;

    cce_ds_host_opts_default(&o, arch, NULL);
    o.max_ctx = 512;
    o.dsa_enable = 1;
    o.dsa_fraction = 0.25f;
    o.cold_autoload = 1;
    o.mtp_k = k;
    o.mtp_draft_layers = dl;
    o.ep_places = 2;

    if (cce_ds_host_open(&h, &hp, &o) != CCE_OK || !h) return -1;

    {
        double warm = 0;
        (void)cce_ds_host_bench(h, 8, &warm);
    }
    if (cce_ds_host_bench_mtp(h, n, &tps_mtp, &tot) != CCE_OK) {
        cce_ds_host_close(h);
        remove(arch);
        return -1;
    }
    if (cce_ds_host_bench(h, n, &tps_base) != CCE_OK) {
        cce_ds_host_close(h);
        remove(arch);
        return -1;
    }

    if (base_out) *base_out = tps_base;
    if (mtp_out) *mtp_out = tps_mtp;
    if (tot_out) *tot_out = tot;

    printf("  k=%d par=%d  BASE=%.1f  MTP=%.1f  speedup=%.2fx  "
           "accept=%.2f  main/tok=%.3f  draft_steps=%d\n",
           k, parallel, tps_base, tps_mtp,
           tps_base > 0 ? tps_mtp / tps_base : 0.0,
           tot.drafted > 0 ? (double)tot.accepted / (double)tot.drafted : 0.0,
           tot.accepted > 0 ? (double)tot.main_steps / (double)tot.accepted
                            : 0.0,
           tot.draft_steps);

    cce_ds_host_close(h);
    remove(arch);
    return 0;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 128;
    int k = argc > 2 ? atoi(argv[2]) : 2;
    int dl = argc > 3 ? atoi(argv[3]) : 0;
    int sweep = (argc <= 2); /* default: sweep k=1,2,4 parallel on/off */
    double tps_base = 0, tps_mtp = 0;
    cce_ds_mtp_stats tot;

    if (n < 4) n = 4;
    if (k < 1) k = 1;
    if (k > 8) k = 8;

    printf("== CNET Forest MTP bench (parallel multi-token verify) ==\n");
    printf("  n_tokens=%d draft_layers=%d\n", n, dl);
    printf("  layers=4 d_model=256 experts=8/2 vocab=256\n");
    {
        const char *sl = getenv("CNET_MTP_SIM_LAUNCH");
        printf("  CNET_MTP_PARALLEL=%s  CNET_MTP_SIM_LAUNCH=%s\n",
               getenv("CNET_MTP_PARALLEL") ? getenv("CNET_MTP_PARALLEL") : "1",
               sl && sl[0] ? sl : "0");
        if (sl && atoi(sl) > 0)
            printf("  (sim launch tax ON — models GPU kernel overhead; "
                   "multi-accept amortizes one tax per burst)\n");
    }

    if (sweep) {
        int ks[] = {1, 2, 4};
        int i, p;
        printf("  --- sweep ---\n");
        for (i = 0; i < 3; ++i) {
            for (p = 1; p >= 0; --p) {
                if (run_one(n, ks[i], dl, p, &tps_base, &tps_mtp, &tot) != 0) {
                    fprintf(stderr, "bench failed k=%d par=%d\n", ks[i], p);
                    return 1;
                }
            }
        }
        /* Final gate line uses k=2 parallel (primary path). */
        if (run_one(n, 2, dl, 1, &tps_base, &tps_mtp, &tot) != 0) return 1;
    } else {
        if (run_one(n, k, dl, 1, &tps_base, &tps_mtp, &tot) != 0) {
            fprintf(stderr, "bench failed\n");
            return 1;
        }
    }

    printf("MTP_BENCH_PASS base_tok_s=%.1f mtp_tok_s=%.1f n=%d k=%d\n",
           tps_base, tps_mtp, n, sweep ? 2 : k);
    return 0;
}
