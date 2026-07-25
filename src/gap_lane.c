/* The gap lane: detect (inbox + health bridge) -> acquire (drain) ->
 * persist (atomic checkpoints). Pure composition of gated machinery;
 * certification remains the only door and DEFER stays total. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/gap_lane.h"
#include "../include/cnet_auto_learn.h"
#include "../include/cnet_charter.h"
#include "../include/specialist.h"
#include "../include/specialist_health.h"
#include "../include/cnet_evidence_bundle.h"

/* ---- small helpers ------------------------------------------------------ */

static void lane_on_gap_close(size_t gap_index, void *ctx);

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

/* ---- SHA-256 (self-contained; FIPS 180-4) for artifact identity -------- */

typedef struct {
    unsigned int h[8];
    unsigned char block[64];
    size_t block_len;
    unsigned long long total_len;
} LaneSha256;

static const unsigned int lane_sha_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static unsigned int lane_rotr(unsigned int x, unsigned n) {
    return (x >> n) | (x << (32u - n));
}

static void lane_sha_init(LaneSha256 *s) {
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->block_len = 0;
    s->total_len = 0;
}

static void lane_sha_compress(LaneSha256 *s, const unsigned char *p) {
    unsigned int w[64], a, b, c, d, e, f, g, h;
    int i;
    for (i = 0; i < 16; i++)
        w[i] = ((unsigned int)p[i * 4] << 24) |
               ((unsigned int)p[i * 4 + 1] << 16) |
               ((unsigned int)p[i * 4 + 2] << 8) |
               (unsigned int)p[i * 4 + 3];
    for (i = 16; i < 64; i++) {
        unsigned int s0 = lane_rotr(w[i - 15], 7) ^ lane_rotr(w[i - 15], 18)
                          ^ (w[i - 15] >> 3);
        unsigned int s1 = lane_rotr(w[i - 2], 17) ^ lane_rotr(w[i - 2], 19)
                          ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (i = 0; i < 64; i++) {
        unsigned int S1 = lane_rotr(e, 6) ^ lane_rotr(e, 11) ^ lane_rotr(e, 25);
        unsigned int ch = (e & f) ^ (~e & g);
        unsigned int t1 = h + S1 + ch + lane_sha_k[i] + w[i];
        unsigned int S0 = lane_rotr(a, 2) ^ lane_rotr(a, 13) ^ lane_rotr(a, 22);
        unsigned int maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned int t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void lane_sha_update(LaneSha256 *s, const unsigned char *p, size_t n) {
    s->total_len += n;
    while (n) {
        size_t take = 64 - s->block_len;
        if (take > n) take = n;
        memcpy(s->block + s->block_len, p, take);
        s->block_len += take;
        p += take;
        n -= take;
        if (s->block_len == 64) { lane_sha_compress(s, s->block); s->block_len = 0; }
    }
}

static void lane_sha_final(LaneSha256 *s, unsigned char out[32]) {
    unsigned long long bits = s->total_len * 8ULL;
    unsigned char pad = 0x80, zero = 0, lenb[8];
    int i;
    lane_sha_update(s, &pad, 1);
    while (s->block_len != 56) lane_sha_update(s, &zero, 1);
    for (i = 0; i < 8; i++) lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    lane_sha_update(s, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4]     = (unsigned char)(s->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)(s->h[i]);
    }
}

int gap_lane_digest_file_full(const char *path, unsigned char sha256_out[32],
                              unsigned long long *trunc_out) {
    FILE *f;
    unsigned char *buf;
    unsigned char hash[32];
    size_t n;
    LaneSha256 s;
    if (!path || !path[0]) return -1;
    f = fopen(path, "rb");
    if (!f) return -1;
    buf = (unsigned char *)malloc(1u << 20);
    if (!buf) { fclose(f); return -1; }
    lane_sha_init(&s);
    while ((n = fread(buf, 1, 1u << 20, f)) > 0)
        lane_sha_update(&s, buf, n);
    free(buf);
    if (ferror(f)) { fclose(f); return -1; }
    fclose(f);
    if (s.total_len == 0) return -1;   /* empty artifact: no identity */
    lane_sha_final(&s, hash);
    if (sha256_out) memcpy(sha256_out, hash, 32);
    if (trunc_out) {
        unsigned long long v = 0;
        int i;
        for (i = 0; i < 8; i++) v = (v << 8) | hash[i];
        *trunc_out = v ? v : 1;   /* 0 is reserved for "no identity" */
    }
    return 0;
}

int gap_lane_digest_file(const char *path, unsigned long long *out) {
    if (!out) return -1;
    return gap_lane_digest_file_full(path, NULL, out);
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
    /* G4: when enforce on, refuse unchartered goal tags (caller may retry). */
    if (cnet_charter_enforce() && goal_port.tag[0] &&
        !cnet_charter_allows(goal_port.tag)) {
        fprintf(stderr, "gap_inbox: charter refuse goal=%s\n", goal_port.tag);
        return -2;
    }
    /* The inbox is the verbatim record of what was requested and could not be
       planned — signatures persist UNREWRITTEN. Teachable normalization
       (CNET_AUTO_LEARN) happens on the ingest side (ingest_inbox), so the lane
       still trains window shapes while the file keeps true provenance, and the
       freeform entry points (cnet_auto_learn_note_text / note_skill) already
       canonicalize before calling here. Rewriting at write time made the lane
       teach a hash-tag window instead of the signature actually requested. */
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
        /* Ingest the signature AS RECORDED. Coalescing in acquire_note_no_plan
           is exact-signature equality, so rewriting here forked every
           structured miss into a phantom w_cur->tk* gap that could never
           coalesce with (or close as) the gap actually requested — the drain
           then deferred the phantom forever. Freeform entries need no rewrite
           at this point either: cnet_auto_learn_note_text / note_skill
           canonicalize before the line is ever written, so teachable lines
           arrive here already teachable. */
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
    int base_exists = 0;
    FILE *probe;
    if (!L || !base_path || !base_path[0] || !ledger_path || !ledger_path[0])
        return -1;
    memset(L, 0, sizeof *L);
    if (copy_path(L->base_path, sizeof L->base_path, base_path) != 0 ||
        copy_path(L->ledger_path, sizeof L->ledger_path, ledger_path) != 0 ||
        copy_path(L->inbox_path, sizeof L->inbox_path, inbox_path) != 0)
        return -2;

    probe = fopen(base_path, "rb");
    if (probe) {
        base_exists = 1;
        fclose(probe);
    } else if (errno != ENOENT) {
        return -3;
    }
    cnb_init(&L->base);
    if (cnb_load(&L->base, base_path) != 0 && base_exists) {
        cnb_free(&L->base);
        return -3;                            /* present corruption is not fresh */
    }
    registry_init_production(&L->reg);
    if (cnb_load_registry(&L->base, &L->reg, &skipped) != 0) {
        registry_free(&L->reg);
        cnb_free(&L->base);
        return -3;
    }
    L->reg.require_certified = 1;
    L->reg.lifecycle_enabled = 1;
    /* Restore digest-bound reliability counters so evidence accumulates across
       restarts instead of resetting to zero (see gap_lane_persist_stats).
       A digest mismatch — the unit was retrained — leaves counters at zero,
       which is the intended retrainer-invalidates rule. */
    {
        size_t si;
        for (si = 0; si < L->reg.count; si++) {
            RegistryEntry *e = &L->reg.entries[si];
            if (e->btn && e->name) (void)cnb_apply_stats(&L->base, e->name, e->btn);
        }
    }
    acquire_ledger_init(&L->ledger);
    acquire_ledger_load(&L->ledger, ledger_path);   /* absent = empty */
    memset(&L->oracles, 0, sizeof L->oracles);
    acquire_config_defaults(&L->acq);
    L->acq.base = &L->base;                   /* seal into the base */
    L->acq.unit_dir = NULL;
    L->acq.on_close = lane_on_gap_close;      /* O(1) reconcile feed */
    L->acq.on_close_ctx = L;
    /* Budgeted self-improve: CNET_LANE_MAX_CLOSURES (0 = unlimited). */
    {
        const char *mc = getenv("CNET_LANE_MAX_CLOSURES");
        if (mc && mc[0]) {
            long v = atol(mc);
            if (v >= 0) L->acq.max_closures_per_drain = (size_t)v;
        }
    }
    L->health_pass_enabled = 1;
    /* One migration/repair scan on the first drain: reconcile CLOSED
       records whose persisted done-mark is absent (v1/v2 ledgers) or
       whose work predates this checkpoint. Steady state is queue-fed. */
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
   window, toolchain_digest = the teaching stack; a teacher identity
   WITHOUT toolchain attestation is not provenance) ride with the sealed
   units and project through soul_oracle_* / cnet_list_oracles — and the
   minted unit points at its descriptor DIRECTLY (cnb_set_unit_provenance;
   projected by soul_unit_provenance).

   Descriptor resolution is IDENTITY-AWARE: a name collision with a
   DIFFERENT identity (e.g. the model artifact swapped under a recurring
   teacher name) mints a versioned descriptor "<name>_i2", "_i3", … — the
   stored identity is never silently reused for a different teacher.
   Exhausting 32 versions under one name fails loudly: that much identity
   churn is an operator signal. Without a live teacher (resume), a record
   links only when the name family is unambiguous — lineage is never
   guessed. A unit's recorded lineage is write-once: an already-chained
   unit is never re-pointed. */

static const CnbOracleDesc *lane_find_desc(const CnetBase *b, const char *n) {
    size_t d;
    for (d = 0; d < b->oracle_count; ++d)
        if (strcmp(b->oracles[d].name, n) == 0) return &b->oracles[d];
    return NULL;
}

/* 1 if `name` is `family` or `family` + "_i<digits>" */
static int lane_desc_in_family(const char *name, const char *family) {
    size_t n = strlen(family);
    const char *p;
    if (strncmp(name, family, n) != 0) return 0;
    if (name[n] == '\0') return 1;
    if (name[n] != '_' || name[n + 1] != 'i' || !name[n + 2]) return 0;
    for (p = name + n + 2; *p; ++p)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* Returns 0 = reconciled (done), 1 = not reconcilable yet (stays queued),
   -1 = hard failure (propagates; record stays queued and retryable). */
static int reconcile_one(GapLane *L, GapRecord *gap) {
    const OracleEntry *e = NULL;
    const CnbOracleDesc *desc;
    char used[CNB_NAME_MAX];
    size_t o;
    unsigned v;

    /* write-once lineage: already chained (and the descriptor is really
       in the base) means fully reconciled */
    if (gap->unit[0]) {
        const char *prov = cnb_unit_provenance(&L->base, gap->unit);
        if (prov && prov[0] && lane_find_desc(&L->base, prov)) {
            gap->provenance_done = 1;
            return 0;
        }
    }

    for (o = 0; o < L->oracles.count; ++o)
        if (strcmp(L->oracles.entries[o].name, gap->oracle) == 0)
            { e = &L->oracles.entries[o]; break; }

    if (e) {
        uint64_t want = cnet_oracle_identity_digest(&e->identity);
        if (want == 0 || e->identity.toolchain_digest == 0)
            return 1;   /* zero/unattested identity is not provenance */
        used[0] = '\0';
        for (v = 1; v <= 32; ++v) {
            char cand[CNB_NAME_MAX];
            if (v == 1)
                snprintf(cand, sizeof cand, "%s", gap->oracle);
            else
                snprintf(cand, sizeof cand, "%.58s_i%u", gap->oracle, v);
            desc = lane_find_desc(&L->base, cand);
            if (!desc) {
                if (cnb_add_oracle_desc_v2(&L->base, cand, "gap_lane_teacher",
                                           e->input_port, e->output_port,
                                           &e->identity) != 0)
                    return -1;
                snprintf(used, sizeof used, "%s", cand);
                break;
            }
            if (desc->behavior_digest == want) {
                snprintf(used, sizeof used, "%s", cand);
                break;
            }
        }
        if (!used[0]) return -1;   /* 32 identities under one name */
    } else {
        /* no live teacher: link only when the family is unambiguous */
        size_t d, matches = 0;
        used[0] = '\0';
        for (d = 0; d < L->base.oracle_count; ++d)
            if (lane_desc_in_family(L->base.oracles[d].name, gap->oracle)) {
                if (++matches == 1)
                    snprintf(used, sizeof used, "%s",
                             L->base.oracles[d].name);
            }
        if (matches == 0) return 1;   /* nothing knowable yet */
        if (matches > 1) {
            gap->provenance_done = 1;  /* ambiguous: done WITHOUT a link —
                                          lineage is never guessed */
            return 0;
        }
    }

    if (gap->unit[0] && cnb_has_unit(&L->base, gap->unit) &&
        cnb_set_unit_provenance(&L->base, gap->unit, used) != 0)
        return -1;
    /* Evidence bundle for the learned unit (sidecar; never blocks admit). */
    if (gap->unit[0] && cnb_has_unit(&L->base, gap->unit)) {
        char store[576];
        CnetEvidenceOpts opts;
        const char *envp = cnet_evidence_store_path_from_env();
        memset(&opts, 0, sizeof opts);
        opts.counterfactual_stability = -1.0f;
        opts.recipe_fp = gap->recipe_fp;
        if (envp && envp[0]) {
            (void)cnet_evidence_record(&L->base, gap->unit, envp, &opts);
        } else if (cnet_evidence_store_path_for_base(L->base_path, store,
                                                     sizeof store) == 0) {
            (void)cnet_evidence_record(&L->base, gap->unit, store, &opts);
        }
    }
    gap->provenance_done = 1;
    return 0;
}

static int lane_enqueue_provenance(GapLane *L, size_t idx) {
    if (L->prov_pending_count == L->prov_pending_cap) {
        size_t cap = L->prov_pending_cap ? L->prov_pending_cap * 2 : 16;
        size_t *np = (size_t *)realloc(L->prov_pending, cap * sizeof *np);
        if (!np) return -1;
        L->prov_pending = np;
        L->prov_pending_cap = cap;
    }
    L->prov_pending[L->prov_pending_count++] = idx;
    return 0;
}

/* acquire's close hook: per-closure O(1) — no ledger rescans */
static void lane_on_gap_close(size_t gap_index, void *ctx) {
    GapLane *L = (GapLane *)ctx;
    if (!L) return;
    if (lane_enqueue_provenance(L, gap_index) != 0)
        L->provenance_dirty = 1;   /* OOM: fall back to a rebuild scan */
}

static int record_unit_provenance(GapLane *L, size_t *reconciled) {
    size_t i, kept = 0;
    int rc = 0;
    if (L->provenance_dirty) {
        /* migration/repair: ONE full scan rebuilds the queue */
        L->prov_pending_count = 0;
        for (i = 0; i < L->ledger.count; ++i) {
            const GapRecord *gap = &L->ledger.gaps[i];
            if (gap->status != GAP_CLOSED || !gap->oracle[0] ||
                gap->provenance_done) continue;
            if (lane_enqueue_provenance(L, i) != 0) return -1;
        }
        L->provenance_dirty = 0;
    }
    for (i = 0; i < L->prov_pending_count; ++i) {
        size_t idx = L->prov_pending[i];
        int one;
        if (rc != 0) { L->prov_pending[kept++] = idx; continue; }
        if (idx >= L->ledger.count) continue;   /* stale index: drop */
        one = reconcile_one(L, &L->ledger.gaps[idx]);
        if (one == 0) { if (reconciled) (*reconciled)++; continue; }
        L->prov_pending[kept++] = idx;          /* 1 or -1: stays queued */
        if (one < 0) rc = -1;
    }
    L->prov_pending_count = kept;
    return rc;
}

int gap_lane_drain(GapLane *L, GapLaneTickReport *r) {
    AcquireReport rep;
    size_t reconciled = 0;
    if (!L || !L->loaded) return -1;
    memset(&rep, 0, sizeof rep);
    acquire_drain(&L->reg, &L->ledger, &L->oracles, &L->acq, &rep);
    if (r) r->drain = rep;
    if (L->provenance_dirty || L->prov_pending_count) {
        int rc = record_unit_provenance(L, &reconciled);
        if (r) r->provenance_reconciled = reconciled;
        if (rc != 0) {
            L->provenance_dirty = 1;   /* failed work retries next drain */
            return -2;
        }
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

/* Persist per-unit reliability counters into the CNB before saving.
 *
 * registry.c increments output_successes/output_failures on every serve, but
 * nothing ever wrote them to disk: cnb_put_stats had no production caller, so
 * every base reported `stats=0`, every evidence record read
 * reliability 0.5 / successes 0 / failures 0, and the lane's own low-reliability
 * heal path (CNET_LANE_LOW_REL, min evidence CNET_LANE_LOW_REL_MIN_EV — see the
 * evidence check further up this file) could never fire because the evidence
 * count restarted at zero on every load. Stats are digest-bound, so a retrained
 * unit's stale counters are rejected on restore rather than carried over.
 */
static void gap_lane_persist_stats(GapLane *L) {
    size_t i;
    for (i = 0; i < L->reg.count; i++) {
        const RegistryEntry *e = &L->reg.entries[i];
        if (!e->btn || !e->name) continue;
        (void)cnb_put_stats(&L->base, e->name, e->btn);
    }
}

int gap_lane_checkpoint(GapLane *L) {
    if (!L || !L->loaded) return -1;
    gap_lane_persist_stats(L);
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
        r->provenance_reconciled = d.provenance_reconciled;
    }
    r->recipe_reopened = r->drain.recipe_reopened;
    if (force_checkpoint || r->inbox_ingested || r->health_noted ||
        r->low_rel_noted || r->healed || r->drain.closed ||
        r->drain.deferred || r->provenance_reconciled || r->recipe_reopened) {
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
    free(L->prov_pending);
    registry_free(&L->reg);
    acquire_ledger_free(&L->ledger);
    cnb_free(&L->base);
    L->loaded = 0;
}
