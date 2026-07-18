/* Forest-native MTP speculative path — make mtp_spec → MTP_SPEC_PASS */
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
    cce_ds_host *h = NULL;
    cce_ds_mtp_stats st, tot;
    double tps_base = 0, tps_mtp = 0;
    int i;

    printf("== CNET Forest MTP speculative ==\n");

    cce_ds_hparams_default_small(&hp);
    hp.n_layer = 4;
    hp.d_model = 128;
    hp.n_heads = 4;
    hp.qk_nope_head_dim = 16;
    hp.qk_rope_head_dim = 8;
    hp.v_head_dim = 16;
    hp.kv_lora_rank = 32;
    hp.n_expert = 4;
    hp.n_expert_used = 2;
    hp.n_ff_exp = 64;
    hp.vocab = 128;

    cce_ds_host_opts_default(&opts, "mtp_spec_test.cce", NULL);
    opts.synthetic = 1;
    opts.max_ctx = 128;
    opts.dsa_enable = 1;
    opts.dsa_fraction = 0.5f;
    opts.cold_autoload = 1;
    opts.mtp_k = 2;
    opts.mtp_draft_layers = 0; /* linear draft — cheap */
    opts.ep_places = 2;

    check(cce_ds_host_open(&h, &hp, &opts) == CCE_OK && h, "host open");
    if (!h) return 1;

    check(cce_ds_host_enable_mtp(h, 2, 0) == CCE_OK, "enable MTP k=2");
    check(h->mtp_ready && h->mtp_head_w && h->mtp_draft_w, "MTP leaves armed");
    check(h->expert_place && h->ep_places == 2, "EP place tags");
    check(h->expert_place[0] == 0 && h->expert_place[1] == 1, "EP round-robin");

    for (i = 0; i < h->d_model; ++i)
        h->residual[i] = 0.02f * sinf(0.05f * (float)i);

    memset(&st, 0, sizeof st);
    check(cce_ds_host_forward_spec(h, 2, &st) == CCE_OK, "spec step");
    check(st.drafted >= 1, "drafted >= 1");
    check(st.accepted >= 1, "accepted >= 1");
    check(st.main_steps >= 1, "main_steps >= 1");
    printf("    step: drafted=%d accepted=%d rejected=%d main=%d draft=%d\n",
           st.drafted, st.accepted, st.rejected, st.main_steps, st.draft_steps);

    /* baseline vs MTP wall clock */
    check(cce_ds_host_bench(h, 32, &tps_base) == CCE_OK, "baseline bench 32");
    check(tps_base > 0, "baseline tok/s > 0");
    check(cce_ds_host_bench_mtp(h, 32, &tps_mtp, &tot) == CCE_OK,
          "MTP bench 32");
    check(tps_mtp > 0, "MTP tok/s > 0");
    check(tot.accepted >= 16, "MTP accepted enough tokens");
    printf("    baseline tok_s=%.1f  mtp tok_s=%.1f  accept_rate=%.2f "
           "main_steps=%d drafted=%d\n",
           tps_base, tps_mtp,
           tot.drafted > 0 ? (double)tot.accepted / (double)tot.drafted : 0.0,
           tot.main_steps, tot.drafted);
    printf("    EP places=%d  experts_fired=%d\n", h->ep_places,
           h->experts_fired);

    cce_ds_host_close(h);
    remove("mtp_spec_test.cce");

    printf("MTP_SPEC_PASS checks=%d failures=%d base_tok_s=%.1f mtp_tok_s=%.1f\n",
           checks, failures, tps_base, tps_mtp);
    return failures ? 1 : 0;
}
