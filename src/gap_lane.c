/* The gap lane: detect (inbox + health bridge) -> acquire (drain) ->
 * persist (atomic checkpoints). Pure composition of gated machinery;
 * certification remains the only door and DEFER stays total. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/gap_lane.h"
#include "../include/specialist.h"
#include "../include/specialist_health.h"

/* ---- small helpers ------------------------------------------------------ */

static int copy_path(char *dst, size_t cap, const char *src) {
    size_t need;
    if (!src) { dst[0] = '\0'; return 0; }
    need = strlen(src) + 1;
    if (need > cap) return -1;
    memcpy(dst, src, need);
    return 0;
}

/* Contract cache over the base (same move as soul_host): heal re-certifies
   against the sealed truth the unit was admitted with. */
static const Contract *lane_contract_lookup(const char *name, void *ctx) {
    GapLane *L = (GapLane *)ctx;
    size_t i;
    BinaryTransformNetwork tmp;
    Contract *c;
    if (!L || !name) return NULL;
    for (i = 0; i < L->contract_count; ++i)
        if (strcmp(L->contract_names[i], name) == 0) return L->contracts[i];
    if (strlen(name) + 1 > sizeof L->contract_names[0]) return NULL;
    c = (Contract *)calloc(1, sizeof *c);
    if (!c) return NULL;
    memset(&tmp, 0, sizeof tmp);
    if (cnb_get_unit(&L->base, name, &tmp, c) != 0) { free(c); return NULL; }
    btn_free(&tmp);
    if (L->contract_count == L->contract_cap) {
        size_t cap = L->contract_cap ? L->contract_cap * 2 : 8;
        Contract **nc = (Contract **)realloc(L->contracts, cap * sizeof *nc);
        char (*nn)[CNB_NAME_MAX] = (char (*)[CNB_NAME_MAX])
            realloc(L->contract_names, cap * sizeof *nn);
        if (nc) L->contracts = nc;
        if (nn) L->contract_names = nn;
        if (!nc || !nn) { contract_free(c); free(c); return NULL; }
        L->contract_cap = cap;
    }
    L->contracts[L->contract_count] = c;
    snprintf(L->contract_names[L->contract_count],
             sizeof L->contract_names[0], "%s", name);
    L->contract_count++;
    return c;
}

/* Scan-note guard: state re-observations must not reopen deferred work.
   Note only when the subject has no OPEN or DEFERRED record. */
static int subject_pending(const AcquireLedger *l, const char *subject) {
    size_t i;
    for (i = 0; i < l->count; ++i) {
        const GapRecord *g = &l->gaps[i];
        if (g->status == GAP_CLOSED) continue;
        if (g->subject[0] && strcmp(g->subject, subject) == 0) return 1;
    }
    return 0;
}

int gap_lane_load_ids(const char *path, int *out, int cap) {
    FILE *f;
    char line[64];
    int n = 0;
    if (!path || !path[0] || !out || cap <= 0) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *end;
        long v;
        const char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\n' || *p == '\0' || *p == '\r') continue;
        v = strtol(p, &end, 10);
        if (end == p || v < 0 ||
            (*end != '\n' && *end != '\r' && *end != '\0')) {
            fclose(f);
            return -1;   /* malformed: refuse the whole file */
        }
        if (n == cap) { fclose(f); return -1; }  /* larger than declared */
        out[n++] = (int)v;
    }
    fclose(f);
    return n;
}

int gap_lane_digest_file(const char *path, unsigned long long *out) {
    FILE *f;
    unsigned char *buf;
    size_t n;
    unsigned long long h = 1469598103934665603ULL;   /* FNV-1a basis */
    if (!path || !path[0] || !out) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    buf = (unsigned char *)malloc(1u << 20);
    if (!buf) { fclose(f); return -1; }
    while ((n = fread(buf, 1, 1u << 20, f)) > 0) {
        size_t i;
        for (i = 0; i < n; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
    }
    free(buf);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    *out = h;
    return 0;
}

/* ---- inbox (serving side + lane side) ----------------------------------- */

static void port_write(FILE *f, Port p) {
    fprintf(f, "%d %zu %zu %s", (int)p.family, p.field_width, p.field_count,
            p.tag[0] ? p.tag : "-");
}

int gap_inbox_note_no_plan(const char *inbox_path,
                           Port input_port, Port goal_port) {
    FILE *f;
    if (!inbox_path || !inbox_path[0]) return -1;
    f = fopen(inbox_path, "a");
    if (!f) return -1;
    fprintf(f, "NO_PLAN ");
    port_write(f, input_port);
    fprintf(f, " ");
    port_write(f, goal_port);
    fprintf(f, "\n");
    fclose(f);
    return 0;
}

static int port_parse(const char *s, Port *out, int *consumed) {
    int fam, n = 0;
    unsigned long w, c;
    char tag[PORT_TAG_MAX];
    memset(out, 0, sizeof *out);
    if (sscanf(s, "%d %lu %lu %31s%n", &fam, &w, &c, tag, &n) != 4) return -1;
    if (fam < 0 || fam > PORT_CONCEPT || w == 0 || c == 0) return -1;
    out->family = (PortFamily)fam;
    out->field_width = (size_t)w;
    out->field_count = (size_t)c;
    if (strcmp(tag, "-") != 0) {
        if (port_set_tag(out, tag) != 0) return -1;
    }
    *consumed = n;
    return 0;
}

/* Rename-then-read: appenders racing with the ingest write to a fresh
   inbox file; nothing is ever truncated under a writer. */
static void ingest_inbox(GapLane *L, GapLaneTickReport *r) {
    char work[560];
    char line[512];
    FILE *f;
    if (!L->inbox_path[0]) return;
    snprintf(work, sizeof work, "%s.ingesting", L->inbox_path);
    if (rename(L->inbox_path, work) != 0) return;  /* no inbox this tick */
    f = fopen(work, "r");
    if (!f) { remove(work); return; }
    while (fgets(line, sizeof line, f)) {
        Port in, goal;
        int used = 0, used2 = 0;
        const char *p = line;
        if (strncmp(p, "NO_PLAN ", 8) != 0) { r->inbox_malformed++; continue; }
        p += 8;
        if (port_parse(p, &in, &used) != 0) { r->inbox_malformed++; continue; }
        p += used;
        while (*p == ' ') p++;
        if (port_parse(p, &goal, &used2) != 0) { r->inbox_malformed++; continue; }
        if (acquire_note_no_plan(&L->ledger, in, goal) >= 0)
            r->inbox_ingested++;
        else
            r->inbox_malformed++;
    }
    fclose(f);
    remove(work);
}

/* ---- lifecycle ----------------------------------------------------------- */

int gap_lane_open(GapLane *L, const char *base_path,
                  const char *ledger_path, const char *inbox_path) {
    size_t skipped = 0;
    if (!L || !base_path || !base_path[0] || !ledger_path || !ledger_path[0])
        return -1;
    memset(L, 0, sizeof *L);
    if (copy_path(L->base_path, sizeof L->base_path, base_path) != 0 ||
        copy_path(L->ledger_path, sizeof L->ledger_path, ledger_path) != 0 ||
        copy_path(L->inbox_path, sizeof L->inbox_path, inbox_path) != 0)
        return -2;

    cnb_init(&L->base);
    cnb_load(&L->base, base_path);            /* absent = fresh base */
    registry_init(&L->reg);
    if (cnb_load_registry(&L->base, &L->reg, &skipped) != 0) {
        registry_free(&L->reg);
        cnb_free(&L->base);
        return -3;
    }
    L->reg.require_certified = 1;
    L->reg.lifecycle_enabled = 1;
    acquire_ledger_init(&L->ledger);
    acquire_ledger_load(&L->ledger, ledger_path);   /* absent = empty */
    memset(&L->oracles, 0, sizeof L->oracles);
    acquire_config_defaults(&L->acq);
    L->acq.base = &L->base;                   /* seal into the base */
    L->acq.unit_dir = NULL;
    L->health_pass_enabled = 1;
    /* Reconcile any CLOSED records loaded from an older/incomplete checkpoint
       once an oracle registry is available. */
    L->provenance_dirty = 1;
    L->loaded = 1;
    return 0;
}

int gap_lane_execute(GapLane *L, Port input_port, Port goal_port,
                     const double *input, size_t in_len,
                     double *output, size_t out_cap) {
    if (!L || !L->loaded) return -1;
    return acquire_execute_or_fallback(&L->reg, &L->ledger, &L->oracles,
                                       &L->acq, input_port, goal_port,
                                       input, in_len, output, out_cap);
}

int gap_lane_scan(GapLane *L, GapLaneTickReport *r) {
    GapLaneTickReport local;
    size_t i;
    if (!L || !L->loaded) return -1;
    if (!r) r = &local;
    memset(r, 0, sizeof *r);

    ingest_inbox(L, r);

    if (L->health_pass_enabled) {
        SpecialistHealthConfig cfg;
        SpecialistHealthReport hr;
        specialist_health_config_defaults(&cfg);
        cfg.contracts = lane_contract_lookup;
        cfg.contracts_ctx = L;
        if (specialist_health_pass(&L->reg, &cfg, &hr) == 0)
            r->healed = hr.healed;
    }

    for (i = 0; i < L->reg.count; ++i) {
        const RegistryEntry *e = &L->reg.entries[i];
        if (!e->name || !e->btn) continue;
        if (e->state == PRIM_RESET) {
            if (!subject_pending(&L->ledger, e->name) &&
                acquire_note_health(&L->ledger, e->name, "reset_unhealed") >= 0)
                r->health_noted++;
        } else if (L->low_rel_floor > 0.0) {
            unsigned long ev = e->btn->output_successes + e->btn->output_failures;
            double rel = btn_reliability(e->btn);
            if (ev >= L->low_rel_min_evidence && rel < L->low_rel_floor &&
                !subject_pending(&L->ledger, e->name) &&
                acquire_note_low_reliability(&L->ledger, e->name, rel,
                                             L->low_rel_floor) >= 0)
                r->low_rel_noted++;
        }
    }
    return 0;
}

/* Unit provenance: a CLOSED gap's teaching oracle becomes a persisted
   descriptor in the base — name, kind, ports, and the oracle's identity
   (retrieval_snapshot_digest = the teaching context, config_digest = the
   window) ride with the sealed units and project through soul_oracle_* /
   cnet_list_oracles — and the minted unit points at its descriptor
   DIRECTLY (cnb_set_unit_provenance; projected by soul_unit_provenance).
   Idempotent by descriptor name; a zero identity is not provenance and
   records nothing.

   Each record reconciles once: provenance_done (runtime-only) marks it, so
   a dirty pass touches only fresh closures — the full-ledger walk happens
   once per process (the open() migration pass over older checkpoints). A
   record whose identity is unavailable right now (no live oracle entry)
   stays not-done: the same teacher name re-binding later can still supply
   it. Any write failure leaves the record not-done and propagates. */
static int record_unit_provenance(GapLane *L) {
    size_t g, o, d;
    for (g = 0; g < L->ledger.count; ++g) {
        GapRecord *gap = &L->ledger.gaps[g];
        const OracleEntry *e = NULL;
        int exists = 0;
        if (gap->status != GAP_CLOSED || !gap->oracle[0] ||
            gap->provenance_done) continue;
        for (d = 0; d < L->base.oracle_count; ++d)
            if (strcmp(L->base.oracles[d].name, gap->oracle) == 0)
                { exists = 1; break; }
        if (!exists) {
            for (o = 0; o < L->oracles.count; ++o)
                if (strcmp(L->oracles.entries[o].name, gap->oracle) == 0)
                    { e = &L->oracles.entries[o]; break; }
            if (!e || cnet_oracle_identity_digest(&e->identity) == 0)
                continue;   /* not reconcilable this pass; stays retryable */
            if (cnb_add_oracle_desc_v2(&L->base, e->name, "gap_lane_teacher",
                                       e->input_port, e->output_port,
                                       &e->identity) != 0)
                return -1;
        }
        if (gap->unit[0] && cnb_has_unit(&L->base, gap->unit) &&
            cnb_set_unit_provenance(&L->base, gap->unit, gap->oracle) != 0)
            return -1;
        gap->provenance_done = 1;
    }
    return 0;
}

int gap_lane_drain(GapLane *L, GapLaneTickReport *r) {
    AcquireReport rep;
    if (!L || !L->loaded) return -1;
    memset(&rep, 0, sizeof rep);
    acquire_drain(&L->reg, &L->ledger, &L->oracles, &L->acq, &rep);
    if (r) r->drain = rep;
    if (rep.closed > 0) L->provenance_dirty = 1;
    if (L->provenance_dirty) {
        if (record_unit_provenance(L) != 0) return -2;
        L->provenance_dirty = 0;
    }
    return 0;
}

/* Atomic ledger save: tmp + rename (the base already saves this way). */
static int ledger_save_atomic(const AcquireLedger *l, const char *path) {
    char tmp[560];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (acquire_ledger_save(l, tmp) != 0) return -1;
    if (rename(tmp, path) != 0) { remove(tmp); return -1; }
    return 0;
}

int gap_lane_checkpoint(GapLane *L) {
    if (!L || !L->loaded) return -1;
    if (cnb_save(&L->base, L->base_path) != 0) return -2;
    if (ledger_save_atomic(&L->ledger, L->ledger_path) != 0) return -3;
    return 0;
}

int gap_lane_tick(GapLane *L, GapLaneTickReport *r, int force_checkpoint) {
    GapLaneTickReport local;
    if (!L || !L->loaded) return -1;
    if (!r) r = &local;
    memset(r, 0, sizeof *r);
    if (gap_lane_scan(L, r) != 0) return -2;
    {
        GapLaneTickReport d;
        memset(&d, 0, sizeof d);
        if (gap_lane_drain(L, &d) != 0) return -3;
        r->drain = d.drain;
    }
    if (force_checkpoint || r->inbox_ingested || r->health_noted ||
        r->low_rel_noted || r->healed || r->drain.closed ||
        r->drain.deferred) {
        if (gap_lane_checkpoint(L) != 0) return -4;
        r->checkpointed = 1;
    }
    return 0;
}

void gap_lane_close(GapLane *L) {
    size_t i;
    if (!L || !L->loaded) return;
    for (i = 0; i < L->contract_count; ++i) {
        contract_free(L->contracts[i]);
        free(L->contracts[i]);
    }
    free(L->contracts);
    free(L->contract_names);
    registry_free(&L->reg);
    acquire_ledger_free(&L->ledger);
    cnb_free(&L->base);
    L->loaded = 0;
}
