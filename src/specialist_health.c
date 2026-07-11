/* Runtime health optimizer. Fix-then-improve over existing certified paths
 * only: audit -> label (contract, then teacher) -> heal -> promote ->
 * shadow-swap. No step here can freeze without a passing certify or retrain
 * without a verified target; the optimizer is policy, never authority. */

#include <string.h>

#include "../include/specialist_health.h"

void specialist_health_config_defaults(SpecialistHealthConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof *cfg);
    cfg->audit = 1;
    cfg->label_from_contract = 1;
    cfg->label_via_teacher = 1;
    cfg->heal = 1;
    cfg->heal_max_epochs = 4000;
    cfg->promote = 1;
    cfg->promote_threshold = 0.9;
    cfg->promote_min_evidence = 16;
    cfg->promote_shadows = 1;
    cfg->shadow_min_evidence = 16;
    /* contracts / contracts_ctx stay NULL: heal and shadow promotion need a
       contract source and are skipped without one. */
}

static size_t ports_total(const Port *ports, size_t n) {
    size_t i, total = 0;
    for (i = 0; i < n; i++) total += ports[i].field_width * ports[i].field_count;
    return total;
}

static int rows_equal(const double *a, const double *b, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/* Target source A: a parked fault whose input appears in the contract's
   exemplar table has a verified target — the exemplar's output. Repeats the
   scan after every successful label because registry_supply_label compacts
   the unlabeled queue. */
static size_t label_from_contract(PrimitiveRegistry *reg,
                                  const RegistryEntry *e,
                                  const Contract *c) {
    size_t labeled = 0;
    size_t in_total, out_total;
    int progress = 1;
    if (!e->queue || !c || c->exemplar_count == 0) return 0;
    in_total = ports_total(c->input_ports, c->input_port_count);
    out_total = ports_total(c->output_ports, c->output_port_count);
    if (in_total != e->queue->input_count ||
        out_total != e->queue->output_count) return 0;
    while (progress) {
        size_t i;
        progress = 0;
        for (i = 0; i < e->queue->unlabeled_count; i++) {
            const double *input = e->queue->unlabeled_inputs + i * in_total;
            size_t k;
            for (k = 0; k < c->exemplar_count; k++) {
                if (!rows_equal(input, c->inputs + k * in_total, in_total))
                    continue;
                if (registry_supply_label(reg, e->name, input,
                                          c->outputs + k * out_total) == 0) {
                    labeled++;
                    progress = 1;
                }
                break;
            }
            if (progress) break;  /* queue compacted; rescan from the top */
        }
    }
    return labeled;
}

int specialist_health_pass(PrimitiveRegistry *reg,
                           const SpecialistHealthConfig *cfg,
                           SpecialistHealthReport *report) {
    SpecialistHealthConfig defaults;
    SpecialistHealthReport local;
    SpecialistHealthReport *rep = report ? report : &local;
    size_t i, provisional_before = 0;

    if (!reg) return -1;
    if (!cfg) {
        specialist_health_config_defaults(&defaults);
        cfg = &defaults;
    }
    memset(rep, 0, sizeof *rep);
    rep->entries = reg->count;

    /* 1. audit: a certified entry whose weights drifted from its
       certification digest is demoted (certified cleared, RESET). */
    if (cfg->audit)
        rep->demoted_by_audit = registry_audit_certified(reg);

    /* 2. label: verified targets for parked faults. Contract first (exact,
       free), then a teacher for what the contract does not cover. */
    for (i = 0; i < reg->count; i++) {
        const RegistryEntry *e = &reg->entries[i];
        if (!e->queue || e->queue->unlabeled_count == 0) continue;
        if (cfg->label_from_contract && cfg->contracts) {
            const Contract *c = cfg->contracts(e->name, cfg->contracts_ctx);
            if (c)
                rep->labeled_from_contract += label_from_contract(reg, e, c);
        }
        if (cfg->label_via_teacher)
            rep->labeled_via_teacher +=
                registry_label_via_teacher(reg, e->name);
    }

    /* 3. heal: retrain on contract ∪ labeled faults, re-certify. FROZEN only
       on a passing certify (registry_heal's contract); an entry with no
       labeled evidence — or a runtime adapter, which refuses matrix
       retraining — stays RESET for the acquisition loop to rebuild. */
    if (cfg->heal && cfg->contracts) {
        for (i = 0; i < reg->count; i++) {
            const RegistryEntry *e = &reg->entries[i];
            const Contract *c;
            if (e->state != PRIM_RESET) continue;
            c = cfg->contracts(e->name, cfg->contracts_ctx);
            if (!c) continue;
            rep->heal_attempted++;
            if (registry_heal(reg, e->name, c, cfg->heal_max_epochs) == 1)
                rep->healed++;
        }
    }

    /* 4. promote: evidence moves FUZZY to PROVISIONAL; never further. */
    if (cfg->promote) {
        for (i = 0; i < reg->count; i++)
            if (reg->entries[i].state == PRIM_PROVISIONAL)
                provisional_before++;
        lifecycle_promote_provisional(reg, cfg->promote_threshold,
                                      cfg->promote_min_evidence);
        for (i = 0; i < reg->count; i++)
            if (reg->entries[i].state == PRIM_PROVISIONAL)
                rep->promoted_provisional++;
        rep->promoted_provisional -= provisional_before;
    }

    /* 5. shadows: evidence-gated hot swap against the incumbent's contract.
       Promotion mutates entries in place (count is stable), so index
       iteration with values captured before the call is safe. */
    if (cfg->promote_shadows && cfg->contracts) {
        for (i = 0; i < reg->count; i++) {
            const char *shadow_name = reg->entries[i].name;
            const char *active_name = reg->entries[i].shadow_of;
            const Contract *c;
            if (!active_name) continue;
            c = cfg->contracts(active_name, cfg->contracts_ctx);
            if (!c) continue;
            if (shadow_promote_if_ready(reg, shadow_name, c,
                                        cfg->shadow_min_evidence) == 1)
                rep->shadows_promoted++;
        }
    }

    /* Report the end state on the shared axes. */
    for (i = 0; i < reg->count; i++) {
        SpecialistTrust t;
        if (reg->entries[i].state == PRIM_RESET) rep->reset_remaining++;
        if (specialist_axes(reg, reg->entries[i].name, &t, NULL) == 0 &&
            t <= SPECIALIST_TRUST_DEMOTED)
            rep->trust[t]++;
    }
    return 0;
}
