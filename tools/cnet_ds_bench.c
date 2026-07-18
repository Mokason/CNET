/* Microbench for isolated DS stack. Usage: cnet_ds_bench [n_tokens] */
#include <stdio.h>
#include <stdlib.h>
#include "../include/cce/cce_ds_runtime.h"

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 128;
    cce_ds_hparams hp;
    cce_ds_host_opts o;
    cce_ds_host* h = NULL;
    double tps = 0;
    if (n < 1) n = 1;

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

    cce_ds_host_opts_default(&o, "ds_bench.cce", NULL);
    o.max_ctx = 256;
    o.dsa_enable = 1;
    o.dsa_fraction = 0.25f;
    o.cold_autoload = 1;

    if (cce_ds_host_open(&h, &hp, &o) != CCE_OK || !h) {
        fprintf(stderr, "open failed\n");
        return 1;
    }
    if (cce_ds_host_bench(h, n, &tps) != CCE_OK) {
        fprintf(stderr, "bench failed\n");
        cce_ds_host_close(h);
        return 1;
    }
    printf("DS_BENCH layers=%d d_model=%d heads=%d experts=%d used=%d "
           "tokens=%d tok_s=%.1f experts_loaded=%d fired=%d slept=%d "
           "dsa_support_sum=%d quant_kv=%d\n",
           hp.n_layer, hp.d_model, hp.n_heads, hp.n_expert, hp.n_expert_used,
           n, tps, h->experts_loaded, h->experts_fired, h->experts_slept,
           h->dsa_support_sum, h->mla && h->mla[0].cache.quant_kv);
    cce_ds_host_close(h);
    remove("ds_bench.cce");
    return 0;
}
