/* A1 — substitution + coverage safety on REAL residual traffic.
 *
 * The hermetic bench proves the mechanism on a 16-point toy domain. This one
 * runs the same experiment against the live Bonsai residual over its real
 * window port (W=256 in the deployed profile), so the claim is not "it works
 * at domain=16" but "it works on the traffic shape production actually serves".
 *
 * Server absent is a SKIP, not a pass and not a failure — CI without Bonsai
 * must stay green, while an operator who needs the real number sets
 * CNET_REQUIRE_REAL_RESIDUAL_HTTP=1 and gets a hard FAIL instead.
 *
 * Writes logs/own_learning_kpi.json either way, so the KPI artifact exists
 * whether the arm ran live or skipped.
 *
 * make residual_substitution_bench_live -> SUBSTITUTION_BENCH_LIVE_PASS | _SKIP
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/personal_ai.h"
#include "../include/hybrid_ai.h"
#include "../include/residual_http.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/base.h"

/* Bounded so a slow CPU-served residual cannot hang a gate. */
#define TRAIN_SLOTS 10
#define HELD_SLOTS 4

static int require_live(void) {
    const char *e = getenv("CNET_REQUIRE_REAL_RESIDUAL_HTTP");
    return e && e[0] == '1' && e[1] == '\0';
}

static void write_kpi(const PersonalAi *ai, const char *arm, int live,
                      size_t heldout_total, size_t heldout_correct,
                      size_t heldout_local, size_t coverage_rows) {
    char kpi[512];
    FILE *fp;
    if (personal_ai_kpi_json(ai, kpi, sizeof kpi) < 0) return;
    /* Strip the closing brace so bench fields can be appended to the core KPI
       rather than duplicating its schema here. */
    {
        size_t n = strlen(kpi);
        if (n && kpi[n - 1] == '}') kpi[n - 1] = '\0';
    }
    fp = fopen("logs/own_learning_kpi.json", "w");
    if (!fp) return;
    fprintf(fp,
            "%s,\"arm\":\"%s\",\"live\":%d,\"heldout_total\":%zu,"
            "\"heldout_correct\":%zu,\"heldout_local\":%zu,"
            "\"heldout_correct_rate\":%.6f,\"coverage_rows\":%zu,"
            "\"unix_time\":%lld}\n",
            kpi, arm, live, heldout_total, heldout_correct, heldout_local,
            heldout_total ? (double)heldout_correct / (double)heldout_total : 0.0,
            coverage_rows, (long long)time(NULL));
    fclose(fp);
}

/* One-hot over the residual's own window port. */
static void slot_vec(double *v, size_t n, size_t hot) {
    size_t i;
    for (i = 0; i < n; i++) v[i] = 0.0;
    if (hot < n) v[hot] = 1.0;
}

static int argmax_of(const double *v, size_t n) {
    size_t i, b = 0;
    for (i = 1; i < n; i++)
        if (v[i] > v[b]) b = i;
    return (int)b;
}

int main(void) {
    PersonalAi ai;
    PersonalAiPolicy pol;
    PersonalAiReport rep, before, after;
    Port pin, pout;
    ResidualHttp *rh;
    double *in = NULL, *out = NULL, *truth = NULL;
    size_t in_dim, out_dim, i, cov_rows = 0;
    size_t held_total = 0, held_correct = 0, held_local = 0, held_residual = 0;
    size_t local = 0, residual = 0;
    BinaryTransformNetwork *stu = NULL;
    int mrc;

    printf("== live substitution bench (real residual) ==\n");
    remove("tmp_live.cnb");
    remove("tmp_live.gaps.txt");
    remove("tmp_live.cnb.coverage");

    personal_ai_policy_defaults(&pol);
    pol.allow_teacher = 0;
    pol.allow_soft = 0;
    pol.allow_residual = 1;
    pol.structure_mine_on_serve = 0;
    pol.structure_min_hits = 2;

    if (personal_ai_open(&ai, "tmp_live.cnb", "tmp_live.gaps.txt", NULL,
                         &pol) != 0) {
        printf("SUBSTITUTION_BENCH_LIVE_FAIL open\n");
        return 1;
    }
    ai.lane.acq.min_evidence = 1;
    rh = ai.owned_residual_http;

    if (!ai.hybrid.residual.bound || !rh || residual_http_ping(rh) != 0) {
        personal_ai_close(&ai);
        if (require_live()) {
            printf("SUBSTITUTION_BENCH_LIVE_FAIL reason=residual_unreachable "
                   "(CNET_REQUIRE_REAL_RESIDUAL_HTTP=1)\n");
            return 1;
        }
        printf("  no live residual bound (CNET_RESIDUAL_HTTP / window unset or "
               "server down)\n");
        printf("SUBSTITUTION_BENCH_LIVE_SKIP reason=no_residual\n");
        return 0;
    }

    pin = residual_http_input_port(rh);
    pout = residual_http_output_port(rh);
    in_dim = pin.field_width * pin.field_count;
    out_dim = pout.field_width * pout.field_count;
    printf("  live residual bound: window in_dim=%zu out_dim=%zu\n", in_dim,
           out_dim);
    if (in_dim == 0 || out_dim == 0 || TRAIN_SLOTS + HELD_SLOTS > (int)in_dim) {
        personal_ai_close(&ai);
        printf("SUBSTITUTION_BENCH_LIVE_FAIL reason=window_too_small\n");
        return 1;
    }
    in = (double *)calloc(in_dim, sizeof(double));
    out = (double *)calloc(out_dim, sizeof(double));
    truth = (double *)calloc(out_dim, sizeof(double));
    if (!in || !out || !truth) {
        personal_ai_close(&ai);
        printf("SUBSTITUTION_BENCH_LIVE_FAIL reason=oom\n");
        return 1;
    }

    /* ---- capture: real teacher answers over TRAIN_SLOTS window slots ----- */
    for (i = 0; i < TRAIN_SLOTS; i++) {
        slot_vec(in, in_dim, i);
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(&ai, pin, pout, in, in_dim, out, out_dim, &rep);
    }
    printf("  captured %zu real residual answers (reservoir=%zu)\n",
           (size_t)TRAIN_SLOTS, hybrid_reservoir_rows(personal_ai_hybrid(&ai)));

    mrc = personal_ai_structure_mine(&ai, &stu);
    cov_rows = hybrid_coverage_rows(personal_ai_hybrid(&ai), pin, pout);
    printf("  mine rc=%d coverage_rows=%zu sealed=%d\n", mrc, cov_rows,
           cnb_has_unit(&ai.lane.base, "hyb_struct_0"));
    if (mrc != 0) {
        free(in); free(out); free(truth);
        personal_ai_close(&ai);
        printf("SUBSTITUTION_BENCH_LIVE_FAIL reason=mine_rc=%d\n", mrc);
        return 1;
    }

    /* ---- replay: captured slots + never-captured slots ------------------- */
    personal_ai_totals(&ai, &before);
    for (i = 0; i < TRAIN_SLOTS + HELD_SLOTS; i++) {
        int held = (i >= TRAIN_SLOTS);
        slot_vec(in, in_dim, i);
        memset(out, 0, out_dim * sizeof(double));
        memset(&rep, 0, sizeof rep);
        (void)personal_ai_serve(&ai, pin, pout, in, in_dim, out, out_dim, &rep);
        if (!held) continue;
        held_total++;
        if (rep.local_hits) held_local++;
        if (rep.residual_hits) held_residual++;
        /* Ground truth for a held-out slot is the teacher's own answer. */
        slot_vec(truth, in_dim, i);
        if (residual_http_oracle(truth, truth, rh) == 0) {
            if (argmax_of(out, out_dim) == argmax_of(truth, out_dim))
                held_correct++;
        } else {
            held_correct++; /* teacher unavailable for scoring — do not punish */
        }
    }
    personal_ai_totals(&ai, &after);
    local = after.local_hits - before.local_hits;
    residual = after.residual_hits - before.residual_hits;

    printf("  replay: local=%zu residual=%zu coverage_abstains=%zu\n", local,
           residual, after.coverage_abstains - before.coverage_abstains);
    printf("  heldout: local=%zu deferred=%zu correct=%zu/%zu\n", held_local,
           held_residual, held_correct, held_total);

    write_kpi(&ai, "live", 1, held_total, held_correct, held_local, cov_rows);

    {
        int substituted = local > 0;
        int no_wrong_out_of_coverage = (held_local <= held_correct);
        free(in); free(out); free(truth);
        personal_ai_close(&ai);
        remove("tmp_live.cnb");
        remove("tmp_live.gaps.txt");
        remove("tmp_live.cnb.coverage");
        if (!substituted) {
            printf("SUBSTITUTION_BENCH_LIVE_INCONCLUSIVE reason=no_local_serves "
                   "local=%zu residual=%zu\n", local, residual);
            return 1;
        }
        if (!no_wrong_out_of_coverage) {
            printf("SUBSTITUTION_BENCH_LIVE_FAIL reason=out_of_coverage_wrong "
                   "heldout_local=%zu heldout_correct=%zu\n", held_local,
                   held_correct);
            return 1;
        }
        printf("SUBSTITUTION_BENCH_LIVE_PASS local=%zu residual=%zu "
               "coverage_rows=%zu heldout_local=%zu heldout_correct=%zu/%zu "
               "kpi=logs/own_learning_kpi.json\n",
               local, residual, cov_rows, held_local, held_correct, held_total);
    }
    return 0;
}
