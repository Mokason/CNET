/* One-dispatch-story gate (docs/dispatch.md).
 *
 * Three layers answer "which specialist runs", and each boundary is
 * machine-checked here:
 *   1  recall (cce_router) dispatches INSIDE a Specialist — a two-branch
 *      model routes per input, yet the planner sees ONE certified unit
 *      whose contract captures the routed composite exactly;
 *   2  certification outranks everything — uncertified candidates are
 *      invisible regardless of evidence, and RESET re-routes;
 *   3  among the certified, learned reliability ranks — flip the
 *      evidence, the plan flips.
 */

#include <stdio.h>
#include <string.h>

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_model.h"

#define SYM 4

static int failures;
static int checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port sym_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT;
    p.field_width = SYM;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static void onehot_row(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

static int argmax4(const double *v) {
    int i, b = 0;
    for (i = 1; i < SYM; i++) if (v[i] > v[b]) b = i;
    return b;
}

/* a permutation cascade: single exact linear head */
static int perm_cascade(cce_cascade *cas, const int *perm) {
    cce_block head;
    int i;
    memset(cas, 0, sizeof *cas);
    memset(&head, 0, sizeof head);
    if (cce_cascade_init(cas, 2) != CCE_OK ||
        cce_block_init_linear(&head, SYM, SYM, 0.01f) != CCE_OK) return -1;
    head.type = CCE_BLOCK_LINEAR_HEAD;
    cce_tensor_zero(&head.weights);
    cce_tensor_zero(&head.bias);
    for (i = 0; i < SYM; i++) head.weights.data[i * SYM + perm[i]] = 1.0f;
    if (cce_cascade_append(cas, &head) != CCE_OK) {
        cce_block_free(&head);
        cce_cascade_free(cas);
        return -1;
    }
    return 0;
}

static int train_identity(BinaryTransformNetwork *btn, unsigned int seed,
                          Port in, Port out) {
    double ti[SYM][SYM], to[SYM][SYM];
    int i;
    for (i = 0; i < SYM; i++) { onehot_row(ti[i], i); onehot_row(to[i], i); }
    if (btn_init(btn, SYM, SYM, 8, 64, 0.5, seed) != 0 ||
        btn_set_ports(btn, in, out) != 0) return -1;
    btn_train_dynamic(btn, (const double *)ti, (const double *)to, SYM,
                      20000, 200, 1e-7, 1e-9);
    return 0;
}

int main(void) {
    const char *forest_path = "tmp_dispatch.cce";
    const int swap_perm[SYM] = {1, 0, 3, 2};
    const int rev_perm[SYM] = {3, 2, 1, 0};
    Port in_port = sym_port("ds_from");
    Port out_port = sym_port("ds_goal");
    cce_cascade cas_a, cas_b;
    cce_forest *forest = NULL;
    cce_model *model = NULL;
    BinaryTransformNetwork adapter, u_fast, u_slow, u_shiny;
    Contract c_model, c_fast, c_slow;
    Specialist spec;
    PrimitiveRegistry reg;
    RoutePlan plan;
    double mined_in[SYM][SYM], mined_out[SYM][SYM];
    double out[SYM];
    int i, ok, routed_a = 0, routed_b = 0;

    memset(&adapter, 0, sizeof adapter);
    memset(&u_fast, 0, sizeof u_fast);
    memset(&u_slow, 0, sizeof u_slow);
    memset(&u_shiny, 0, sizeof u_shiny);
    memset(&c_model, 0, sizeof c_model);
    memset(&c_fast, 0, sizeof c_fast);
    memset(&c_slow, 0, sizeof c_slow);
    memset(&plan, 0, sizeof plan);
    registry_init(&reg);
    reg.lifecycle_enabled = 1;
    remove(forest_path);

    printf("== one dispatch story: recall / synthesis / policy ==\n");

    /* ---- layer 1: recall dispatches INSIDE the specialist -------------- */
    check(perm_cascade(&cas_a, swap_perm) == 0 &&
          perm_cascade(&cas_b, rev_perm) == 0 &&
          cce_forest_open(&forest, forest_path, 4) == CCE_OK &&
          cce_forest_add_branch(forest, &cas_a, "recall-swap") == CCE_OK &&
          cce_forest_add_branch(forest, &cas_b, "recall-rev") == CCE_OK,
          "two-branch fixture forest builds (swap vs reverse)");
    /* distinct centroids: inputs 0/1 live near branch A, 2/3 near B */
    {
        int d;
        for (d = 0; d < SYM; d++) {
            forest->branches[0].centroid[d] = (d < 2) ? 1.0f : 0.0f;
            forest->branches[1].centroid[d] = (d < 2) ? 0.0f : 1.0f;
        }
        forest->branches[0].centroid_dim = SYM;
        forest->branches[1].centroid_dim = SYM;
    }
    check(cce_model_create(&model, "dispatch-recall") == CCE_OK &&
          cce_model_add_forest(model, forest, "primary") == CCE_OK,
          "routed model assembles");

    ok = 1;
    for (i = 0; i < SYM; i++) {
        float fin[SYM], fout[SYM];
        int j, dim = 0, hot;
        onehot_row(mined_in[i], i);
        for (j = 0; j < SYM; j++) fin[j] = (float)mined_in[i][j];
        if (cce_model_forward(model, fin, SYM, fout, SYM, &dim) != CCE_OK ||
            dim != SYM) { ok = 0; break; }
        for (j = 0; j < SYM; j++) mined_out[i][j] = (double)fout[j];
        hot = argmax4(mined_out[i]);
        if (hot == swap_perm[i]) routed_a++;
        if (hot == rev_perm[i]) routed_b++;
    }
    check(ok, "routed forward answers on the whole domain");
    check(routed_a > 0 && routed_b > 0,
          "recall used BOTH branches across the domain (real routing)");

    check(specialist_wrap_cce_model(&spec, &adapter, model, 0,
                                    &in_port, 1, &out_port, 1,
                                    0x44495350415443ULL, SYM * SYM,
                                    "ds_routed_unit") == 0 &&
          contract_init_borrowed(&c_model, "ds_routed_unit", &adapter,
                                 (const double *)mined_in,
                                 (const double *)mined_out, SYM) == 0 &&
          specialist_admit(&reg, &spec, &c_model) == 0,
          "the ROUTED composite certifies as one specialist");
    reg.require_certified = 1;
    check(route_plan(&reg, in_port, out_port, &plan) == 0 &&
          plan.length == 1 &&
          strcmp(plan.names[0], "ds_routed_unit") == 0,
          "the planner sees one node, not two branches");
    plan.strict = 1;
    ok = 1;
    for (i = 0; i < SYM; i++) {
        if (route_execute(&plan, mined_in[i], SYM, out, SYM) != 0 ||
            memcmp(out, mined_out[i], sizeof out) != 0) ok = 0;
    }
    check(ok, "strict execution replays the routed behavior exactly (4/4)");

    /* ---- layer 2: certification outranks everything --------------------- */
    {
        Port a = sym_port("ds_stage_a");
        Port b = sym_port("ds_stage_b");
        double ti[SYM][SYM], to[SYM][SYM];
        for (i = 0; i < SYM; i++) { onehot_row(ti[i], i); onehot_row(to[i], i); }
        check(train_identity(&u_fast, 42, a, b) == 0 &&
              train_identity(&u_slow, 43, a, b) == 0 &&
              train_identity(&u_shiny, 44, a, b) == 0,
              "three same-signature candidates train");
        check(contract_init_borrowed(&c_fast, "ds_u_fast", &u_fast,
                                     (const double *)ti,
                                     (const double *)to, SYM) == 0 &&
              registry_add_certified(&reg, &u_fast, "ds_u_fast",
                                     &c_fast) == 0 &&
              contract_init_borrowed(&c_slow, "ds_u_slow", &u_slow,
                                     (const double *)ti,
                                     (const double *)to, SYM) == 0 &&
              registry_add_certified(&reg, &u_slow, "ds_u_slow",
                                     &c_slow) == 0,
              "two candidates certify and register");
        check(registry_add(&reg, &u_shiny, "ds_u_shiny") == 0,
              "a third registers WITHOUT certification");
        u_fast.output_successes = 50;
        u_slow.output_successes = 1;
        u_slow.output_failures = 1;
        u_shiny.output_successes = 1000;  /* stellar evidence, no proof */

        check(route_plan(&reg, a, b, &plan) == 0 && plan.length == 1 &&
              strcmp(plan.names[0], "ds_u_fast") == 0,
              "reliability picks the stronger certified candidate");
        check(registry_set_state(&reg, "ds_u_fast", PRIM_RESET) == 0 &&
              route_plan(&reg, a, b, &plan) == 0 &&
              plan.length == 1 &&
              strcmp(plan.names[0], "ds_u_slow") == 0,
              "RESET re-routes to the certified alternative");
        check(registry_set_state(&reg, "ds_u_fast", PRIM_FROZEN) == 0 &&
              route_plan(&reg, a, b, &plan) == 0 &&
              plan.length == 1 &&
              strcmp(plan.names[0], "ds_u_fast") == 0,
              "restoring the state restores the preference");
        check(strcmp(plan.names[0], "ds_u_shiny") != 0,
              "evidence never outranks certification (shiny stays invisible)");

        /* ---- layer 2b: among the certified, reliability ranks ---------- */
        u_slow.output_successes = 500;
        u_slow.output_failures = 0;
        check(route_plan(&reg, a, b, &plan) == 0 && plan.length == 1 &&
              strcmp(plan.names[0], "ds_u_slow") == 0,
              "flip the evidence and the plan flips with it");
    }

    registry_free(&reg);
    contract_free(&c_model);
    contract_free(&c_fast);
    contract_free(&c_slow);
    btn_free(&adapter);
    btn_free(&u_fast);
    btn_free(&u_slow);
    btn_free(&u_shiny);
    if (model) cce_model_destroy(model);
    if (forest) cce_forest_close(forest);
    remove(forest_path);

    printf("DISPATCH_STORY_%s checks=%d\n", failures ? "FAIL" : "PASS",
           checks);
    return failures ? 1 : 0;
}
