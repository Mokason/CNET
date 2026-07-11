/* soul_host.c - thin, P/Invoke-friendly C ABI over the CNET certified engine.
 * Loads a .cnb base into a registry; runs named units safely; routes to a
 * unit by its REAL typed ports; reports real reliability. Heavy lifting is
 * the public engine (cnb_*, registry_*, route_*, btn_*), exported from the
 * shared lib (make cnet_dll).
 */

#include "../include/soul_host.h"
#include "../include/base.h"
#include "../include/router.h"
#include "../include/nn.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"
#include "../include/specialist_health.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

struct SoulHost {
    CnetBase base;
    PrimitiveRegistry reg;
    int loaded;
    /* Contract cache for the health tick: rematerialized on demand from the
       sealed unit blobs; one heap Contract per name so pointers stay stable
       across cache growth. Owned by the host, freed in soul_close. */
    Contract **contracts;
    char (*contract_names)[CNB_NAME_MAX];
    size_t contract_count, contract_cap;
};

/* Health-tick contract source: the base itself. A miss materializes the
   sealed unit (per-blob CNU1 seal verified by cnb_get_unit), keeps the
   Contract, and discards the temporary BTN — the registry instance stays
   the single execution truth. */
static const Contract *soul_contract_lookup(const char *name, void *ctx) {
    SoulHost *h = (SoulHost *)ctx;
    size_t i;
    BinaryTransformNetwork tmp;
    Contract *c;
    if (!h || !name) return NULL;
    for (i = 0; i < h->contract_count; ++i)
        if (strcmp(h->contract_names[i], name) == 0) return h->contracts[i];
    if (strlen(name) + 1 > sizeof h->contract_names[0]) return NULL;
    c = (Contract *)calloc(1, sizeof *c);
    if (!c) return NULL;
    memset(&tmp, 0, sizeof tmp);
    if (cnb_get_unit(&h->base, name, &tmp, c) != 0) {
        free(c);
        return NULL;
    }
    btn_free(&tmp);
    if (h->contract_count == h->contract_cap) {
        size_t cap = h->contract_cap ? h->contract_cap * 2 : 8;
        Contract **nc = (Contract **)realloc(h->contracts, cap * sizeof *nc);
        char (*nn)[CNB_NAME_MAX] = (char (*)[CNB_NAME_MAX])
            realloc(h->contract_names, cap * sizeof *nn);
        if (nc) h->contracts = nc;
        if (nn) h->contract_names = nn;
        if (!nc || !nn) { contract_free(c); free(c); return NULL; }
        h->contract_cap = cap;
    }
    h->contracts[h->contract_count] = c;
    snprintf(h->contract_names[h->contract_count],
             sizeof h->contract_names[0], "%s", name);
    h->contract_count++;
    return c;
}

/* Sum of a contract's port totals (field_width * field_count), in doubles. */
static size_t ports_total(const Port *ports, size_t n) {
    size_t t = 0, i;
    for (i = 0; i < n; ++i) t += ports[i].field_width * ports[i].field_count;
    return t;
}

/* The registry is the live source of truth after cnb_load_registry has replayed
   certification. Rematerializing a fresh BTN from CNB per call would split
   execution evidence from the planner's instance. */
static RegistryEntry *soul_find_unit(SoulHost *h, const char *name) {
    size_t i;
    if (!h || !h->loaded || !name) return NULL;
    for (i = 0; i < h->reg.count; ++i) {
        RegistryEntry *entry = &h->reg.entries[i];
        if (entry->btn && entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static void soul_record_result(BinaryTransformNetwork *btn, int success) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    if (success) atomic_fetch_add_explicit(&btn->output_successes, 1, memory_order_relaxed);
    else atomic_fetch_add_explicit(&btn->output_failures, 1, memory_order_relaxed);
#else
    if (success) btn->output_successes++;
    else btn->output_failures++;
#endif
}

CNET_API int soul_open(const char *base_path, const char *model_path,
                       SoulHost **out) {
    (void)model_path;                  /* hybrid model loading is host-side */
    if (!base_path || !out) return -1;

    SoulHost *h = (SoulHost *)calloc(1, sizeof *h);
    if (!h) return -2;

    cnb_init(&h->base);
    if (cnb_load(&h->base, base_path) != 0) {
        cnb_free(&h->base);
        free(h);
        return -3;
    }
    registry_init(&h->reg);
    {
        size_t skipped = 0;
        if (cnb_load_registry(&h->base, &h->reg, &skipped) != 0) {
            registry_free(&h->reg);
            cnb_free(&h->base);
            free(h);
            return -4;
        }
    }
    h->reg.require_certified = 1;
    h->loaded = 1;
    *out = h;
    return 0;
}

CNET_API int soul_unit_count(SoulHost *h) {
    if (!h || !h->loaded || h->reg.count > (size_t)INT_MAX) return -1;
    return (int)h->reg.count;
}

CNET_API int soul_unit_name(SoulHost *h, int index, char *out, int out_cap) {
    const char *name;
    size_t need;
    if (!h || !h->loaded || !out || out_cap <= 0 || index < 0 ||
        (size_t)index >= h->reg.count) return -1;
    name = h->reg.entries[(size_t)index].name;
    if (!name || !h->reg.entries[(size_t)index].btn) return -2;
    need = strlen(name) + 1;
    if (need > (size_t)out_cap) {
        out[0] = '\0';
        return -3;
    }
    memcpy(out, name, need);
    return 0;
}

static int soul_copy_descriptor_atom(const char *value, char *out, int out_cap) {
    size_t need;
    if (!value || !out || out_cap <= 0) return -1;
    need = strlen(value) + 1;
    if (need > (size_t)out_cap) {
        out[0] = '\0';
        return -3;
    }
    memcpy(out, value, need);
    return 0;
}

CNET_API int soul_oracle_count(SoulHost *h) {
    if (!h || !h->loaded || h->base.oracle_count > (size_t)INT_MAX) return -1;
    return (int)h->base.oracle_count;
}

CNET_API int soul_oracle_name(SoulHost *h, int index, char *out, int out_cap) {
    if (!h || !h->loaded || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    return soul_copy_descriptor_atom(h->base.oracles[(size_t)index].name,
                                     out, out_cap);
}

CNET_API int soul_oracle_kind(SoulHost *h, int index, char *out, int out_cap) {
    if (!h || !h->loaded || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    return soul_copy_descriptor_atom(h->base.oracles[(size_t)index].kind,
                                     out, out_cap);
}

CNET_API int soul_oracle_identity(
    SoulHost *h, int index,
    uint64_t *behavior_digest,
    uint64_t *artifact_digest,
    uint64_t *contract_digest,
    uint64_t *config_digest,
    uint64_t *retrieval_snapshot_digest,
    uint64_t *toolchain_digest) {
    const CnbOracleDesc *desc;
    if (!h || !h->loaded || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    desc = &h->base.oracles[(size_t)index];
    if (behavior_digest) *behavior_digest = desc->behavior_digest;
    if (artifact_digest) *artifact_digest = desc->identity.artifact_digest;
    if (contract_digest) *contract_digest = desc->identity.contract_digest;
    if (config_digest) *config_digest = desc->identity.config_digest;
    if (retrieval_snapshot_digest)
        *retrieval_snapshot_digest = desc->identity.retrieval_snapshot_digest;
    if (toolchain_digest) *toolchain_digest = desc->identity.toolchain_digest;
    return 0;
}

CNET_API int soul_unit_dims(SoulHost *h, const char *name,
                            int *in_total, int *out_total) {
    RegistryEntry *entry = soul_find_unit(h, name);
    size_t in_n, out_n;
    if (!entry) return -2;
    in_n = ports_total(entry->btn->input_ports, entry->btn->input_port_count);
    out_n = ports_total(entry->btn->output_ports, entry->btn->output_port_count);
    if (in_n > (size_t)INT_MAX || out_n > (size_t)INT_MAX) return -3;
    if (in_total)  *in_total  = (int)in_n;
    if (out_total) *out_total = (int)out_n;
    return 0;
}

CNET_API int soul_run(SoulHost *h, const char *name,
                      const double *in, double *out, int out_cap) {
    RegistryEntry *entry = soul_find_unit(h, name);
    BinaryTransformNetwork *btn;
    double *clean_in;
    size_t in_total, out_total, n, off, p;
    const double *result;
    int valid = 1;
    if (!entry || !in || !out || out_cap <= 0) return -1;
    btn = entry->btn;
    in_total = ports_total(btn->input_ports, btn->input_port_count);
    out_total = ports_total(btn->output_ports, btn->output_port_count);
    clean_in = malloc(in_total * sizeof *clean_in);
    if (!clean_in) return -3;
    off = 0;
    for (p = 0; p < btn->input_port_count; ++p) {
        size_t width = btn->input_ports[p].field_width * btn->input_ports[p].field_count;
        if (!port_validate(btn->input_ports[p], in + off) ||
            port_canonicalize(btn->input_ports[p], in + off, clean_in + off) != 0) {
            free(clean_in);
            return -4;
        }
        off += width;
    }
    result = btn_forward(btn, clean_in);
    free(clean_in);
    if (!result) return -3;
    off = 0;
    for (p = 0; p < btn->output_port_count; ++p) {
        size_t width = btn->output_ports[p].field_width * btn->output_ports[p].field_count;
        if (!port_validate(btn->output_ports[p], result + off)) valid = 0;
        off += width;
    }
    soul_record_result(btn, valid);

    n = (out_total < (size_t)out_cap) ? out_total : (size_t)out_cap;  /* never overflow */
    memcpy(out, result, n * sizeof(double));
    return (int)out_total;   /* > out_cap signals truncation to the caller */
}

CNET_API int soul_route(SoulHost *h, const char *goal_tag,
                        const double *in, int in_cap, double *out, int out_cap) {
    RegistryEntry *entry;
    BinaryTransformNetwork *btn;
    Port input, goal;
    size_t in_total, out_total;
    char name[CNB_NAME_MAX];
    RoutePlan plan;
    int rc;

    if (!h || !h->loaded || !goal_tag || !in || !out) return -1;

    /* the unit that owns a goal tag is named "acq_<tag>" (flagship convention);
       take its REAL input/output ports instead of fabricating them. */
    snprintf(name, sizeof name, "acq_%s", goal_tag);
    entry = soul_find_unit(h, name);
    if (!entry) return -2;
    btn = entry->btn;
    if (btn->input_port_count < 1 || btn->output_port_count < 1) return -2;
    input = btn->input_ports[0];
    goal  = btn->output_ports[0];
    in_total  = ports_total(&input, 1);
    out_total = ports_total(&goal, 1);

    if ((int)in_total > in_cap || (int)out_total > out_cap) return -4;

    memset(&plan, 0, sizeof plan);
    if (route_plan(&h->reg, input, goal, &plan) != 0 || plan.length == 0) return -3;
    rc = route_execute(&plan, in, in_total, out, out_total);
    return rc == 0 ? (int)out_total : -5;
}

CNET_API int soul_unit_reliability_milli(SoulHost *h, const char *name) {
    RegistryEntry *entry = soul_find_unit(h, name);
    double rel;
    if (!entry) return -2;
    rel = btn_reliability(entry->btn);
    return (int)(rel * 1000.0 + 0.5);
}

CNET_API int soul_health_tick(SoulHost *h, long long *counts, int counts_cap) {
    SpecialistHealthConfig cfg;
    SpecialistHealthReport rep;
    long long full[SOUL_HEALTH_COUNTS];
    int i, n;
    if (!h || !h->loaded || !counts || counts_cap <= 0) return -1;
    specialist_health_config_defaults(&cfg);
    cfg.contracts = soul_contract_lookup;
    cfg.contracts_ctx = h;
    if (specialist_health_pass(&h->reg, &cfg, &rep) != 0) return -2;
    full[0] = (long long)rep.entries;
    full[1] = (long long)rep.demoted_by_audit;
    full[2] = (long long)rep.labeled_from_contract;
    full[3] = (long long)rep.labeled_via_teacher;
    full[4] = (long long)rep.heal_attempted;
    full[5] = (long long)rep.healed;
    full[6] = (long long)rep.promoted_provisional;
    full[7] = (long long)rep.shadows_promoted;
    full[8] = (long long)rep.reset_remaining;
    for (i = 0; i < 4; ++i) full[9 + i] = (long long)rep.trust[i];
    n = counts_cap < SOUL_HEALTH_COUNTS ? counts_cap : SOUL_HEALTH_COUNTS;
    for (i = 0; i < n; ++i) counts[i] = full[i];
    return n;
}

CNET_API int soul_unit_axes(SoulHost *h, const char *name,
                            int *trust, int *role) {
    SpecialistTrust t;
    SpecialistRole r;
    if (!h || !h->loaded || !name) return -1;
    if (specialist_axes(&h->reg, name, &t, &r) != 0) return -2;
    if (trust) *trust = (int)t;
    if (role) *role = (int)r;
    return 0;
}

CNET_API void soul_close(SoulHost *h) {
    size_t i;
    if (!h) return;
    for (i = 0; i < h->contract_count; ++i) {
        contract_free(h->contracts[i]);
        free(h->contracts[i]);
    }
    free(h->contracts);
    free(h->contract_names);
    if (h->loaded) { registry_free(&h->reg); cnb_free(&h->base); }
    free(h);
}
