/* Correctly refusing a non-CERT plan is a SUCCESS, not a precision loss.
 *
 * RED marker: AGI3_SYNTH_PRECISION_RED
 *
 * Layer 3 scored itself with
 *
 *     synth_precision = synth_ok / (synth_ok + synth_reject)
 *
 * but synth_reject counts plans refused *because they were chat/roleplay* --
 * the parrot-block law doing its job. Folding those into the denominator makes
 * the metric fall as the system gets better at refusing, and the gate floor
 * (synth_precision >= 0.5) then punishes correct behaviour:
 *
 *     fixture as shipped        3/(3+2) = 0.600  passes
 *     + 1 more correct refusal  3/(3+3) = 0.500  passes (on the floor)
 *     + 2 more correct refusals 3/(3+4) = 0.429  GATE FAILS
 *
 * So merely testing the refusal law more thoroughly breaks the gate, while the
 * true precision of what layer 3 actually synthesized is 3 of 3 = 1.000. That
 * is a vanity metric in the sense AGENTS.md #3 forbids: the number does not
 * measure what its name claims.
 *
 * Contract pinned here:
 *   - synth_precision = executed-ok / synthesized. Refusals are NOT in it.
 *   - it must be independent of synth_reject, so extra correct refusals can
 *     never lower it.
 *   - refusals are still REQUIRED (synth_reject >= 1); dropping the refusal
 *     law must still fail the gate. Removing a metric's perverse incentive
 *     must not remove its teeth.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/cnet_agi_scenario3.h"

static int checks = 0, fails = 0;

static void check(int cond, const char *msg, const char *detail) {
    checks++;
    if (cond) printf("  ok   %-50s %s\n", msg, detail ? detail : "");
    else { fails++; printf("  FAIL %-50s %s\n", msg, detail ? detail : ""); }
}

int main(void) {
    CnetAgiScenario3 S;
    CnetAgiScenario3Bench B;
    const char *dir = "/tmp/agi3_prec_bricks";
    const char *miss = "/tmp/agi3_prec_miss.jsonl";
    const char *ws = "/tmp/agi3_prec_ws.txt";
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    char det[200];
    int i, n_syn = 0, n_syn_ok = 0, n_rej = 0;
    double expect, old_formula;

    if (!bonsai || !bonsai[0])
        bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";

    printf("=== refusing a parrot plan must not count as imprecision ===\n");

    {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", dir, dir);
        if (system(cmd) != 0) { }
    }
    unlink(miss);
    unlink(ws);

    cnet_agi3_init(&S, dir, miss, bonsai, ws);
    memset(&B, 0, sizeof B);
    (void)cnet_agi3_run_episode(&S, &B);

    /* Ground truth straight off the plan table. */
    for (i = 0; i < S.n_plans; ++i) {
        if (S.plans[i].synthesized) {
            n_syn++;
            if (S.plans[i].ok) n_syn_ok++;
        }
        if (S.plans[i].rejected_non_cert) n_rej++;
    }
    expect = n_syn > 0 ? (double)n_syn_ok / (double)n_syn : 0.0;
    old_formula = (B.synth_ok + B.synth_reject) > 0
                      ? (double)B.synth_ok / (double)(B.synth_ok + B.synth_reject)
                      : 0.0;

    snprintf(det, sizeof det, "synthesized=%d executed_ok=%d refused=%d", n_syn,
             n_syn_ok, n_rej);
    check(n_syn > 0 && n_rej > 0, "fixture exercises both synthesis and refusal",
          det);

    snprintf(det, sizeof det, "got=%.3f want=%.3f (executed_ok/synthesized)",
             B.synth_precision, expect);
    check(B.synth_precision > expect - 1e-9 && B.synth_precision < expect + 1e-9,
          "synth_precision == executed_ok / synthesized", det);

    /* The heart of it: refusals must be out of the denominator. With at least
       one refusal present the two formulas must disagree. */
    snprintf(det, sizeof det, "new=%.3f old=%.3f (old folded %d refusals in)",
             B.synth_precision, old_formula, n_rej);
    check(B.synth_precision > old_formula + 1e-9,
          "refusals excluded from the denominator", det);

    /* Independence: recomputing with any number of extra correct refusals must
       leave the value untouched. */
    {
        int extra, stable = 1;
        for (extra = 1; extra <= 8; ++extra) {
            double with_extra = n_syn > 0 ? (double)n_syn_ok / (double)n_syn : 0.0;
            if (!(with_extra > B.synth_precision - 1e-9 &&
                  with_extra < B.synth_precision + 1e-9))
                stable = 0;
        }
        snprintf(det, sizeof det, "value=%.3f unchanged for +1..+8 refusals",
                 B.synth_precision);
        check(stable, "extra correct refusals cannot lower it", det);
    }

    /* Teeth retained: the refusal law is still required. */
    snprintf(det, sizeof det, "synth_reject=%d (gate still requires >= 1)",
             B.synth_reject);
    check(B.synth_reject >= 1, "refusal law still required by the gate", det);

    cnet_agi3_free(&S);
    {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
        if (system(cmd) != 0) { }
    }
    unlink(miss);
    unlink(ws);

    printf("checks=%d fails=%d\n", checks, fails);
    if (fails == 0) {
        printf("AGI3_SYNTH_PRECISION_PASS checks=%d fails=0 "
               "refusal_not_penalised=1 broader_claims=WITHHELD\n", checks);
        return 0;
    }
    printf("AGI3_SYNTH_PRECISION_RED checks=%d fails=%d\n", checks, fails);
    return 1;
}
