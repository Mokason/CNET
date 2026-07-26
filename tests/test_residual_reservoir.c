/* Input reservoir gate (M3) — retain real traffic, not one overwritten exemplar.
 *
 * Before: hybrid_trace_residual memcpy'd over tr->in/tr->out on every matching
 * request, so a trace was ONE last-seen pair per port shape. The miner could
 * not train on what users actually asked; it synthesised a one-hot basis and
 * re-labelled it through the residual, and any port family that was not
 * single-field ONEHOT degenerated to a single exemplar.
 *
 * After: each trace keeps up to K DISTINCT real (in,out) pairs, port-family
 * agnostic. The miner prefers those — but never at the cost of coverage: a
 * small one-hot alphabet's synthetic basis spans the whole input domain, so
 * partial traffic must not replace it.
 *
 * make residual_reservoir → RESIDUAL_RESERVOIR_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Multi-field one-hot port: field_count > 1 is the family the old miner could
   not expand, so it is the family the reservoir must rescue. */
static Port PMF(const char *tag, size_t width, size_t count) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = width;
    p.field_count = count;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

typedef struct {
    size_t fw, fc, out_dim;
} ResCtx;

/* Compositional teacher: sum of field symbols, mod out_dim. */
static int res_addmod(const double *in, double *out, void *ctx) {
    ResCtx *c = (ResCtx *)ctx;
    size_t f, i, sum = 0;
    if (!in || !out || !c) return -1;
    for (f = 0; f < c->fc; f++) {
        size_t hot = 0;
        for (i = 1; i < c->fw; i++)
            if (in[f * c->fw + i] > in[f * c->fw + hot]) hot = i;
        sum += hot;
    }
    for (i = 0; i < c->out_dim; i++) out[i] = 0.0;
    out[sum % c->out_dim] = 1.0;
    return 0;
}

static void encode_pair(double *in, size_t fw, size_t a, size_t b) {
    size_t i;
    for (i = 0; i < fw * 2; i++) in[i] = 0.0;
    in[a] = 1.0;
    in[fw + b] = 1.0;
}

int main(void) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep;
    ResCtx ctx;
    Port pin = PMF("rsv_in", 4, 2);   /* in_dim 8, two fields */
    Port pout = PMF("rsv_out", 4, 1); /* out_dim 4 */
    double in[8], out[4];
    const char *base = "tmp_reservoir.cnb";
    const char *led = "tmp_reservoir.gaps.txt";
    const HybridAi *h;
    size_t a, b, rows;

    remove(base);
    remove(led);
    printf("== input reservoir: real traffic retained per port shape ==\n");

    ctx.fw = 4;
    ctx.fc = 2;
    ctx.out_dim = 4;

    /* Small K so the FIFO bound is observable. */
    setenv("CNET_RESIDUAL_RESERVOIR_K", "6", 1);
    unsetenv("CNET_FAULT_LOG"); /* isolate from the capture gate */

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0;
    pol.allow_soft = 0;
    pol.allow_residual = 1;
    pol.structure_mine_on_serve = 0;

    check(personal_ai_open(&ai, base, led, NULL, &pol) == 0, "open");
    check(personal_ai_bind_residual(&ai, "addmod", res_addmod, &ctx) == 0,
          "bind compositional residual");
    h = personal_ai_hybrid(&ai);

    /* --- distinct real inputs are retained, not overwritten --------------- */
    for (a = 0; a < 2; a++) {
        for (b = 0; b < 2; b++) {
            encode_pair(in, 4, a, b);
            memset(&rep, 0, sizeof rep);
            check(personal_ai_serve(&ai, pin, pout, in, 8, out, 4, &rep) == 0,
                  "serve reaches residual (multi-field port)");
        }
    }
    rows = hybrid_reservoir_rows(h);
    check(rows == 4, "4 distinct multi-field inputs retained (was 1 exemplar)");
    check(hybrid_reservoir_rows_for(h, pin, pout) == 4,
          "per-port-shape accessor agrees");

    /* --- repeats refresh, they do not grow the set ------------------------ */
    encode_pair(in, 4, 0, 0);
    memset(&rep, 0, sizeof rep);
    (void)personal_ai_serve(&ai, pin, pout, in, 8, out, 4, &rep);
    check(hybrid_reservoir_rows(h) == 4, "repeat input does not duplicate");

    /* --- FIFO bound at K -------------------------------------------------- */
    for (a = 0; a < 4; a++) {
        for (b = 0; b < 4; b++) {
            encode_pair(in, 4, a, b);
            memset(&rep, 0, sizeof rep);
            (void)personal_ai_serve(&ai, pin, pout, in, 8, out, 4, &rep);
        }
    }
    rows = hybrid_reservoir_rows(h);
    check(rows == 6, "reservoir bounded by K=6 after 16 distinct inputs");

    /* Offered count outruns retained count — the bound is real, not luck. */
    {
        size_t i, offered = 0;
        for (i = 0; i < h->trace_count; i++) offered += h->traces[i].res_offered;
        check(offered > rows, "offered pairs exceed retained (FIFO evicted)");
    }

    /* --- the reservoir holds REAL pairs, correctly labelled by the teacher - */
    {
        size_t i, t, bad = 0, seen = 0;
        for (t = 0; t < h->trace_count; t++) {
            const HybridTrace *tr = &h->traces[t];
            for (i = 0; i < tr->res_count; i++) {
                double expect[4];
                res_addmod(tr->res_in + i * tr->in_dim, expect, &ctx);
                if (memcmp(expect, tr->res_out + i * tr->out_dim,
                           tr->out_dim * sizeof(double)) != 0)
                    bad++;
                seen++;
            }
        }
        check(seen > 0 && bad == 0,
              "every retained pair carries the teacher's real answer");
    }

    personal_ai_close(&ai);
    remove(base);
    remove(led);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("RESIDUAL_RESERVOIR_PASS checks=%d k=6\n", checks);
        return 0;
    }
    printf("RESIDUAL_RESERVOIR_FAIL failures=%d\n", failures);
    return 1;
}
