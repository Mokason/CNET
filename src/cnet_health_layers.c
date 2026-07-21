/*
 * cnet_health_layers.c — five-layer diagnostic ladder for specialists.
 *
 * Measures only. Does not demote, heal, or admit. Maintenance remains
 * specialist_health_pass.
 */
#include "../include/cnet_health_layers.h"
#include "../include/specialist.h"
#include "../include/nn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void cnet_health_layer_config_defaults(CnetHealthLayerConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->utility_reliability_floor = 0.9;
    cfg->utility_min_evidence = 16;
    cfg->require_certify = 1;
}

const char *cnet_health_layer_name(CnetHealthLayer layer) {
    switch (layer) {
        case CNET_HEALTH_LAYER_REGISTRY:  return "registry";
        case CNET_HEALTH_LAYER_LOADABLE:  return "loadable";
        case CNET_HEALTH_LAYER_EXECUTION: return "execution";
        case CNET_HEALTH_LAYER_SEMANTIC:  return "semantic";
        case CNET_HEALTH_LAYER_UTILITY:   return "utility";
        default: return "unknown";
    }
}

const char *cnet_health_verdict_name(CnetHealthVerdict v) {
    switch (v) {
        case CNET_HEALTH_PASS: return "pass";
        case CNET_HEALTH_FAIL: return "fail";
        case CNET_HEALTH_SKIP: return "skip";
        default: return "unknown";
    }
}

static void set_layer(CnetUnitHealthLayers *u, CnetHealthLayer L,
                      CnetHealthVerdict v, const char *reason) {
    u->layer[L] = v;
    snprintf(u->reason[L], sizeof u->reason[L], "%s",
             reason ? reason : "");
}

static void skip_above(CnetUnitHealthLayers *u, CnetHealthLayer from,
                       const char *reason) {
    int L;
    for (L = (int)from; L < CNET_HEALTH_LAYER_COUNT; ++L)
        set_layer(u, (CnetHealthLayer)L, CNET_HEALTH_SKIP, reason);
}

static const RegistryEntry *find_entry(const PrimitiveRegistry *reg,
                                       const char *name) {
    size_t i;
    if (!reg || !name) return NULL;
    for (i = 0; i < reg->count; ++i)
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    return NULL;
}

static size_t port_total(const Port *ports, size_t n) {
    size_t i, t = 0;
    for (i = 0; i < n; ++i)
        t += ports[i].field_width * ports[i].field_count;
    return t;
}

/* Build a simple one-hot (or zero) probe of length n. */
static void fill_probe(double *buf, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) buf[i] = 0.0;
    if (n > 0) buf[0] = 1.0;
}

static void finalize_depth(CnetUnitHealthLayers *u) {
    int L;
    u->deepest_pass = -1;
    for (L = 0; L < CNET_HEALTH_LAYER_COUNT; ++L) {
        if (u->layer[L] != CNET_HEALTH_PASS) break;
        u->deepest_pass = L;
    }
    u->production_ready =
        (u->deepest_pass == (int)CNET_HEALTH_LAYER_UTILITY) ? 1 : 0;
}

int cnet_health_check_unit(const PrimitiveRegistry *reg, const char *name,
                           const CnetHealthLayerConfig *cfg,
                           CnetUnitHealthLayers *out) {
    CnetHealthLayerConfig defaults;
    const RegistryEntry *e;
    BinaryTransformNetwork *btn;
    size_t in_n, out_n;
    double *probe = NULL;
    const double *y;
    SpecialistTrust trust = SPECIALIST_TRUST_UNCERTIFIED;
    SpecialistRole role = SPECIALIST_ROLE_ACTIVE;
    double rel = -1.0;
    size_t evidence = 0;
    const Contract *c = NULL;

    if (!reg || !name || !name[0] || !out) return -1;
    if (!cfg) {
        cnet_health_layer_config_defaults(&defaults);
        cfg = &defaults;
    }
    memset(out, 0, sizeof *out);
    snprintf(out->unit_name, sizeof out->unit_name, "%s", name);
    out->reliability = -1.0;

    /* ---- 0 REGISTRY ---- */
    e = find_entry(reg, name);
    if (!e) {
        set_layer(out, CNET_HEALTH_LAYER_REGISTRY, CNET_HEALTH_FAIL,
                  "absent");
        skip_above(out, CNET_HEALTH_LAYER_LOADABLE, "no_registry");
        finalize_depth(out);
        return 0;
    }
    set_layer(out, CNET_HEALTH_LAYER_REGISTRY, CNET_HEALTH_PASS, "present");

    /* ---- 1 LOADABLE ---- */
    btn = e->btn;
    if (!btn) {
        set_layer(out, CNET_HEALTH_LAYER_LOADABLE, CNET_HEALTH_FAIL,
                  "btn_null");
        skip_above(out, CNET_HEALTH_LAYER_EXECUTION, "not_loadable");
        finalize_depth(out);
        return 0;
    }
    in_n = btn->input_count;
    out_n = btn->output_count;
    if (in_n == 0 || out_n == 0) {
        set_layer(out, CNET_HEALTH_LAYER_LOADABLE, CNET_HEALTH_FAIL,
                  "zero_dims");
        skip_above(out, CNET_HEALTH_LAYER_EXECUTION, "not_loadable");
        finalize_depth(out);
        return 0;
    }
    if (btn->input_port_count == 0 || btn->output_port_count == 0) {
        set_layer(out, CNET_HEALTH_LAYER_LOADABLE, CNET_HEALTH_FAIL,
                  "no_ports");
        skip_above(out, CNET_HEALTH_LAYER_EXECUTION, "not_loadable");
        finalize_depth(out);
        return 0;
    }
    if (port_total(btn->input_ports, btn->input_port_count) != in_n ||
        port_total(btn->output_ports, btn->output_port_count) != out_n) {
        set_layer(out, CNET_HEALTH_LAYER_LOADABLE, CNET_HEALTH_FAIL,
                  "port_dim_mismatch");
        skip_above(out, CNET_HEALTH_LAYER_EXECUTION, "not_loadable");
        finalize_depth(out);
        return 0;
    }
    set_layer(out, CNET_HEALTH_LAYER_LOADABLE, CNET_HEALTH_PASS, "resident");

    /* ---- 2 EXECUTION ---- */
    probe = (double *)calloc(in_n, sizeof(double));
    if (!probe) {
        set_layer(out, CNET_HEALTH_LAYER_EXECUTION, CNET_HEALTH_FAIL,
                  "oom");
        skip_above(out, CNET_HEALTH_LAYER_SEMANTIC, "no_execution");
        finalize_depth(out);
        return 0;
    }
    fill_probe(probe, in_n);
    y = btn_forward(btn, probe);
    free(probe);
    if (!y) {
        set_layer(out, CNET_HEALTH_LAYER_EXECUTION, CNET_HEALTH_FAIL,
                  "forward_null");
        skip_above(out, CNET_HEALTH_LAYER_SEMANTIC, "no_execution");
        finalize_depth(out);
        return 0;
    }
    set_layer(out, CNET_HEALTH_LAYER_EXECUTION, CNET_HEALTH_PASS, "forward_ok");

    /* ---- 3 SEMANTIC ---- */
    if (cfg->contracts)
        c = cfg->contracts(name, cfg->contracts_ctx);
    if (!c) {
        /* No contract source: accept certified flag as weak semantic signal,
           else SKIP (cannot evaluate acceptance without a spec). */
        if (e->certified && e->state != PRIM_RESET) {
            set_layer(out, CNET_HEALTH_LAYER_SEMANTIC, CNET_HEALTH_PASS,
                      "certified_flag");
        } else if (!cfg->require_certify) {
            set_layer(out, CNET_HEALTH_LAYER_SEMANTIC, CNET_HEALTH_PASS,
                      "no_contract_optional");
        } else {
            set_layer(out, CNET_HEALTH_LAYER_SEMANTIC, CNET_HEALTH_SKIP,
                      "no_contract");
            skip_above(out, CNET_HEALTH_LAYER_UTILITY, "no_semantic");
            finalize_depth(out);
            return 0;
        }
    } else if (cfg->require_certify) {
        /* btn_certify is non-const but does not retrain; cast is intentional. */
        if (btn_certify((BinaryTransformNetwork *)btn, c, NULL) == 0)
            set_layer(out, CNET_HEALTH_LAYER_SEMANTIC, CNET_HEALTH_PASS,
                      "certify_ok");
        else {
            set_layer(out, CNET_HEALTH_LAYER_SEMANTIC, CNET_HEALTH_FAIL,
                      "certify_fail");
            skip_above(out, CNET_HEALTH_LAYER_UTILITY, "no_semantic");
            finalize_depth(out);
            return 0;
        }
    } else {
        set_layer(out, CNET_HEALTH_LAYER_SEMANTIC, CNET_HEALTH_PASS,
                  "contract_present");
    }

    /* ---- 4 UTILITY (production) ---- */
    (void)specialist_axes(reg, name, &trust, &role);
    rel = btn_reliability(btn);
    out->reliability = rel;
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    evidence = (size_t)btn->output_successes + (size_t)btn->output_failures;
#else
    evidence = (size_t)btn->output_successes + (size_t)btn->output_failures;
#endif
    out->evidence = evidence;

    if (e->state == PRIM_RESET || trust == SPECIALIST_TRUST_DEMOTED) {
        set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_FAIL,
                  "demoted");
    } else if (e->shadow_of != NULL || role == SPECIALIST_ROLE_SHADOW) {
        set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_FAIL,
                  "shadow_not_production");
    } else if (role == SPECIALIST_ROLE_ADVISORY) {
        set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_FAIL,
                  "advisory");
    } else if (role == SPECIALIST_ROLE_RECIPE) {
        set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_FAIL,
                  "recipe_chunk");
    } else if (e->certified || trust == SPECIALIST_TRUST_CERTIFIED ||
               e->state == PRIM_FROZEN) {
        /* Certified admission is production utility. Only flag low live
           reliability once enough outcomes exist (fresh certs score ~0.5). */
        if (cfg->utility_min_evidence > 0 &&
            evidence >= cfg->utility_min_evidence &&
            rel < cfg->utility_reliability_floor)
            set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_FAIL,
                      "low_reliability");
        else
            set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_PASS,
                      "certified_production");
    } else if (trust == SPECIALIST_TRUST_EVIDENCED ||
               e->state == PRIM_PROVISIONAL) {
        if (evidence >= cfg->utility_min_evidence &&
            rel >= cfg->utility_reliability_floor)
            set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_PASS,
                      "provisional_useful");
        else if (evidence < cfg->utility_min_evidence)
            set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_FAIL,
                      "insufficient_evidence");
        else
            set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_FAIL,
                      "low_reliability");
    } else {
        set_layer(out, CNET_HEALTH_LAYER_UTILITY, CNET_HEALTH_FAIL,
                  "uncertified");
    }

    finalize_depth(out);
    return 0;
}

int cnet_health_check_registry(const PrimitiveRegistry *reg,
                               const CnetHealthLayerConfig *cfg,
                               CnetRegistryHealthLayers *agg,
                               CnetHealthUnitFn on_unit, void *on_unit_ctx) {
    size_t i;
    CnetHealthLayerConfig defaults;
    if (!reg) return -1;
    if (!cfg) {
        cnet_health_layer_config_defaults(&defaults);
        cfg = &defaults;
    }
    if (agg) memset(agg, 0, sizeof *agg);
    for (i = 0; i < reg->count; ++i) {
        CnetUnitHealthLayers u;
        int L;
        if (!reg->entries[i].name) continue;
        if (cnet_health_check_unit(reg, reg->entries[i].name, cfg, &u) != 0)
            continue;
        if (agg) {
            agg->units++;
            for (L = 0; L < CNET_HEALTH_LAYER_COUNT; ++L) {
                if (u.layer[L] == CNET_HEALTH_PASS) agg->pass[L]++;
                else if (u.layer[L] == CNET_HEALTH_FAIL) agg->fail[L]++;
                else agg->skip[L]++;
            }
            if (u.production_ready) agg->production_ready++;
            if (u.deepest_pass >= -1 &&
                u.deepest_pass + 1 < (int)(CNET_HEALTH_LAYER_COUNT + 1))
                agg->deepest_hist[u.deepest_pass + 1]++;
        }
        if (on_unit) on_unit(&u, on_unit_ctx);
    }
    return 0;
}

int cnet_health_layers_format_json(const CnetUnitHealthLayers *u, char *buf,
                                   size_t cap) {
    int n;
    if (!u || !buf || cap < 32) return -1;
    n = snprintf(
        buf, cap,
        "{\"unit\":\"%s\","
        "\"registry\":\"%s\",\"loadable\":\"%s\",\"execution\":\"%s\","
        "\"semantic\":\"%s\",\"utility\":\"%s\","
        "\"registry_reason\":\"%s\",\"loadable_reason\":\"%s\","
        "\"execution_reason\":\"%s\",\"semantic_reason\":\"%s\","
        "\"utility_reason\":\"%s\","
        "\"deepest_pass\":%d,\"production_ready\":%d,"
        "\"reliability\":%.6f,\"evidence\":%zu}",
        u->unit_name,
        cnet_health_verdict_name(u->layer[CNET_HEALTH_LAYER_REGISTRY]),
        cnet_health_verdict_name(u->layer[CNET_HEALTH_LAYER_LOADABLE]),
        cnet_health_verdict_name(u->layer[CNET_HEALTH_LAYER_EXECUTION]),
        cnet_health_verdict_name(u->layer[CNET_HEALTH_LAYER_SEMANTIC]),
        cnet_health_verdict_name(u->layer[CNET_HEALTH_LAYER_UTILITY]),
        u->reason[CNET_HEALTH_LAYER_REGISTRY],
        u->reason[CNET_HEALTH_LAYER_LOADABLE],
        u->reason[CNET_HEALTH_LAYER_EXECUTION],
        u->reason[CNET_HEALTH_LAYER_SEMANTIC],
        u->reason[CNET_HEALTH_LAYER_UTILITY],
        u->deepest_pass, u->production_ready,
        u->reliability, u->evidence);
    if (n < 0 || (size_t)n >= cap) return -1;
    return 0;
}
