/* Hermetic gate for the counterfactual serving shadow channel
 * (CNET_COUNTERFACTUAL, default OFF).
 *
 * Seals a small certified roster into a real CNB, then serves the IDENTICAL
 * query sequence through soul_route/soul_request with the knob OFF and ON
 * (fresh host each phase, identical evidence histories) and asserts:
 *   1. every served answer and return code is BYTE-IDENTICAL knob-on vs
 *      knob-off — the core authority assertion (evidence may RANK or REPORT,
 *      never change a served answer; docs/dispatch.md);
 *   2. counterfactual report metadata is absent when OFF and present when ON
 *      (soul_counterfactual_last + the stderr telemetry line);
 *   3. refusal semantics are unchanged: unknown goals and novel signatures
 *      refuse with the same codes, the gap-inbox note still fires, and a
 *      refusal never carries counterfactual metadata;
 *   4. roster overflow keeps the served unit reported: with MORE same-shape
 *      certified units than the 32-slot shadow roster sealed AHEAD of the
 *      served unit in registry order, the report still names the served
 *      unit as primary (slot 0 is reserved for it) and never lists it as
 *      an alternative.
 */

#include <stdio.h>
#include "../include/cnet_platform.h"
#include <stdlib.h>
#include <string.h>

#include "../include/soul_host.h"
#include "../include/base.h"
#include "../include/contract/contract.h"
#include "../include/nn.h"

#define CF_GOALS 4

static int failures;

static void check(int ok, const char *name) {
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port port(PortFamily family, size_t width, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = family;
    p.field_width = width;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

/* Seal one certified unit acq_<goal_tag> (cf_input -> goal_tag) into `base`
   with a deterministic bias-driven output and the given exemplar input. */
static int seal_unit(CnetBase *base, const char *goal_tag,
                     const double *exemplar_in, double bias_sign) {
    BinaryTransformNetwork btn;
    Contract contract;
    Port in_port = port(PORT_ONEHOT, 2, "cf_input");
    Port out_port = port(PORT_ONEHOT, 2, goal_tag);
    char unit_name[64];
    double expected[2] = {0.0, 0.0};
    const double *raw;
    int reused = 0, ok = 1;

    memset(&btn, 0, sizeof btn);
    memset(&contract, 0, sizeof contract);
    snprintf(unit_name, sizeof unit_name, "acq_%s", goal_tag);
    if (btn_init(&btn, 2, 2, 2, 2, 0.1, 17u) != 0) return -1;
    btn.output_bias[0] = 10.0 * bias_sign;
    btn.output_bias[1] = -10.0 * bias_sign;
    memset(btn.hidden_output_weights, 0,
           btn.hidden_count * btn.output_count * sizeof(double));
    if (btn_set_ports(&btn, in_port, out_port) != 0) ok = 0;
    raw = ok ? btn_forward(&btn, exemplar_in) : NULL;
    if (!raw || port_canonicalize(out_port, raw, expected) != 0) ok = 0;
    if (ok && contract_init_borrowed(&contract, unit_name, &btn,
                                     exemplar_in, expected, 1) != 0) ok = 0;
    if (ok && btn_certify(&btn, &contract, NULL) != 0) ok = 0;
    if (ok && cnb_add_unit(base, &btn, &contract, &reused) != 0) ok = 0;
    contract_free(&contract);
    btn_free(&btn);
    return ok ? 0 : -1;
}

/* One serving phase: the FIXED query sequence every phase must replay
   identically. Fills rc[0..CF_GOALS+1] and outs[goal][0..1]. */
static void serve_phase(SoulHost *host, const char *goals[CF_GOALS],
                        const double *query, int *rc,
                        double outs[CF_GOALS][2]) {
    double scratch[2] = {0.0, 0.0};
    int g;
    for (g = 0; g < CF_GOALS; ++g) {
        memset(outs[g], 0, 2 * sizeof(double));
        rc[g] = soul_route(host, goals[g], query, 2, outs[g], 2);
    }
    /* refusals, same order in both phases */
    rc[CF_GOALS] = soul_route(host, "cf_goal_none", query, 2, scratch, 2);
    rc[CF_GOALS + 1] = soul_request(host, PORT_ONEHOT, 4, 1, "cf_novel_in",
                                    PORT_ONEHOT, 4, 1, "cf_novel_goal",
                                    NULL, 0, NULL, 0);
}

int main(void) {
    const char *base_path = "tmp_cf_serving.cnb";
    const char *goals[CF_GOALS] = {"cf_goal_alpha", "cf_goal_bravo",
                                   "cf_goal_charlie", "cf_goal_delta"};
    const double ex_a[2] = {1.0, 0.0};
    const double ex_b[2] = {0.0, 1.0};
    const double query[2] = {1.0, 0.0};
    double outs_off[CF_GOALS][2], outs_on[CF_GOALS][2];
    int rc_off[CF_GOALS + 2], rc_on[CF_GOALS + 2];
    char report[512];
    CnetBase base;
    SoulHost *host = NULL;
    int g, ok;

    remove(base_path);
    remove("tmp_cf_serving.cnb.tmp");
    remove("tmp_cf_serving_off.inbox");
    remove("tmp_cf_serving_on.inbox");
    cnet_unsetenv("CNET_COUNTERFACTUAL");

    printf("== counterfactual serving: report-only shadow evidence ==\n");
    cnb_init(&base);
    ok = seal_unit(&base, goals[0], ex_a, 1.0) == 0 &&
         seal_unit(&base, goals[1], ex_b, -1.0) == 0 &&
         seal_unit(&base, goals[2], ex_a, 1.0) == 0 &&
         seal_unit(&base, goals[3], ex_b, -1.0) == 0;
    check(ok, "roster seals four certified units");
    check(cnb_save(&base, base_path) == 0, "base saves atomically");
    cnb_free(&base);

    /* ---- phase OFF: knob unset, the legacy serving path ---- */
    cnet_setenv("CNET_GAP_INBOX", "tmp_cf_serving_off.inbox", 1);
    check(soul_open(base_path, NULL, &host) == 0 && host != NULL,
          "host opens with the knob OFF");
    check(soul_counterfactual_last(host, report, (int)sizeof report) == -2,
          "OFF: counterfactual channel reads disabled");
    serve_phase(host, goals, query, rc_off, outs_off);
    check(soul_counterfactual_last(host, report, (int)sizeof report) == -2,
          "OFF: no metadata after serving");
    soul_close(host);
    host = NULL;
    ok = 1;
    for (g = 0; g < CF_GOALS; ++g)
        if (rc_off[g] != 2) ok = 0;
    check(ok, "OFF: every goal serves (baseline answers captured)");
    check(rc_off[CF_GOALS] == -2 && rc_off[CF_GOALS + 1] == -3,
          "OFF: unknown goal and novel signature refuse");
    {
        FILE *ib = fopen("tmp_cf_serving_off.inbox", "r");
        check(ib != NULL, "OFF: novel signature landed in the gap inbox");
        if (ib) fclose(ib);
    }

    /* ---- phase ON: knob set BEFORE soul_open, identical query replay ---- */
    cnet_setenv("CNET_COUNTERFACTUAL", "1", 1);
    cnet_setenv("CNET_GAP_INBOX", "tmp_cf_serving_on.inbox", 1);
    check(soul_open(base_path, NULL, &host) == 0 && host != NULL,
          "host opens with the knob ON");
    check(soul_counterfactual_last(host, report, (int)sizeof report) == -3,
          "ON: no report before anything is served");
    serve_phase(host, goals, query, rc_on, outs_on);

    /* 1. the core assertion: served answers are byte-identical */
    ok = 1;
    for (g = 0; g < CF_GOALS + 2; ++g)
        if (rc_on[g] != rc_off[g]) ok = 0;
    check(ok, "ON: every return code identical to OFF");
    ok = 1;
    for (g = 0; g < CF_GOALS; ++g)
        if (memcmp(outs_on[g], outs_off[g], 2 * sizeof(double)) != 0) ok = 0;
    check(ok, "ON: every served answer BYTE-IDENTICAL to OFF");

    /* 3. refusal semantics unchanged, and a refusal carries no metadata:
       the last routed query in the replay was the unknown goal (-2). */
    check(soul_counterfactual_last(host, report, (int)sizeof report) == -3,
          "ON: refused route cleared the report (no stale metadata)");
    {
        FILE *ib = fopen("tmp_cf_serving_on.inbox", "r");
        check(ib != NULL, "ON: gap-inbox refusal note still fires");
        if (ib) fclose(ib);
    }

    /* 2. metadata present when ON: re-serve one goal and read the report */
    check(soul_route(host, goals[3], query, 2, outs_on[3], 2) == 2,
          "ON: goal re-serves for the metadata read");
    memset(report, 0, sizeof report);
    check(soul_counterfactual_last(host, report, (int)sizeof report) == 0,
          "ON: served answer carries a counterfactual report");
    check(strstr(report, "goal=cf_goal_delta") != NULL &&
          strstr(report, "unit=acq_cf_goal_delta") != NULL,
          "report names the served goal and unit");
    check(strstr(report, "alternatives=3") != NULL &&
          strstr(report, "alt0=") != NULL &&
          strstr(report, "alt1=") != NULL &&
          strstr(report, "alt2=") != NULL,
          "report ranks three alternative routes");
    check(strstr(report, "=acq_cf_goal_delta:") == NULL,
          "the served unit is excluded from the alternatives");
    {
        const char *p = strstr(report, "consistency=");
        double v = p ? atof(p + 12) : -1.0;
        check(p != NULL && v >= 0.0 && v <= 1.0,
              "consistency score is present and normalized");
    }
    check(soul_counterfactual_last(host, report, 4) == -4,
          "undersized report buffer is refused, never truncated silently");
    soul_close(host);
    host = NULL;

    /* ---- overflow phase (knob still ON): 33 same-shape certified fillers
       sealed AHEAD of the served unit, one more than the 32-slot roster.
       Without the reserved primary slot the roster saturates before ever
       reaching the served unit and the shadow vanishes silently; with it
       the served unit must be primary and never an alternative. Filler
       goal tags use doubled letter+digit suffixes so every pair stays at
       edit distance >= 2 under tag near-miss governance. */
    {
        const char *ovf_path = "tmp_cf_overflow.cnb";
        char goal[32];
        double out[2] = {0.0, 0.0};
        int u;

        remove(ovf_path);
        remove("tmp_cf_overflow.cnb.tmp");
        cnb_init(&base);
        ok = 1;
        for (u = 0; u < 33; ++u) {
            snprintf(goal, sizeof goal, "cf_ovf_%c%c%c%c",
                     'a' + u % 26, 'a' + u % 26, '0' + u / 26, '0' + u / 26);
            if (seal_unit(&base, goal, (u & 1) ? ex_b : ex_a,
                          (u & 1) ? -1.0 : 1.0) != 0) ok = 0;
        }
        ok = ok && seal_unit(&base, "cf_goal_omega", ex_a, 1.0) == 0;
        check(ok, "overflow roster seals 33 fillers, served unit LAST");
        check(cnb_save(&base, ovf_path) == 0, "overflow base saves");
        cnb_free(&base);

        check(soul_open(ovf_path, NULL, &host) == 0 && host != NULL,
              "overflow host opens with the knob ON");
        check(soul_route(host, "cf_goal_omega", query, 2, out, 2) == 2,
              "overflow: unit past roster capacity still serves");
        memset(report, 0, sizeof report);
        check(soul_counterfactual_last(host, report, (int)sizeof report) == 0,
              "overflow: report present despite saturated roster");
        check(strstr(report, "unit=acq_cf_goal_omega") != NULL,
              "overflow: served unit is primary (reserved slot 0)");
        check(strstr(report, "=acq_cf_goal_omega:") == NULL,
              "overflow: served unit never listed as an alternative");
        soul_close(host);
        host = NULL;
        remove(ovf_path);
        remove("tmp_cf_overflow.cnb.tmp");
    }

    cnet_unsetenv("CNET_COUNTERFACTUAL");
    remove(base_path);
    remove("tmp_cf_serving.cnb.tmp");
    remove("tmp_cf_serving_off.inbox");
    remove("tmp_cf_serving_on.inbox");

    printf("COUNTERFACTUAL_SERVING_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
