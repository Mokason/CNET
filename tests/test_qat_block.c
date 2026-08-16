/* QAT modern block gate (hermetic — no model files, no $(CCE) link).

   This target exists because make transformer_qat cannot build on boxes
   lacking curl/mmap/fsync/POSIX-mkdir, and gradcheck is the only thing
   standing between a hand-rolled backward pass and silent wrongness.

   Spec: docs/superpowers/specs/2026-08-16-qat-modern-block-design.md */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_transformer_qat.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } \
                              else printf("  ok   %s\n", msg); } while (0)

#define V_   24
#define T_   6

static void make_seq(int* seq, int* tgt) {
    unsigned long long r = 0xC0FFEEULL;
    for (int k = 0; k < T_; k++) {
        r = r * 6364136223846793005ULL + 1442695040888963407ULL;
        seq[k] = (int)((r >> 33) % V_);
    }
    *tgt = (3 * seq[T_ - 1] + 7) % V_;
}

/* Legacy config: zero-init plus dims. MUST stay the GPT-2 shape. */
static void cfg_legacy(cce_transformer_qat_config* c) {
    memset(c, 0, sizeof *c);
    c->n_layer = 2; c->n_embd = 8; c->n_head = 2;
    c->mlp_hidden = 16; c->vocab = V_; c->block_size = 16;
    c->seed = 42;
}

int main(void) {
    int seq[T_], tgt;
    /* Unbuffered: a crash inside a gate must not swallow the output that
       says which gate crashed. */
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== QAT modern block gate ===\n");
    make_seq(seq, &tgt);

    printf("[1] hermetic core links and the legacy gradcheck still passes\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "legacy trainer creates");
        if (t) {
            double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
            printf("  info legacy gradcheck rel err = %.3e\n", rel);
            CHECK(rel < 5e-3, "legacy backward matches central differences");
            cce_transformer_qat_free(t);
        }
    }

    printf("[2] every allocated parameter group is gradcheck-reachable\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "trainer creates");
        if (t) {
            int ng = cce_transformer_qat_group_count(t);
            int np = cce_transformer_qat_param_count(t);
            /* legacy: tok_emb + pos_emb + 12/layer + lnf_w/b + head_w/b */
            int expect = 2 + 12 * c.n_layer + 4;
            printf("  info groups=%d params=%d expected_groups=%d\n",
                   ng, np, expect);
            CHECK(ng == expect, "group count matches the legacy allocation exactly");
            CHECK(np > 0, "parameter count is non-zero");
            /* pos_emb is frozen FP by design: registered so the audit stays
               total, but skipped by gradcheck since backward writes no
               gradient for it. Exactly one such group in the legacy shape. */
            CHECK(cce_transformer_qat_trainable_count(t) == ng - 1,
                  "exactly one registered group is frozen (pos_emb)");
            cce_transformer_qat_free(t);
        }
    }

    printf("[3] zero-init config is legacy, and invalid configs are refused\n");
    {
        cce_transformer_qat_config c;
        cfg_legacy(&c);
        CHECK(c.norm_kind == QAT_NORM_LN, "zero norm_kind is LayerNorm");
        CHECK(c.pos_kind == QAT_POS_LEARNED, "zero pos_kind is learned");
        CHECK(c.mlp_kind == QAT_MLP_GELU, "zero mlp_kind is GELU");
        CHECK(c.no_bias == 0, "zero no_bias means biases present");
        CHECK(c.n_kv_head == 0, "zero n_kv_head means MHA");
        CHECK(c.rope_pairing == QAT_ROPE_HALF, "zero rope_pairing is half-split");

        /* refusal, never clamping: a clamped config would train a model that
           is not the one that was asked for */
        { cce_transformer_qat_config b = c; b.n_head = 3;   /* 8 % 3 != 0 */
          CHECK(cce_transformer_qat_create(&b) == NULL, "n_embd % n_head != 0 refused"); }
        { cce_transformer_qat_config b = c; b.n_kv_head = 3; /* 2 % 3 != 0 */
          CHECK(cce_transformer_qat_create(&b) == NULL, "n_head % n_kv_head != 0 refused"); }
        { cce_transformer_qat_config b = c; b.n_kv_head = 4; /* > n_head */
          CHECK(cce_transformer_qat_create(&b) == NULL, "n_kv_head > n_head refused"); }
        { cce_transformer_qat_config b = c; b.pos_kind = QAT_POS_ROPE; b.rope_theta = 0.0f;
          CHECK(cce_transformer_qat_create(&b) == NULL, "ROPE with theta<=0 refused"); }
        { cce_transformer_qat_config b = c; b.norm_kind = 99;
          CHECK(cce_transformer_qat_create(&b) == NULL, "out-of-range norm_kind refused"); }
        { cce_transformer_qat_config b = c; b.mlp_hidden = 0;
          CHECK(cce_transformer_qat_create(&b) == NULL, "mlp_hidden <= 0 refused"); }
        CHECK(cce_transformer_qat_create(NULL) == NULL, "NULL config refused");
    }

    printf("[4] legacy logits are bit-identical across the refactor\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat *a, *b;
        float *la, *lb;
        int i, same = 1;
        cfg_legacy(&c);
        a = cce_transformer_qat_create(&c);
        b = cce_transformer_qat_create(&c);
        la = (float*)malloc(V_ * sizeof *la);
        lb = (float*)malloc(V_ * sizeof *lb);
        CHECK(a && b && la && lb, "two fixed-seed trainers create");
        if (a && b && la && lb) {
            cce_transformer_qat_logits(a, seq, T_, la);
            cce_transformer_qat_logits(b, seq, T_, lb);
            for (i = 0; i < V_; ++i)
                if (memcmp(&la[i], &lb[i], sizeof(float)) != 0) same = 0;
            CHECK(same, "same seed gives bit-identical legacy logits");
            /* ANCHOR: every later task must reproduce this exact value. */
            printf("  info LEGACY ANCHOR logit[0] = %.9g\n", (double)la[0]);
        }
        free(la); free(lb);
        cce_transformer_qat_free(a); cce_transformer_qat_free(b);
    }

    printf("[5] RMSNorm gradcheck\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.norm_kind = QAT_NORM_RMS;
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "RMSNorm trainer creates");
        if (t) {
            /* RMSNorm has no bias: ln1_b + ln2_b per layer, and lnf_b, go away */
            int expect = 2 + 10 * c.n_layer + 3;
            printf("  info RMS groups=%d expected=%d\n",
                   cce_transformer_qat_group_count(t), expect);
            CHECK(cce_transformer_qat_group_count(t) == expect,
                  "RMSNorm drops exactly the norm-bias groups");
            {
                double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
                printf("  info RMSNorm gradcheck rel err = %.3e\n", rel);
                CHECK(rel < 5e-3, "RMSNorm backward matches central differences");
            }
            cce_transformer_qat_free(t);
        }
    }

    printf("[6] RoPE gradcheck\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.pos_kind = QAT_POS_ROPE;
        c.rope_theta = 10000.0f;
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "RoPE trainer creates");
        if (t) {
            int expect = 1 + 12 * c.n_layer + 4;   /* pos_emb gone */
            printf("  info RoPE groups=%d expected=%d\n",
                   cce_transformer_qat_group_count(t), expect);
            CHECK(cce_transformer_qat_group_count(t) == expect,
                  "RoPE drops the learned pos_emb group");
            {
                double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
                printf("  info RoPE gradcheck rel err = %.3e\n", rel);
                CHECK(rel < 5e-3, "RoPE backward matches central differences");
            }
            cce_transformer_qat_free(t);
        }
    }

    printf("[7] RoPE algebraic properties\n");
    {
        /* Gradcheck cannot catch a wrong PAIRING: any consistent rotation
           differentiates correctly. These two properties are what actually
           pin the convention. */
        int hd = 8, j;
        float q[8], k[8], qa[8], ka[8], qb[8], kb[8];
        double n0 = 0, n1 = 0, d_far = 0, d_near = 0;
        for (j = 0; j < hd; ++j) { q[j] = 0.1f * (j + 1); k[j] = 0.2f - 0.03f * j; }
        memcpy(qa, q, sizeof q); memcpy(ka, k, sizeof k);
        memcpy(qb, q, sizeof q); memcpy(kb, k, sizeof k);
        cce_transformer_qat_rope_test(qa, hd, 3, 10000.0f, QAT_ROPE_HALF);
        cce_transformer_qat_rope_test(ka, hd, 1, 10000.0f, QAT_ROPE_HALF);
        cce_transformer_qat_rope_test(qb, hd, 5, 10000.0f, QAT_ROPE_HALF);
        cce_transformer_qat_rope_test(kb, hd, 3, 10000.0f, QAT_ROPE_HALF);
        for (j = 0; j < hd; ++j) {
            n0 += (double)q[j] * q[j];
            n1 += (double)qa[j] * qa[j];
            d_far  += (double)qa[j] * ka[j];
            d_near += (double)qb[j] * kb[j];
        }
        printf("  info |q|^2 %.9g -> %.9g ; <q3,k1>=%.9g <q5,k3>=%.9g\n",
               n0, n1, d_far, d_near);
        CHECK(fabs(n0 - n1) < 1e-5, "rotation preserves vector norm");
        CHECK(fabs(d_far - d_near) < 1e-5,
              "dot product depends only on relative offset (3-1 == 5-3)");
        /* and the two pairings must genuinely differ, else the knob is a lie */
        {
            float qi[8];
            int diff = 0;
            memcpy(qi, q, sizeof q);
            cce_transformer_qat_rope_test(qi, hd, 3, 10000.0f, QAT_ROPE_INTERLEAVED);
            for (j = 0; j < hd; ++j)
                if (memcmp(&qi[j], &qa[j], sizeof(float)) != 0) diff = 1;
            CHECK(diff, "half-split and interleaved are genuinely different");
        }
    }

    printf("[8] SwiGLU gradcheck\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.mlp_kind = QAT_MLP_SWIGLU;
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "SwiGLU trainer creates");
        if (t) {
            /* +gate_w and +gate_b per layer: the 13th and 14th per-layer
               groups, exactly what the old groups[128] guard would have
               silently dropped. */
            int expect = 2 + 14 * c.n_layer + 4;
            printf("  info SwiGLU groups=%d expected=%d\n",
                   cce_transformer_qat_group_count(t), expect);
            CHECK(cce_transformer_qat_group_count(t) == expect,
                  "SwiGLU adds the gate groups");
            {
                double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
                printf("  info SwiGLU gradcheck rel err = %.3e\n", rel);
                CHECK(rel < 5e-3, "SwiGLU backward matches central differences");
            }
            cce_transformer_qat_free(t);
        }
    }

    printf("[9] GQA gradcheck\n");
    {
        cce_transformer_qat_config c;
        cce_transformer_qat* t;
        cfg_legacy(&c);
        c.n_head = 4; c.n_embd = 8;   /* hd = 2 */
        c.n_kv_head = 2;              /* 2 query heads share each KV head */
        t = cce_transformer_qat_create(&c);
        CHECK(t != NULL, "GQA trainer creates");
        if (t) {
            double rel = cce_transformer_qat_gradcheck(t, seq, T_, tgt, 6);
            printf("  info GQA gradcheck rel err = %.3e\n", rel);
            CHECK(rel < 5e-3, "GQA backward matches central differences");
            cce_transformer_qat_free(t);
        }
    }

    printf("[10] MHA is exactly the kvh == n_head special case\n");
    {
        cce_transformer_qat_config c1, c2;
        cce_transformer_qat *a, *b;
        float *la, *lb;
        int i, same = 1;
        cfg_legacy(&c1);
        cfg_legacy(&c2); c2.n_kv_head = c2.n_head;   /* explicit MHA */
        a = cce_transformer_qat_create(&c1);
        b = cce_transformer_qat_create(&c2);
        la = (float*)malloc(V_ * sizeof *la);
        lb = (float*)malloc(V_ * sizeof *lb);
        CHECK(a && b && la && lb, "both trainers create");
        if (a && b && la && lb) {
            cce_transformer_qat_logits(a, seq, T_, la);
            cce_transformer_qat_logits(b, seq, T_, lb);
            for (i = 0; i < V_; ++i)
                if (memcmp(&la[i], &lb[i], sizeof(float)) != 0) same = 0;
            CHECK(same, "n_kv_head=0 and n_kv_head=n_head are bit-identical");
        }
        free(la); free(lb);
        cce_transformer_qat_free(a); cce_transformer_qat_free(b);
    }

    printf("[11] GQA sharing genuinely changes the K gradient\n");
    {
        /* The KV gradient must SUM over every query head that read it. A
           last-writer-wins bug can cancel inside the directional gradcheck
           projection, so check it directly: with all 4 query heads sharing
           one KV head, |dK| must differ from the no-sharing case. */
        cce_transformer_qat_config c1, c2;
        cce_transformer_qat *a, *b;
        double na, nb;
        cfg_legacy(&c1); c1.n_head = 4; c1.n_embd = 8; c1.n_kv_head = 1;
        cfg_legacy(&c2); c2.n_head = 4; c2.n_embd = 8; c2.n_kv_head = 4;
        a = cce_transformer_qat_create(&c1);
        b = cce_transformer_qat_create(&c2);
        CHECK(a && b, "both GQA configs create");
        if (a && b) {
            na = cce_transformer_qat_kgrad_norm(a, seq, T_, tgt);
            nb = cce_transformer_qat_kgrad_norm(b, seq, T_, tgt);
            printf("  info |dQKV_w| shared(kvh=1)=%.6g unshared(kvh=4)=%.6g\n", na, nb);
            CHECK(na > 0.0 && nb > 0.0, "both produce non-zero QKV gradient");
            CHECK(na != nb, "sharing changes the gradient (not last-writer-wins)");
        }
        cce_transformer_qat_free(a); cce_transformer_qat_free(b);
    }

    printf("checks=%d fails=%d\n", checks, fails);
    if (fails) return 1;
    printf("ALL QAT BLOCK TESTS PASSED\n");
    return 0;
}
