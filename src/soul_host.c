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
#include "../include/gap_lane.h"

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
    /* Gap inbox (CNET_GAP_INBOX env at soul_open; "" = disabled): serving
       no-plans are appended here for the gap lane to ingest — the serving
       process detects, the lane learns. */
    char gap_inbox[512];
    /* Remounted oracle descriptors (soul_mount_oracles). One slot per base
       oracle descriptor, sized ONCE to base.oracle_count (fixed after load) so
       a slot's &bound_entries[i] never moves — the adapter BTN and the live
       registry entry both borrow it. Host-owned; freed exactly once in
       soul_close AFTER registry_free drops the borrows. */
    OracleEntry *bound_entries;              /* oracle_count; borrowed by adapters */
    BinaryTransformNetwork *bound_adapters;  /* oracle_count adapter BTNs (btn_free frees ctx) */
    Contract **bound_contracts;              /* oracle_count recovered sealed contracts (owned) */
    unsigned char *bound_mounted;            /* oracle_count: 1 = live in the registry */
    size_t bound_cap;                        /* == oracle_count once allocated, else 0 */
    size_t mounted_count;
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
    {
        const char *inbox = getenv("CNET_GAP_INBOX");
        if (inbox && inbox[0] && strlen(inbox) < sizeof h->gap_inbox)
            memcpy(h->gap_inbox, inbox, strlen(inbox) + 1);
    }
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

CNET_API int soul_oracle_artifact_sha256(SoulHost *h, int index,
                                         unsigned char out32[32]) {
    if (!h || !h->loaded || !out32 || index < 0 ||
        (size_t)index >= h->base.oracle_count) return -1;
    memcpy(out32, h->base.oracles[(size_t)index].identity.artifact_sha256, 32);
    return 0;
}

CNET_API int soul_unit_provenance(SoulHost *h, const char *name,
                                  char *out, int out_cap) {
    const char *prov;
    if (!h || !h->loaded || !name || !out || out_cap <= 0) return -1;
    prov = cnb_unit_provenance(&h->base, name);
    if (!prov) return -2;
    return soul_copy_descriptor_atom(prov, out, out_cap);
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
    if (route_plan(&h->reg, input, goal, &plan) != 0 || plan.length == 0) {
        /* a serving miss is a knowledge gap: hand it to the lane */
        if (h->gap_inbox[0])
            gap_inbox_note_no_plan(h->gap_inbox, input, goal);
        return -3;
    }
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

static int request_port(Port *p, int family, int width, int count,
                        const char *tag) {
    memset(p, 0, sizeof *p);
    if (family < PORT_RAW || family > PORT_CONCEPT || width <= 0 || count <= 0)
        return -1;
    p->family = (PortFamily)family;
    p->field_width = (size_t)width;
    p->field_count = (size_t)count;
    if (tag && tag[0] && port_set_tag(p, tag) != 0) return -1;
    return 0;
}

CNET_API int soul_request(SoulHost *h,
                          int in_family, int in_width, int in_count,
                          const char *in_tag,
                          int goal_family, int goal_width, int goal_count,
                          const char *goal_tag,
                          const double *in, int in_len,
                          double *out, int out_cap) {
    Port input, goal;
    RoutePlan plan;
    size_t in_total, out_total;

    if (!h || !h->loaded) return -1;
    /* a request needs REAL types on both ends: an untagged input port is a
       wildcard, which trivially "already satisfies" any same-shape goal
       (the 0-length identity) — such a request is unservable by
       construction and would mint an unreachable-by-default unit */
    if (!in_tag || !in_tag[0] || !goal_tag || !goal_tag[0]) return -1;
    if (request_port(&input, in_family, in_width, in_count, in_tag) != 0 ||
        request_port(&goal, goal_family, goal_width, goal_count,
                     goal_tag) != 0)
        return -1;
    in_total = input.field_width * input.field_count;
    out_total = goal.field_width * goal.field_count;

    memset(&plan, 0, sizeof plan);
    /* length 0 = the wildcard-tag identity plan ("input already satisfies
       the goal type") — a capability request wants a PRODUCING unit, so it
       counts as no plan, same rule as soul_route. */
    if (route_plan(&h->reg, input, goal, &plan) != 0 || plan.length == 0) {
        /* an unservable request IS the knowledge gap: hand it to the lane */
        if (h->gap_inbox[0])
            gap_inbox_note_no_plan(h->gap_inbox, input, goal);
        return -3;
    }
    if (!in) return 0;  /* capability probe: plannable, not executed */
    if ((size_t)in_len != in_total || !out ||
        (size_t)out_cap < out_total) return -4;
    if (route_execute(&plan, in, in_total, out, out_total) != 0) return -5;
    return (int)out_total;
}

/* Find a native unit whose recorded provenance points at descriptor
   `oracle_name` and recover its sealed contract into *out (owns_data). The
   oracle taught that unit, so the unit's own sealed exemplars are the evidence
   the remounted adapter must reproduce. Returns 0 with *out filled (caller owns
   it), or -1 if no provenance-linked unit recovers a contract. */
static int soul_recover_provenance_contract(SoulHost *h, const char *oracle_name,
                                            Contract *out) {
    size_t i;
    for (i = 0; i < h->base.unit_count; ++i) {
        if (strcmp(h->base.units[i].provenance, oracle_name) != 0) continue;
        {
            BinaryTransformNetwork tmp;
            memset(&tmp, 0, sizeof tmp);
            if (cnb_get_unit(&h->base, h->base.units[i].name, &tmp, out) == 0) {
                btn_free(&tmp);        /* the native BTN is already live in the registry */
                return 0;
            }
            /* recovery failed on this unit — keep scanning for another teacher */
        }
    }
    return -1;
}

CNET_API int soul_mount_oracles(SoulHost *h, SoulOracleResolver resolver,
                                void *rctx, SoulMountReport *report) {
    SoulMountReport rep;
    size_t i, n;

    memset(&rep, 0, sizeof rep);
    if (!h || !h->loaded) return -1;
    if (!resolver) return -1;   /* descriptor-only projection is soul_open's default */

    n = h->base.oracle_count;
    /* Allocate the stable per-descriptor slots once (never realloc'd, so an
       adapter's borrowed &bound_entries[i] can't dangle). */
    if (n > 0 && h->bound_cap == 0) {
        h->bound_entries = (OracleEntry *)calloc(n, sizeof *h->bound_entries);
        h->bound_adapters =
            (BinaryTransformNetwork *)calloc(n, sizeof *h->bound_adapters);
        h->bound_contracts = (Contract **)calloc(n, sizeof *h->bound_contracts);
        h->bound_mounted = (unsigned char *)calloc(n, sizeof *h->bound_mounted);
        if (!h->bound_entries || !h->bound_adapters || !h->bound_contracts ||
            !h->bound_mounted) {
            free(h->bound_entries);   h->bound_entries = NULL;
            free(h->bound_adapters);  h->bound_adapters = NULL;
            free(h->bound_contracts); h->bound_contracts = NULL;
            free(h->bound_mounted);   h->bound_mounted = NULL;
            return -2;
        }
        h->bound_cap = n;
    }

    for (i = 0; i < n; ++i) {
        const CnbOracleDesc *desc = &h->base.oracles[i];
        SoulOracleBinding b;
        Contract c;
        Contract *hc;
        Specialist s;
        OracleEntry *e;
        uint64_t asserted;
        size_t cost;

        if (h->bound_mounted[i]) continue;   /* idempotent: already live */

        memset(&b, 0, sizeof b);
        if (resolver(desc->name, desc->kind, &b, rctx) != 0 || b.fn == NULL) {
            rep.unbound++;
            continue;
        }
        /* Identity integrity: the sealed descriptor MUST carry a digest and the
           caller MUST assert a matching identity. No digest, no asserted
           identity, or a mismatch is refused — never silent runtime trust. */
        asserted = b.has_identity ? cnet_oracle_identity_digest(&b.identity) : 0;
        if (desc->behavior_digest == 0 || asserted == 0 ||
            asserted != desc->behavior_digest) {
            rep.identity_mismatch++;
            continue;
        }
        /* Provenance evidence: a native unit's OWN sealed contract. */
        memset(&c, 0, sizeof c);
        if (soul_recover_provenance_contract(h, desc->name, &c) != 0) {
            rep.missing_provenance++;
            continue;
        }
        /* Allocate the retained contract before admission. Once the registry
           borrows the adapter there must be no fallible ownership step left. */
        hc = (Contract *)malloc(sizeof *hc);
        if (!hc) {
            contract_free(&c);
            if (report) *report = rep;
            return -2;
        }
        *hc = c;   /* move the owned tables; hc is now the sole owner */
        /* Build the bound entry in its stable slot; the adapter borrows it. */
        e = &h->bound_entries[i];
        memset(e, 0, sizeof *e);
        snprintf(e->name, sizeof e->name, "%s", desc->name);
        e->input_port = desc->input_port;
        e->output_port = desc->goal_port;
        e->fn = b.fn;
        e->ctx = b.ctx;
        e->identity = desc->identity;             /* authoritative sealed identity */
        e->behavior_digest = desc->behavior_digest;

        cost = e->output_port.field_width * e->output_port.field_count;
        /* Wrap as an ORACLE specialist (adapter over the resolved callback) and
           admit through the one door against the recovered sealed contract. The
           base-owned descriptor name is the borrowed registry name (stable for
           the host's life: registry_free runs before cnb_free in soul_close). */
        if (specialist_wrap_oracle(&s, &h->bound_adapters[i], e,
                                   desc->behavior_digest, cost,
                                   desc->name) != 0) {
            contract_free(hc);
            free(hc);
            rep.cert_failed++;
            continue;
        }
        if (specialist_admit(&h->reg, &s, hc) != 0) {
            btn_free(&h->bound_adapters[i]);      /* releases the adapter context */
            memset(&h->bound_adapters[i], 0, sizeof h->bound_adapters[i]);
            contract_free(hc);
            free(hc);
            rep.cert_failed++;
            continue;
        }
        h->bound_contracts[i] = hc;
        h->bound_mounted[i] = 1;
        h->mounted_count++;
        rep.mounted++;
    }

    if (report) *report = rep;
    return rep.mounted;
}

CNET_API int soul_unit_kind(SoulHost *h, const char *name, int *kind) {
    RegistryEntry *entry;
    if (!h || !h->loaded || !name) return -1;
    entry = soul_find_unit(h, name);
    if (!entry) return -2;
    if (kind) *kind = (int)entry->kind;
    return 0;
}

CNET_API int soul_mounted_oracle_count(SoulHost *h) {
    if (!h || !h->loaded || h->mounted_count > (size_t)INT_MAX) return -1;
    return (int)h->mounted_count;
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
    if (h->loaded) {
        /* registry_free first: it drops the registry's borrows of the adapter
           BTNs and their (base-owned) names. Then release each mounted adapter
           (frees its projection context) and the recovered contract it was
           certified against, before cnb_free reclaims the base. */
        registry_free(&h->reg);
        for (i = 0; i < h->bound_cap; ++i) {
            if (h->bound_mounted[i]) btn_free(&h->bound_adapters[i]);
            if (h->bound_contracts[i]) {
                contract_free(h->bound_contracts[i]);
                free(h->bound_contracts[i]);
            }
        }
        free(h->bound_entries);
        free(h->bound_adapters);
        free(h->bound_contracts);
        free(h->bound_mounted);
        cnb_free(&h->base);
    }
    free(h);
}
