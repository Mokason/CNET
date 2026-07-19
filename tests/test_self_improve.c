/* Bounded self-improve gate: distill multi-step plans + recipe proposals
 * + harness oracle admit + deploy env free-wins.
 * make self_improve → SELF_IMPROVE_PASS
 */
#include <stdio.h>
#include "../include/cnet_platform.h"
#include <stdlib.h>
#include <string.h>

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"
#include "../include/contract/contract.h"
#include "../include/library.h"
#include "../include/self_improve.h"
#include "../include/resource_governor.h"
#include "../include/harness_oracle.h"
#include "../include/specialist.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port PT(PortFamily f, size_t w, size_t c, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = f;
    p.field_width = w;
    p.field_count = c;
    if (tag) port_set_tag(&p, tag);
    return p;
}

static void msb2(int i, double *o) {
    o[0] = (double)((i >> 1) & 1);
    o[1] = (double)(i & 1);
}
static void msb3(int i, double *o) {
    o[0] = (double)((i >> 2) & 1);
    o[1] = (double)((i >> 1) & 1);
    o[2] = (double)(i & 1);
}

static int make_dec(BinaryTransformNetwork *b) {
    double in[4][4] = {{0}};
    double tg[4][2];
    int i;
    if (btn_init(b, 4, 2, 1, 16, 0.8, 11u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 4, 1, "si_a"),
                      PT(PORT_BINARY_MSB, 2, 1, "si_b")) != 0) return -1;
    for (i = 0; i < 4; ++i) { in[i][i] = 1.0; msb2(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static int make_inc(BinaryTransformNetwork *b) {
    double in[4][2];
    double tg[4][3];
    int i;
    if (btn_init(b, 2, 3, 1, 16, 0.8, 13u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, "si_b"),
                      PT(PORT_BINARY_MSB, 3, 1, "si_c")) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb3(i + 1, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static int rot1_oracle(const double *in, double *out, void *ctx) {
    int i, hot = 0;
    (void)ctx;
    for (i = 1; i < 4; i++) if (in[i] > in[hot]) hot = i;
    for (i = 0; i < 4; i++) out[i] = 0.0;
    out[(hot + 1) & 3] = 1.0;
    return 0;
}

int main(void) {
    BinaryTransformNetwork dec = {0}, inc = {0};
    BinaryTransformNetwork *chunk = NULL;
    PrimitiveRegistry reg;
    RoutePlan plan;
    SelfImproveReport rep;
    CnetResourceGovernor gov;
    CnetGovPolicy pol;
    LibraryGateConfig gate;
    const char *prop_path = "tmp_self_improve_proposals.jsonl";
    HarnessOracleSlot slot;
    CnetOracleIdentity id;
    Contract orc_c;
    double orc_in[4][4], orc_tg[4][4];
    Port pin, pout;
    int i;

    printf("== self_improve ==\n");
    remove(prop_path);
    registry_init(&reg);

    /* Deploy free-wins fill holes only. */
    cnet_unsetenv("CNET_TRAIN_FAST");
    cnet_unsetenv("CNET_ORACLE_INT8");
    cnet_unsetenv("CNET_ACQ_STAGES");
    cnet_unsetenv("CNET_LANE_MAX_CLOSURES");
    cnet_unsetenv("CNET_TEACHER_IDLE_SEC");
    cnet_unsetenv("CNET_HEALTH_TICK_SECONDS");
    cnet_unsetenv("CNET_ACQ_ADAPTIVE");
    cnet_unsetenv("CNET_LANE_PROBE_BATCH");
    cnet_unsetenv("CNET_GGUF_MMAP");
    {
        int n = self_improve_apply_deploy_env();
        check(n >= 6, "deploy env sets free-win holes");
        check(getenv("CNET_TRAIN_FAST") && strcmp(getenv("CNET_TRAIN_FAST"), "1") == 0,
              "TRAIN_FAST defaulted");
        check(getenv("CNET_ORACLE_INT8") && strcmp(getenv("CNET_ORACLE_INT8"), "1") == 0,
              "ORACLE_INT8 defaulted");
        check(getenv("CNET_ACQ_STAGES") && strcmp(getenv("CNET_ACQ_STAGES"), "40") == 0,
              "ACQ_STAGES=40 defaulted");
    }
    cnet_setenv("CNET_TRAIN_FAST", "0", 1);
    check(self_improve_apply_deploy_env() == 0 ||
          strcmp(getenv("CNET_TRAIN_FAST"), "0") == 0,
          "explicit operator choice not overridden");

    check(make_dec(&dec) == 0 && make_inc(&inc) == 0, "base primitives train");
    registry_add(&reg, &dec, "si_dec");
    registry_add(&reg, &inc, "si_inc");
    /* Seed evidence so gated path can be tested; gate default is off. */
    dec.output_successes = 32;
    inc.output_successes = 32;

    check(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, "si_a"),
                     PT(PORT_BINARY_MSB, 3, 1, "si_c"), &plan) == 0 &&
          plan.length >= 2,
          "multi-step plan exists");

    memset(&rep, 0, sizeof rep);
    library_gate_config_defaults(&gate);
    gate.enabled = 0;
    check(self_improve_distill_route(&reg, &plan, NULL, &gate, NULL,
                                     &chunk, &rep) == 0 &&
          rep.distilled == 1 && chunk != NULL,
          "distill mints certified chunk via specialist door");
    check(reg.count >= 3, "registry grew after distill");

    /* Budget refuses second mint when rate limit exhausted. */
    cnet_gov_policy_deploy_defaults(&pol);
    pol.max_closures_per_drain = 1;
    cnet_gov_open(&gov, &pol);
    cnet_gov_begin_drain(&gov);
    cnet_gov_note_close(&gov); /* consume the one close */
    memset(&rep, 0, sizeof rep);
    check(self_improve_distill_route(&reg, &plan, NULL, &gate, &gov,
                                     NULL, &rep) == 1 &&
          rep.skipped_budget == 1,
          "governor rate-limit skips further distill");
    cnet_gov_close(&gov);

    /* Recipe proposals: ban quality-floor lowering. */
    memset(&rep, 0, sizeof rep);
    check(self_improve_propose_recipe(prop_path, "lower_margin", "\"x\"", 1,
                                      &rep) == -2,
          "banned kind refused");
    check(self_improve_propose_recipe(prop_path, "topk_set_v2",
                                      "\"margin_sweep predicts +154\"", 5,
                                      &rep) == 0 &&
          rep.proposals_written == 1,
          "topk_set_v2 proposal written");
    check(self_improve_propose_recipe(prop_path, "train_fast_default",
                                      "\"TEACH_FAST_PASS byte-identical\"", 4,
                                      &rep) == 0,
          "train_fast_default proposal written");

    /* Harness/oracle admit path through the one door. */
    pin = PT(PORT_ONEHOT, 4, 1, "ho_in");
    pout = PT(PORT_ONEHOT, 4, 1, "ho_out");
    for (i = 0; i < 4; i++) {
        int j;
        for (j = 0; j < 4; j++) {
            orc_in[i][j] = (j == i) ? 1.0 : 0.0;
            orc_tg[i][j] = (j == ((i + 1) & 3)) ? 1.0 : 0.0;
        }
    }
    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = 0x4841524e45535331ULL;
    id.contract_digest = 0xC0C0C0C0ULL;
    memset(&slot, 0, sizeof slot);
    {
        PrimitiveRegistry reg2;
        registry_init(&reg2);
        check(harness_oracle_bind(&slot, "ho_rot1", pin, pout, rot1_oracle,
                                  NULL, &id, 0x484F5F524F5431ULL) == 0,
              "harness oracle binds");
        memset(&orc_c, 0, sizeof orc_c);
        check(contract_init_borrowed(&orc_c, "ho_rot1", &slot.btn,
                                     (const double *)orc_in,
                                     (const double *)orc_tg, 4) == 0,
              "oracle contract from table");
        check(harness_oracle_admit(&slot, &reg2, &orc_c) == 0,
              "harness oracle admitted through specialist door");
        check(reg2.count == 1 && reg2.entries[0].certified,
              "admitted entry certified");
        harness_oracle_unbind(&slot);
        contract_free(&orc_c);
        registry_free(&reg2);
    }

    if (chunk) { /* owned by registry borrow pattern; free after registry_free */ }
    registry_free(&reg);
    btn_free(&dec);
    btn_free(&inc);
    if (chunk) { btn_free(chunk); free(chunk); }
    remove(prop_path);

    printf("SELF_IMPROVE_PASS checks=%d\n", checks);
    return failures ? 1 : 0;
}
