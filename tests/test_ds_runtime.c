/* Full DS stack: map → forest → MLA+MoE forward → DSA + cold experts → bench
 * make ds_stack → DS_STACK_PASS
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_ds_runtime.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    cce_ds_hparams hp;
    cce_ds_host_opts opts;
    cce_ds_host* h = NULL;
    double tok_s = 0;
    int i;

    printf("== CNET DS runtime stack (isolated) ==\n");

    cce_ds_hparams_default_small(&hp);
    hp.n_layer = 2;
    hp.d_model = 128;
    hp.n_heads = 4;
    hp.qk_nope_head_dim = 16;
    hp.qk_rope_head_dim = 8;
    hp.v_head_dim = 16;
    hp.kv_lora_rank = 32;
    hp.n_expert = 4;
    hp.n_expert_used = 2;
    hp.n_ff_exp = 64;
    hp.vocab = 256;

    cce_ds_host_opts_default(&opts, "ds_stack_test.cce", NULL);
    opts.synthetic = 1;
    opts.max_ctx = 64;
    opts.dsa_enable = 1;
    opts.dsa_fraction = 0.25f;
    opts.cold_autoload = 1;
    opts.bind_cold = 0;

    check(cce_ds_host_open(&h, &hp, &opts) == CCE_OK && h, "host open synthetic");
    if (!h) return 1;

    check(h->forest && h->forest->num_branches > 0, "forest bound");
    check(h->mla && h->mla[0].cache.c_kv != NULL, "MLA layer0 ready");

    /* seed residual */
    for (i = 0; i < h->d_model; ++i)
        h->residual[i] = 0.02f * sinf(0.07f * (float)i);
    check(cce_ds_host_forward_token(h) == CCE_OK, "forward token 0");
    check(h->pos == 1, "pos advanced");
    {
        int finite = 1;
        double amax = 0;
        for (i = 0; i < h->d_model; ++i) {
            if (!isfinite(h->residual[i])) finite = 0;
            if (fabs((double)h->residual[i]) > amax)
                amax = fabs((double)h->residual[i]);
        }
        check(finite && amax > 0, "residual finite non-zero");
    }

    /* more tokens with DSA */
    check(cce_ds_host_forward_token(h) == CCE_OK, "forward token 1");
    check(cce_ds_host_forward_token(h) == CCE_OK, "forward token 2");
    check(h->experts_loaded >= 0, "cold autoload counter live");
    printf("    experts_loaded=%d dsa_support_sum=%d\n",
           h->experts_loaded, h->dsa_support_sum);

    /* ensure_expert explicit */
    {
        int before = h->experts_loaded;
        check(cce_ds_host_ensure_expert(h, 0, 3) == CCE_OK, "ensure expert 0/3");
        check(h->experts_loaded >= before, "expert load counted");
        check(cce_forest_get_resident(h->forest, "L00.ffn.e003.g") != NULL,
              "cold expert now resident");
    }

    /* pack path: write pack from map synthetic, reopen host from pack */
    {
        cce_ds_host* h2 = NULL;
        cce_ds_host_opts o2;
        check(cce_ds_pack_write("ds_stack.cnetpack", &h->map, NULL, NULL) == CCE_OK,
              "write pack");
        cce_ds_host_opts_default(&o2, "ds_stack_pack.cce", "ds_stack.cnetpack");
        o2.synthetic = 0;
        o2.max_ctx = 32;
        o2.dsa_enable = 1;
        o2.cold_autoload = 1;
        check(cce_ds_host_open(&h2, &hp, &o2) == CCE_OK && h2, "host from pack");
        if (h2) {
            for (i = 0; i < h2->d_model; ++i) h2->residual[i] = 0.01f * (float)i;
            check(cce_ds_host_forward_token(h2) == CCE_OK, "pack host forward");
            cce_ds_host_close(h2);
        }
        remove("ds_stack.cnetpack");
        remove("ds_stack_pack.cce");
    }

    /* microbench */
    check(cce_ds_host_bench(h, 32, &tok_s) == CCE_OK, "bench 32 tokens");
    check(tok_s > 0, "tok/s > 0");
    printf("    bench: %.1f tok/s (32 tokens, synthetic 2L MLA+MoE+DSA)\n", tok_s);
    printf("    dsa_avg_support=%.1f\n",
           h->tokens_fwd > 0
               ? (double)h->dsa_support_sum / (double)(h->tokens_fwd * h->n_layer)
               : 0.0);

    cce_ds_host_close(h);
    remove("ds_stack_test.cce");

    printf("DS_STACK_PASS checks=%d failures=%d tok_s=%.2f\n",
           checks, failures, tok_s);
    return failures ? 1 : 0;
}
