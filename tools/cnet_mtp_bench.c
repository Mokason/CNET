/* Compare baseline vs Forest MTP speculative tok/s.
 * Usage: cnet_mtp_bench [n_tokens] [mtp_k] [draft_layers]
 */
#include <stdio.h>
#include <stdlib.h>
#include "../include/cce/cce_ds_runtime.h"

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 64;
    int k = argc > 2 ? atoi(argv[2]) : 2;
    int dl = argc > 3 ? atoi(argv[3]) : 0;
    cce_ds_hparams hp;
    cce_ds_host_opts o;
    cce_ds_host *h = NULL;
    double tps_base = 0, tps_mtp = 0;
    cce_ds_mtp_stats tot;

    if (n < 4) n = 4;
    if (k < 1) k = 1;
    if (k > 8) k = 8;

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

    cce_ds_host_opts_default(&o, "mtp_bench.cce", NULL);
    o.max_ctx = 256;
    o.dsa_enable = 1;
    o.dsa_fraction = 0.25f;
    o.cold_autoload = 1;
    o.mtp_k = k;
    o.mtp_draft_layers = dl;
    o.ep_places = 2;

    if (cce_ds_host_open(&h, &hp, &o) != CCE_OK || !h) {
        fprintf(stderr, "open failed\n");
        return 1;
    }

    /* Warmup once so neither path gets a cold-cache free lunch. */
    {
        double warm = 0;
        (void)cce_ds_host_bench(h, 8, &warm);
    }

    /* MTP first, then baseline (equal footing after warmup). */
    if (cce_ds_host_bench_mtp(h, n, &tps_mtp, &tot) != CCE_OK) {
        fprintf(stderr, "mtp bench failed\n");
        cce_ds_host_close(h);
        return 1;
    }
    if (cce_ds_host_bench(h, n, &tps_base) != CCE_OK) {
        fprintf(stderr, "baseline bench failed\n");
        cce_ds_host_close(h);
        return 1;
    }

    printf("== CNET Forest MTP bench ==\n");
    printf("  layers=%d d_model=%d experts=%d used=%d vocab=%d\n", hp.n_layer,
           hp.d_model, hp.n_expert, hp.n_expert_used, hp.vocab);
    printf("  n_tokens=%d mtp_k=%d draft_layers=%d ep_places=%d\n", n, k, dl,
           h->ep_places);
    printf("  BASELINE  tok_s=%.1f\n", tps_base);
    printf("  MTP       tok_s=%.1f  speedup=%.2fx\n", tps_mtp,
           tps_base > 0 ? tps_mtp / tps_base : 0.0);
    printf("  MTP stats drafted=%d accepted=%d rejected=%d main_steps=%d "
           "draft_steps=%d\n",
           tot.drafted, tot.accepted, tot.rejected, tot.main_steps,
           tot.draft_steps);
    printf("  accept_rate=%.3f  main_steps_per_tok=%.3f\n",
           tot.drafted > 0 ? (double)tot.accepted / (double)tot.drafted : 0.0,
           tot.accepted > 0 ? (double)tot.main_steps / (double)tot.accepted
                            : 0.0);
    printf("MTP_BENCH_PASS base_tok_s=%.1f mtp_tok_s=%.1f n=%d k=%d\n",
           tps_base, tps_mtp, n, k);

    cce_ds_host_close(h);
    remove("mtp_bench.cce");
    return 0;
}
