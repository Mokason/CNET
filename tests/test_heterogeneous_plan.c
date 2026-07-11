/* The heterogeneous-plan acceptance gate — the acid test of unification.
 *
 * ONE ordinary planner plan whose nodes are three different backends behind
 * the one Specialist type: a native matrix BTN (trained), a real CCE model
 * (exact linear-head permutation), and an Oracle unit (external callback).
 * All three enter through the single admission door (specialist_admit),
 * are planned by the unmodified route/DAG planners with
 * require_certified = 1, execute strict over the WHOLE enumerated domain,
 * and share the trust/role axis vocabulary. If BTN, CCE, and Oracle merely
 * coexisted, this gate could not pass; it passes only if they are the same
 * planning citizen.
 *
 * Fixture: a 4-symbol one-hot alphabet chained through unique tags
 *   het_stage_a --btn(rot1)--> het_stage_b --cce(swap)--> het_stage_c
 *               --oracle(rot2)--> het_stage_d
 * so the types force the three-kind chain and the composed permutation is
 * verified exactly on all 4 canonical inputs.
 */

#include <stdio.h>
#include <string.h>

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/acquire.h"
#include "../include/specialist.h"
#include "../include/model_runtime.h"
#include "../include/cce/cce_tensor.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_cascade.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_model.h"

#define SYM 4

static int failures;

static void check(int ok, const char *name) {
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

static int rot1(int i) { return (i + 1) & 3; }
static int swap_pair(int i) { return i ^ 1; }
static int rot2(int i) { return (i + 2) & 3; }

static void onehot_row(double *row, int hot) {
    int i;
    for (i = 0; i < SYM; i++) row[i] = (i == hot) ? 1.0 : 0.0;
}

/* The external tool: an ordinary v1 oracle computing rot2 on the symbol. */
static int rot2_oracle(const double *in, double *out, void *ctx) {
    int i, hot = 0;
    (void)ctx;
    for (i = 1; i < SYM; i++) if (in[i] > in[hot]) hot = i;
    for (i = 0; i < SYM; i++) out[i] = 0.0;
    out[rot2(hot)] = 1.0;
    return 0;
}

static cce_model *build_swap_model(const char *forest_path,
                                   cce_forest **forest_out) {
    cce_cascade cascade;
    cce_block head;
    cce_forest *forest = NULL;
    cce_model *model = NULL;
    int i;

    memset(&cascade, 0, sizeof cascade);
    memset(&head, 0, sizeof head);
    if (cce_cascade_init(&cascade, 2) != CCE_OK) return NULL;
    if (cce_block_init_linear(&head, SYM, SYM, 0.01f) != CCE_OK) return NULL;
    head.type = CCE_BLOCK_LINEAR_HEAD;
    /* Exact swap permutation: out[o] = sum_i in[i] * w[i*SYM + o]. */
    cce_tensor_zero(&head.weights);
    cce_tensor_zero(&head.bias);
    for (i = 0; i < SYM; i++) head.weights.data[i * SYM + swap_pair(i)] = 1.0f;
    if (cce_cascade_append(&cascade, &head) != CCE_OK) return NULL;

    remove(forest_path);
    if (cce_forest_open(&forest, forest_path, 4) != CCE_OK ||
        cce_forest_add_branch(forest, &cascade, "het-swap") != CCE_OK ||
        cce_model_create(&model, "het-swap-model") != CCE_OK ||
        cce_model_add_forest(model, forest, "primary") != CCE_OK) return NULL;
    *forest_out = forest;
    return model;
}

/* Walk a planner-built DAG and record, by pointer identity, which of the
   three specialists actually appear as primitive nodes. */
static void walk_dag(const DagNode *node,
                     const BinaryTransformNetwork *b,
                     const BinaryTransformNetwork *c,
                     const BinaryTransformNetwork *o,
                     int *saw_btn, int *saw_cce, int *saw_oracle,
                     size_t *prim_nodes) {
    size_t k;
    if (!node) return;
    if (node->kind == DAG_PRIMITIVE) {
        (*prim_nodes)++;
        if (node->btn == b) *saw_btn = 1;
        if (node->btn == c) *saw_cce = 1;
        if (node->btn == o) *saw_oracle = 1;
    }
    for (k = 0; k < node->child_count; k++)
        walk_dag(node->children[k], b, c, o, saw_btn, saw_cce, saw_oracle,
                 prim_nodes);
}

int main(void) {
    const char *forest_path = "tmp_het_plan.cce";
    Port port_a = sym_port("het_stage_a");
    Port port_b = sym_port("het_stage_b");
    Port port_c = sym_port("het_stage_c");
    Port port_d = sym_port("het_stage_d");

    double btn_in[SYM][SYM], btn_tgt[SYM][SYM];
    double cce_in[SYM][SYM], cce_tgt[SYM][SYM];
    double orc_in[SYM][SYM], orc_tgt[SYM][SYM];

    BinaryTransformNetwork btn, cce_adapter, oracle_adapter, impostor;
    Contract btn_contract, cce_contract, orc_contract;
    Specialist spec_btn, spec_cce, spec_orc, spec_bad;
    PrimitiveRegistry reg;
    OracleRegistry oracles;
    cce_forest *forest = NULL;
    cce_model *model = NULL;
    RoutePlan plan;
    SpecialistTrust trust;
    SpecialistRole role;
    double out[SYM];
    int i, ok;

    memset(&btn, 0, sizeof btn);
    memset(&cce_adapter, 0, sizeof cce_adapter);
    memset(&oracle_adapter, 0, sizeof oracle_adapter);
    memset(&impostor, 0, sizeof impostor);
    memset(&btn_contract, 0, sizeof btn_contract);
    memset(&cce_contract, 0, sizeof cce_contract);
    memset(&orc_contract, 0, sizeof orc_contract);
    memset(&oracles, 0, sizeof oracles);
    memset(&plan, 0, sizeof plan);
    registry_init(&reg);

    for (i = 0; i < SYM; i++) {
        onehot_row(btn_in[i], i);   onehot_row(btn_tgt[i], rot1(i));
        onehot_row(cce_in[i], i);   onehot_row(cce_tgt[i], swap_pair(i));
        onehot_row(orc_in[i], i);   onehot_row(orc_tgt[i], rot2(i));
    }

    printf("== heterogeneous plan: BTN + CCE + Oracle as ONE specialist type ==\n");

    /* -- native BTN specialist: trained rot1 ----------------------------- */
    check(btn_init(&btn, SYM, SYM, 8, 64, 0.5, 42) == 0 &&
          btn_set_ports(&btn, port_a, port_b) == 0,
          "native BTN initializes with typed ports");
    btn_train_dynamic(&btn, (const double *)btn_in, (const double *)btn_tgt,
                      SYM, 20000, 200, 1e-4, 1e-6);
    check(contract_init_borrowed(&btn_contract, "het_btn_rot1", &btn,
                                 (const double *)btn_in,
                                 (const double *)btn_tgt, SYM) == 0,
          "rot1 contract authored from training table");
    check(specialist_wrap_btn(&spec_btn, &btn, "het_btn_rot1") == 0 &&
          spec_btn.kind == SPECIALIST_KIND_BTN,
          "native BTN wraps as Specialist(kind=btn)");
    check(specialist_admit(&reg, &spec_btn, &btn_contract) == 0 &&
          spec_btn.digest != 0,
          "BTN admitted through the one door (certified, digest recorded)");

    /* -- CCE specialist: exact linear-head swap -------------------------- */
    model = build_swap_model(forest_path, &forest);
    check(model != NULL && forest != NULL, "real CCE swap model builds");
    ok = 1;
    for (i = 0; i < SYM && model; i++) {
        float fin[SYM], fout[SYM];
        int j, dim = 0;
        for (j = 0; j < SYM; j++) fin[j] = (float)cce_in[i][j];
        if (cce_model_forward(model, fin, SYM, fout, SYM, &dim) != CCE_OK ||
            dim != SYM) { ok = 0; break; }
        for (j = 0; j < SYM; j++)
            if (fout[j] != (float)cce_tgt[i][j]) ok = 0;
    }
    check(ok, "direct CCE forward is the exact swap permutation");
    check(specialist_wrap_cce_model(&spec_cce, &cce_adapter, model, 0,
                                    &port_b, 1, &port_c, 1,
                                    0x4845545f43434531ULL, SYM * SYM,
                                    "het_cce_swap") == 0 &&
          spec_cce.kind == SPECIALIST_KIND_CCE,
          "CCE model wraps as Specialist(kind=cce)");
    check(contract_init_borrowed(&cce_contract, "het_cce_swap", &cce_adapter,
                                 (const double *)cce_in,
                                 (const double *)cce_tgt, SYM) == 0 &&
          specialist_admit(&reg, &spec_cce, &cce_contract) == 0,
          "CCE admitted through the SAME door");

    /* -- Oracle specialist: external rot2 tool --------------------------- */
    check(acquire_oracle_register(&oracles, "het_oracle_rot2", port_c, port_d,
                                  rot2_oracle, NULL) == 0,
          "external oracle registers");
    check(specialist_wrap_oracle(&spec_orc, &oracle_adapter,
                                 &oracles.entries[0],
                                 0x4845545f4f524331ULL, 1,
                                 "het_oracle_rot2") == 0 &&
          spec_orc.kind == SPECIALIST_KIND_ORACLE,
          "oracle wraps as Specialist(kind=oracle)");
    check(contract_init_borrowed(&orc_contract, "het_oracle_rot2",
                                 &oracle_adapter,
                                 (const double *)orc_in,
                                 (const double *)orc_tgt, SYM) == 0 &&
          specialist_admit(&reg, &spec_orc, &orc_contract) == 0,
          "oracle admitted through the SAME door");

    /* -- the door refuses an impostor, regardless of kind ---------------- */
    check(btn_init(&impostor, SYM, SYM, 8, 64, 0.5, 7) == 0 &&
          btn_set_ports(&impostor, port_a, port_b) == 0 &&
          specialist_wrap_btn(&spec_bad, &impostor, "het_btn_rot1") == 0 &&
          specialist_admit(&reg, &spec_bad, &btn_contract) != 0 &&
          reg.count == 3,
          "untrained impostor is refused at the door (registry untouched)");

    /* -- shared axis vocabulary ------------------------------------------ */
    ok = 1;
    {
        const char *names[3] =
            {"het_btn_rot1", "het_cce_swap", "het_oracle_rot2"};
        const Specialist *specs[3] = {&spec_btn, &spec_cce, &spec_orc};
        for (i = 0; i < 3; i++) {
            if (specialist_axes(&reg, names[i], &trust, &role) != 0 ||
                trust != SPECIALIST_TRUST_CERTIFIED ||
                role != SPECIALIST_ROLE_ACTIVE) ok = 0;
            else
                printf("    %-16s kind=%-6s trust=%-11s role=%s\n", names[i],
                       specialist_kind_name(specs[i]->kind),
                       specialist_trust_name(trust),
                       specialist_role_name(role));
        }
    }
    check(ok, "all three kinds read certified/active on the shared axes");
    check(specialist_residency_from_cce_tier(CCE_TIER_WARM) ==
              SPECIALIST_RES_WARM &&
          specialist_residency_from_model_state(CNET_MODEL_STATE_RESIDENT) ==
              SPECIALIST_RES_HOT &&
          specialist_residency_from_model_state(CNET_MODEL_STATE_FAILED) ==
              SPECIALIST_RES_COLD,
          "residency mappers fold CCE tiers + model states into one axis");

    /* -- ONE plan, three kinds, certified end-to-end, strict ------------- */
    reg.require_certified = 1;
    reg.lifecycle_enabled = 1;
    check(route_plan(&reg, port_a, port_d, &plan) == 0 && plan.length == 3 &&
          strcmp(plan.names[0], "het_btn_rot1") == 0 &&
          strcmp(plan.names[1], "het_cce_swap") == 0 &&
          strcmp(plan.names[2], "het_oracle_rot2") == 0,
          "ordinary planner chains BTN -> CCE -> Oracle unaided");
    plan.strict = 1;
    ok = 1;
    for (i = 0; i < SYM; i++) {
        double expected[SYM];
        int j;
        onehot_row(expected, rot2(swap_pair(rot1(i))));
        if (route_execute(&plan, btn_in[i], SYM, out, SYM) != 0) { ok = 0; break; }
        for (j = 0; j < SYM; j++) if (out[j] != expected[j]) ok = 0;
    }
    check(ok, "strict execution exact on the whole enumerated domain (4/4)");

    {
        DagPlan dplan;
        DagSource source;
        int saw_btn = 0, saw_cce = 0, saw_oracle = 0;
        size_t prim_nodes = 0;
        memset(&dplan, 0, sizeof dplan);
        source.type = port_a;
        source.values = btn_in[0];
        check(dag_plan(&reg, &source, 1, port_d, &dplan) == 0,
              "DAG planner finds the heterogeneous plan");
        walk_dag(dplan.root, &btn, &cce_adapter, &oracle_adapter,
                 &saw_btn, &saw_cce, &saw_oracle, &prim_nodes);
        check(prim_nodes == 3 && saw_btn && saw_cce && saw_oracle,
              "ONE DAG contains all three kinds as ordinary nodes");
        dplan.strict = 1;
        ok = 1;
        for (i = 0; i < SYM; i++) {
            double expected[SYM];
            int j;
            onehot_row(expected, rot2(swap_pair(rot1(i))));
            source.values = btn_in[i];
            if (dag_execute(&dplan, &source, 1, out, SYM) != 0) { ok = 0; break; }
            for (j = 0; j < SYM; j++) if (out[j] != expected[j]) ok = 0;
        }
        check(ok, "strict DAG execution exact on the whole domain (4/4)");
        dag_free(&dplan);
    }

    check(registry_audit_certified(&reg) == 0,
          "certificate-to-weights audit demotes nothing across kinds");

    /* -- lifecycle acts identically on a non-BTN kind --------------------- */
    check(registry_set_state(&reg, "het_cce_swap", PRIM_RESET) == 0 &&
          specialist_axes(&reg, "het_cce_swap", &trust, NULL) == 0 &&
          trust == SPECIALIST_TRUST_DEMOTED,
          "demoting the CCE kind reads DEMOTED on the trust axis");
    check(route_plan(&reg, port_a, port_d, &plan) != 0,
          "planner refuses the chain while its CCE stage is demoted");
    check(registry_set_state(&reg, "het_cce_swap", PRIM_FROZEN) == 0 &&
          route_plan(&reg, port_a, port_d, &plan) == 0 && plan.length == 3,
          "restoring trust restores the heterogeneous plan");

    registry_free(&reg);
    contract_free(&btn_contract);
    contract_free(&cce_contract);
    contract_free(&orc_contract);
    btn_free(&btn);
    btn_free(&impostor);
    btn_free(&cce_adapter);
    btn_free(&oracle_adapter);
    if (model) cce_model_destroy(model);
    if (forest) cce_forest_close(forest);
    remove(forest_path);

    printf("HET_PLAN_%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
