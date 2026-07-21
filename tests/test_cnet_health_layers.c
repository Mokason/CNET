/*
 * test_cnet_health_layers.c — five-layer health diagnostics gate.
 * make health_layers → HEALTH_LAYERS_PASS
 */
#include <stdio.h>
#include <string.h>

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"
#include "../include/cnet_health_layers.h"

#define SYM 4

static int failures, checks;

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

static struct { const char *name; const Contract *c; } table[8];
static size_t table_n;

static void table_put(const char *name, const Contract *c) {
    table[table_n].name = name;
    table[table_n].c = c;
    table_n++;
}

static const Contract *lookup(const char *name, void *ctx) {
    size_t i;
    (void)ctx;
    for (i = 0; i < table_n; i++)
        if (strcmp(table[i].name, name) == 0) return table[i].c;
    return NULL;
}

int main(void) {
    PrimitiveRegistry reg;
    BinaryTransformNetwork btn;
    Contract contract;
    Port in = sym_port("hl_in");
    Port out = sym_port("hl_out");
    double inputs[SYM * SYM];
    double targets[SYM * SYM];
    int i;
    CnetHealthLayerConfig cfg;
    CnetUnitHealthLayers u;
    CnetRegistryHealthLayers agg;
    char json[1024];
    Specialist s;

    printf("== cnet_health_layers ==\n");
    for (i = 0; i < SYM; i++) {
        onehot_row(inputs + i * SYM, i);
        onehot_row(targets + i * SYM, i);
    }

    memset(&btn, 0, sizeof btn);
    check(btn_init(&btn, SYM, SYM, 8, 64, 0.5, 11u) == 0, "btn init");
    check(btn_set_ports(&btn, in, out) == 0, "ports");
    btn_train_dynamic(&btn, inputs, targets, SYM, 8000, 200, 1e-4, 1e-6);
    check(contract_init_borrowed(&contract, "hl_id", &btn, inputs, targets,
                                 SYM) == 0,
          "contract");
    check(btn_certify(&btn, &contract, NULL) == 0, "certify");

    registry_init_production(&reg);
    reg.require_certified = 1;
    table_n = 0;
    table_put("hl_id", &contract);
    memset(&s, 0, sizeof s);
    check(specialist_wrap_btn(&s, &btn, "hl_id") == 0, "wrap");
    check(specialist_admit(&reg, &s, &contract) == 0, "admit");

    cnet_health_layer_config_defaults(&cfg);
    cfg.contracts = lookup;
    cfg.utility_min_evidence = 0; /* fresh certified may have zero outcomes */

    check(cnet_health_check_unit(&reg, "missing", &cfg, &u) == 0, "check miss");
    check(u.layer[CNET_HEALTH_LAYER_REGISTRY] == CNET_HEALTH_FAIL &&
              strcmp(u.reason[CNET_HEALTH_LAYER_REGISTRY], "absent") == 0,
          "missing: registry FAIL");
    check(u.layer[CNET_HEALTH_LAYER_LOADABLE] == CNET_HEALTH_SKIP,
          "missing: loadable SKIP");
    check(u.deepest_pass == -1, "missing: deepest -1");
    check(!u.production_ready, "missing: not production");

    check(cnet_health_check_unit(&reg, "hl_id", &cfg, &u) == 0, "check good");
    check(u.layer[CNET_HEALTH_LAYER_REGISTRY] == CNET_HEALTH_PASS,
          "good: registry");
    check(u.layer[CNET_HEALTH_LAYER_LOADABLE] == CNET_HEALTH_PASS,
          "good: loadable");
    check(u.layer[CNET_HEALTH_LAYER_EXECUTION] == CNET_HEALTH_PASS,
          "good: execution");
    check(u.layer[CNET_HEALTH_LAYER_SEMANTIC] == CNET_HEALTH_PASS,
          "good: semantic");
    check(u.layer[CNET_HEALTH_LAYER_UTILITY] == CNET_HEALTH_PASS,
          "good: utility");
    check(u.deepest_pass == (int)CNET_HEALTH_LAYER_UTILITY,
          "good: deepest utility");
    check(u.production_ready, "good: production ready");

    check(cnet_health_layers_format_json(&u, json, sizeof json) == 0,
          "format json");
    check(strstr(json, "\"registry\":\"pass\"") &&
              strstr(json, "\"loadable\":\"pass\"") &&
              strstr(json, "\"execution\":\"pass\"") &&
              strstr(json, "\"semantic\":\"pass\"") &&
              strstr(json, "\"utility\":\"pass\""),
          "json five pass");

    /* Semantic fail: break weights after admit, keep entry. */
    {
        size_t k;
        for (k = 0; k < btn.hidden_count * btn.output_count; k++)
            btn.hidden_output_weights[k] = 0.0;
        for (k = 0; k < btn.output_count; k++) btn.output_bias[k] = 0.0;
        check(cnet_health_check_unit(&reg, "hl_id", &cfg, &u) == 0,
              "check broken");
        check(u.layer[CNET_HEALTH_LAYER_REGISTRY] == CNET_HEALTH_PASS &&
                  u.layer[CNET_HEALTH_LAYER_LOADABLE] == CNET_HEALTH_PASS &&
                  u.layer[CNET_HEALTH_LAYER_EXECUTION] == CNET_HEALTH_PASS,
              "broken: still runs");
        check(u.layer[CNET_HEALTH_LAYER_SEMANTIC] == CNET_HEALTH_FAIL,
              "broken: semantic FAIL");
        check(u.layer[CNET_HEALTH_LAYER_UTILITY] == CNET_HEALTH_SKIP,
              "broken: utility SKIP after semantic fail");
        check(u.deepest_pass == (int)CNET_HEALTH_LAYER_EXECUTION,
              "broken: deepest execution");
    }

    /* Registry aggregate */
    check(cnet_health_check_registry(&reg, &cfg, &agg, NULL, NULL) == 0,
          "registry check");
    check(agg.units == 1, "agg units");
    check(agg.pass[CNET_HEALTH_LAYER_REGISTRY] == 1, "agg registry pass");
    check(agg.fail[CNET_HEALTH_LAYER_SEMANTIC] == 1, "agg semantic fail");

    check(strcmp(cnet_health_layer_name(CNET_HEALTH_LAYER_LOADABLE),
                 "loadable") == 0,
          "layer name");
    check(strcmp(cnet_health_verdict_name(CNET_HEALTH_SKIP), "skip") == 0,
          "verdict name");

    /* No contract => semantic SKIP (require_certify) unless certified_flag.
       After weight break certified may still be set — clear via demote path:
       set state RESET and certified 0. */
    {
        CnetHealthLayerConfig cfg2 = cfg;
        cfg2.contracts = NULL;
        reg.entries[0].certified = 0;
        reg.entries[0].state = PRIM_FUZZY;
        check(cnet_health_check_unit(&reg, "hl_id", &cfg2, &u) == 0,
              "no-contract check");
        check(u.layer[CNET_HEALTH_LAYER_SEMANTIC] == CNET_HEALTH_SKIP,
              "no contract: semantic SKIP");
    }

    registry_free(&reg);
    contract_free(&contract);
    btn_free(&btn);

    if (failures) {
        printf("HEALTH_LAYERS_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("HEALTH_LAYERS_PASS checks=%d\n", checks);
    return 0;
}
