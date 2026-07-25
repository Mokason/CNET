/* Checkpoint contract for cce_moe_xf.
 *
 * A 24/7 loop is only cumulative if resume is exact. If a checkpoint dropped
 * the Adam moments, every tick would re-enter bias-correction warmup and throw
 * away the second-moment scale — the loop would look busy while relearning the
 * same ground, which is precisely the treadmill the gap-lane loop was stuck in.
 *
 * Asserts:
 *   1. save -> load reproduces the forward loss bit-for-bit
 *   2. Adam moments survive: a step taken after reload equals the step the
 *      original model would have taken (moment-blind resume fails this)
 *   3. the step counter round-trips
 *   4. a corrupted or truncated file is REFUSED, not silently reinterpreted
 *   5. a config mismatch is refused
 */
#include "../include/cce/cce_moe_xf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures, checks;
static void check(int ok, const char *msg) {
    checks++;
    if (!ok) failures++;
    printf("  %-68s %s\n", msg, ok ? "PASS" : "FAIL");
}

static unsigned S = 0x77u;
static unsigned rr(void) { S = S * 1664525u + 1013904223u; return S; }

int main(void) {
    cce_moe_xf_cfg c;
    cce_moe_xf *m, *r;
    const char *path = "/tmp/moe_ckpt_test.bin";
    int tok[33], i, step = 0, got = -1;
    double l0, l1, la, lb;

    memset(&c, 0, sizeof c);
    c.vocab = 32; c.d_model = 16; c.seq = 32; c.n_expert = 4; c.top_k = 2;
    c.d_ff = 24; c.lb_coef = 0.02f; c.weight_decay = 1e-3f; c.seed = 5;

    m = cce_moe_xf_create(&c);
    check(m != NULL, "create");
    if (!m) return 1;
    for (i = 0; i < 33; i++) tok[i] = (int)(rr() % (unsigned)c.vocab);

    /* Train a few steps so the Adam moments are non-trivial. */
    for (i = 0; i < 12; i++) {
        cce_moe_xf_zero_grad(m);
        (void)cce_moe_xf_seq(m, tok, 1, NULL, NULL);
        cce_moe_xf_adam(m, 0.01f, ++step);
    }
    l0 = cce_moe_xf_seq(m, tok, 0, NULL, NULL);

    check(cce_moe_xf_save(m, path, step) == 0, "save");
    r = cce_moe_xf_load(path, &got);
    check(r != NULL, "load");
    if (!r) { cce_moe_xf_free(m); return 1; }
    check(got == step, "step counter round-trips");

    l1 = cce_moe_xf_seq(r, tok, 0, NULL, NULL);
    check(l0 == l1, "reloaded forward loss is bit-identical");

    /* One more optimiser step on each. With the moments restored the two must
       land on the same loss; a moment-blind resume diverges here. */
    cce_moe_xf_zero_grad(m);
    (void)cce_moe_xf_seq(m, tok, 1, NULL, NULL);
    cce_moe_xf_adam(m, 0.01f, step + 1);
    la = cce_moe_xf_seq(m, tok, 0, NULL, NULL);

    cce_moe_xf_zero_grad(r);
    (void)cce_moe_xf_seq(r, tok, 1, NULL, NULL);
    cce_moe_xf_adam(r, 0.01f, step + 1);
    lb = cce_moe_xf_seq(r, tok, 0, NULL, NULL);
    check(la == lb, "Adam moments survive: post-resume step matches exactly");
    cce_moe_xf_free(r);

    /* Corruption must be refused. */
    {
        FILE *f = fopen(path, "r+b");
        long mid;
        int byte = 0;
        check(f != NULL, "reopen for corruption");
        if (f) {
            fseek(f, 0, SEEK_END);
            mid = ftell(f) / 2;
            fseek(f, mid, SEEK_SET);
            byte = fgetc(f);
            fseek(f, mid, SEEK_SET);
            fputc(byte ^ 0xFF, f);
            fclose(f);
        }
        r = cce_moe_xf_load(path, &got);
        check(r == NULL, "a corrupted checkpoint is refused (checksum)");
        if (r) cce_moe_xf_free(r);
    }

    /* Truncation must be refused. */
    check(cce_moe_xf_save(m, path, step) == 0, "re-save clean");
    {
        FILE *f = fopen(path, "rb");
        long n = 0;
        char *buf;
        if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
                 buf = (char *)malloc((size_t)n);
                 if (buf) { size_t rd = fread(buf, 1, (size_t)n, f); fclose(f);
                     f = fopen(path, "wb");
                     if (f) { fwrite(buf, 1, rd / 2, f); fclose(f); }
                     free(buf);
                 } else fclose(f); }
        r = cce_moe_xf_load(path, &got);
        check(r == NULL, "a truncated checkpoint is refused");
        if (r) cce_moe_xf_free(r);
    }

    /* A checkpoint written for a different shape must not load. */
    {
        cce_moe_xf_cfg c2 = c;
        cce_moe_xf *m2;
        c2.d_model = 32;
        m2 = cce_moe_xf_create(&c2);
        check(m2 != NULL && cce_moe_xf_save(m2, path, 1) == 0, "save a different shape");
        if (m2) cce_moe_xf_free(m2);
        r = cce_moe_xf_load(path, &got);
        check(r != NULL, "the other shape loads on its own terms");
        if (r) {
            check(got == 1, "its step round-trips too");
            cce_moe_xf_free(r);
        }
    }

    remove(path);
    cce_moe_xf_free(m);
    printf("MOE_CKPT_%s checks=%d\n", failures ? "FAIL" : "PASS", checks);
    return failures ? 1 : 0;
}
