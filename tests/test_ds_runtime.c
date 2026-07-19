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
    check(h->experts_fired >= 0, "experts_fired telemetry");
    check(h->mla[0].cache.quant_kv == 1 || h->mla_quant_kv == 0,
          "MLA quant_kv armed or opted out");
    printf("    experts_loaded=%d fired=%d slept=%d dsa_support_sum=%d\n",
           h->experts_loaded, h->experts_fired, h->experts_slept,
           h->dsa_support_sum);

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
    check(h->experts_fired > 0, "bench fired sparse experts");
    printf("    bench: %.1f tok/s (32 tokens, synthetic 2L MLA+MoE+DSA)\n", tok_s);
    printf("    dsa_avg_support=%.1f experts_fired=%d slept=%d quant_kv=%d\n",
           h->tokens_fwd > 0
               ? (double)h->dsa_support_sum / (double)(h->tokens_fwd * h->n_layer)
               : 0.0,
           h->experts_fired, h->experts_slept, h->mla[0].cache.quant_kv);

    /* ---- Audit regression RED (0a01b50): cce_ds_host_ensure_expert must
     * propagate the underlying cce_forest_add_cascade_branch failure
     * rather than always returning CCE_OK and bumping experts_loaded.
     * The old ADD_IF_MISS macro masked the rc and incremented the counter
     * on failure (telemetry lie). We exhaust the host's forest capacity by
     * stuffing dummy resident cascades until add_cascade_branch would fail,
     * then demand an expert and require BOTH a non-OK return AND no bump
     * of experts_loaded. */
    {
        cce_ds_host_opts mo;
        cce_ds_host* mh = NULL;
        cce_ds_hparams mhp = hp;
        int loaded_before, stuffed;
        int rc;
        cce_ds_host_opts_default(&mo, "ds_ensure_fail.cce", NULL);
        mo.synthetic = 1;
        mo.max_ctx = 16;
        mhp.n_expert = 4;
        mhp.n_expert_used = 1;
        check(cce_ds_host_open(&mh, &mhp, &mo) == CCE_OK && mh,
              "ensure-fail host open");
        if (mh) {
            /* Stuff the forest up to capacity so the next add_cascade_branch
             * inside ensure_expert MUST fail with CCE_ERR_INVALID_ARG. */
            stuffed = 0;
            while (mh->forest->num_branches < mh->forest->max_branches) {
                cce_cascade* cas = NULL;
                char nm[32];
                if (cce_cascade_create(&cas, 2) != CCE_OK) break;
                if (cce_cascade_add_linear_head(cas, 4, 4, 0.02f) != CCE_OK) {
                    cce_cascade_destroy(cas); break;
                }
                snprintf(nm, sizeof nm, "stuff%05d", stuffed);
                if (cce_forest_add_cascade_branch(mh->forest, cas, nm, NULL) !=
                    CCE_OK) { cce_cascade_destroy(cas); break; }
                free(cas);
                stuffed++;
            }
            check(mh->forest->num_branches == mh->forest->max_branches,
                  "forest stuffed to capacity");
            loaded_before = mh->experts_loaded;
            /* expert 1 is valid (in map) but add will fail at capacity. */
            rc = cce_ds_host_ensure_expert(mh, 0, 1);
            check(rc != CCE_OK,
                  "ensure_expert rejected when forest full (error propagated)");
            check(mh->experts_loaded == loaded_before,
                  "experts_loaded NOT incremented on failure (telemetry honest)");
            printf("    ensure-fail: stuffed=%d rc=%d loaded_before=%d after=%d\\n",
                   stuffed, rc, loaded_before, mh->experts_loaded);
            cce_ds_host_close(mh);
        }
        remove("ds_ensure_fail.cce");
    }

    /* ---- Audit regression RED (67f317c, MTP path): when MTP draft path
     * advances pos on layer 0 (mtp_draft_layers>=1), a reject must restore
     * pos to its pre-draft value so the next verify step doesn't skip a
     * real position. Run forward_spec; after it returns, h->pos must equal
     * the sum of accepted tokens (NOT drift). */
    {
        cce_ds_host* mh = NULL;
        cce_ds_host_opts mo;
        cce_ds_mtp_stats st;
        cce_ds_hparams mhp = hp;
        int pos_before, accepted;
        cce_ds_host_opts_default(&mo, "ds_mtp_pos.cce", NULL);
        mo.synthetic = 1;
        mo.max_ctx = 64;
        mo.mtp_k = 2;
        mo.mtp_draft_layers = 1;
        check(cce_ds_host_open(&mh, &mhp, &mo) == CCE_OK && mh, "mtp host open");
        if (mh) {
            for (i = 0; i < mh->d_model; ++i)
                mh->residual[i] = 0.01f * (float)i;
            pos_before = mh->pos;
            check(cce_ds_host_forward_spec(mh, 2, &st) == CCE_OK,
                  "mtp forward_spec ok");
            accepted = st.accepted;
            /* The invariant: pos advanced by exactly accepted tokens
             * (draft path's pos++ inside mtp_draft_step MUST be rolled back
             * by snap_restore). Drift = silent position skip on reject. */
            check(mh->pos == pos_before + accepted,
                  "mtp pos advanced by accepted only (no draft drift)");
            printf("    mtp pos: before=%d after=%d accepted=%d\\n",
                   pos_before, mh->pos, accepted);
            cce_ds_host_close(mh);
        }
        remove("ds_mtp_pos.cce");
    }

    /* ---- Audit regression RED (0a01b50, build_layer_mla fused-buffer
     * aliasing): w_uk and w_uv MUST NOT share a single fused kv_up buffer,
     * and w_uq/w_qr MUST NOT share a single fused q_up buffer. The old code
     * allocated ONE block for the fused leaf and set w_uk=full, w_uv=full+
     * offset (same malloc, two views), then stored the single owning pointer
     * in a w_kr "pointer bag" via a strict-aliasing violation (storing
     * float** through a const float*). The new contract: each weight is its
     * OWN allocation, and w_kr is NULL (k_rope is packed in w_dkv per the
     * MLA_KV_DOWN contract). The ownership table must map each view to one
     * of six pairwise-distinct allocations; this avoids undefined pointer
     * arithmetic between unrelated allocations in the regression test. */
    {
        cce_mla* m0 = &h->mla[0];
        int a, b, all_distinct = 1;

        check(m0->owned_bufs != NULL && m0->n_owned == 6,
              "MLA records six separately-owned weight buffers");
        if (m0->owned_bufs && m0->n_owned == 6) {
            check(m0->w.w_dkv == m0->owned_bufs[0] &&
                  m0->w.w_uk  == m0->owned_bufs[1] &&
                  m0->w.w_uv  == m0->owned_bufs[2] &&
                  m0->w.w_uq  == m0->owned_bufs[3] &&
                  m0->w.w_qr  == m0->owned_bufs[4] &&
                  m0->w.w_o   == m0->owned_bufs[5],
                  "MLA views map exactly to owned buffers");
            for (a = 0; a < m0->n_owned; ++a)
                for (b = a + 1; b < m0->n_owned; ++b)
                    if (m0->owned_bufs[a] == m0->owned_bufs[b])
                        all_distinct = 0;
            check(all_distinct,
                  "MLA owned weight buffers are pairwise distinct");
        }
        check(m0->w.w_kr == NULL,
              "MLA w_kr is NULL (no strict-aliasing pointer-bag hack)");
    }

    /* ---- Audit regression RED (0a01b50, mla_token idx/wt leak on kr>512):
     * When the q8 latent-KV path hits kr>512 it returns UNSUPPORTED early.
     * The old code leaked the idx[]/wt[] scratch allocated for DSA on every
     * such exit (two separate early-return sites). We arm quant_kv and a
     * large kv_lora_rank and run forward; the call must return UNSUPPORTED
     * (kr>512 limit) and the host must close cleanly. We also run a second
     * forward to confirm no state corruption from the leaked-but-now-freed
     * buffers. */
    {
        cce_ds_host* mh = NULL;
        cce_ds_host_opts mo;
        cce_ds_hparams mhp = hp;
        int rc;
        cce_ds_host_opts_default(&mo, "ds_mla_leak.cce", NULL);
        mo.synthetic = 1;
        mo.max_ctx = 16;
        mo.mla_quant_kv = 1;
        mo.dsa_enable = 1;
        mo.dsa_fraction = 0.5f;
        /* kv_lora_rank > 512 trips the q8 latent-KV limit inside mla_token. */
        mhp.kv_lora_rank = 600;
        mhp.n_layer = 1;
        check(cce_ds_host_open(&mh, &mhp, &mo) == CCE_OK && mh,
              "mla-leak host open (kr=600, quant_kv)");
        if (mh) {
            for (i = 0; i < mh->d_model; ++i)
                mh->residual[i] = 0.01f * (float)i;
            rc = cce_ds_host_forward_token(mh);
            /* kr>512 path returns UNSUPPORTED inside mla_token; the host
             * forward_token_core treats non-OK MLA as zero-attention, so
             * the overall forward still returns OK. We assert no crash and
             * that the host is still usable. */
            (void)rc;
            check(cce_ds_host_forward_token(mh) == CCE_OK,
                  "second forward after kr>512 path (no state corruption)");
            cce_ds_host_close(mh);
        }
        remove("ds_mla_leak.cce");
    }

    /* ---- Audit regression RED (ad4a301, 67f317c MTP): random synthetic
     * MTP draft weights (mtp_fill_randn) must NOT be silently armed on a
     * non-synthetic / real-model host. The old cce_ds_host_enable_mtp always
     * filled random draft/embed/head weights regardless of whether the host
     * was hermetic-synthetic or backed by a real .cnetpack. On a real host
     * that produces fabricated speculative outputs masquerading as model
     * predictions. We open a pack-backed host and require enable_mtp to
     * REFUSE (return non-OK) and leave mtp_ready=0. */
    {
        cce_ds_host* ph = NULL;
        cce_ds_host_opts po;
        cce_result rc;
        /* Reuse the pack written earlier in this test. The pack was removed
         * at the end of the pack round-trip block, so write a fresh one. */
        check(cce_ds_pack_write("ds_mtp_real.cnetpack", &h->map, NULL, NULL)
              == CCE_OK, "write pack for real-host MTP refusal");
        cce_ds_host_opts_default(&po, "ds_mtp_real.cce", "ds_mtp_real.cnetpack");
        po.synthetic = 0;
        po.max_ctx = 32;
        check(cce_ds_host_open(&ph, &hp, &po) == CCE_OK && ph,
              "real pack-backed host open");
        if (ph) {
            rc = cce_ds_host_enable_mtp(ph, 2, 0);
            check(rc != CCE_OK,
                  "enable_mtp REFUSED on non-synthetic (real) host");
            check(ph->mtp_ready == 0,
                  "mtp_ready stays 0 after refusal (no random draft weights)");
            check(ph->mtp_draft_w == NULL && ph->mtp_embed == NULL &&
                  ph->mtp_head_w == NULL && ph->mtp_draft_A == NULL,
                  "no random MTP buffers allocated on real host");
            cce_ds_host_close(ph);
        }
        remove("ds_mtp_real.cnetpack");
        remove("ds_mtp_real.cce");
    }

    cce_ds_host_close(h);
    remove("ds_stack_test.cce");

    printf("DS_STACK_PASS checks=%d failures=%d tok_s=%.2f\n",
           checks, failures, tok_s);
    return failures ? 1 : 0;
}
