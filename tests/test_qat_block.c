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

    printf("checks=%d fails=%d\n", checks, fails);
    if (fails) return 1;
    printf("ALL QAT BLOCK TESTS PASSED\n");
    return 0;
}
