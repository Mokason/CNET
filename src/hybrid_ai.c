#include "../include/hybrid_ai.h"
#include "../include/external_teacher.h"
#include "../include/cnet_lfru.h"
#include "../include/residual_gguf.h"
#include "../include/base.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t port_tot(Port p) {
    return p.field_width * p.field_count;
}

/* FNV-1a 64 of port shape + tag — cheap reject before full match. */
static uint64_t port_key(Port p) {
    uint64_t h = 14695981039346656037ULL;
    const unsigned char *t = (const unsigned char *)p.tag;
    h ^= (uint64_t)(unsigned)p.family;
    h *= 1099511628211ULL;
    h ^= (uint64_t)p.field_width;
    h *= 1099511628211ULL;
    h ^= (uint64_t)p.field_count;
    h *= 1099511628211ULL;
    for (; *t; t++) {
        h ^= (uint64_t)*t;
        h *= 1099511628211ULL;
    }
    return h ? h : 1ULL;
}

/* Empty tag is a wildcard. Keys (when both tags set) reject before strcmp. */
static int port_match_keys(Port a, Port b, uint64_t ka, uint64_t kb) {
    if (a.family != b.family || a.field_width != b.field_width ||
        a.field_count != b.field_count)
        return 0;
    if (a.tag[0] && b.tag[0]) {
        if (ka != kb) return 0;
        return strcmp(a.tag, b.tag) == 0;
    }
    return 1;
}

static double top_margin(const double *v, size_t n) {
    double t1 = -1e300, t2 = -1e300;
    size_t i;
    if (!v || n == 0) return 0.0;
    if (n == 1) return v[0];
    for (i = 0; i < n; i++) {
        double x = v[i];
        if (x > t1) {
            t2 = t1;
            t1 = x;
        } else if (x > t2) {
            t2 = x;
        }
    }
    return t1 - t2;
}

void hybrid_ai_init(HybridAi *h) {
    if (!h) return;
    memset(h, 0, sizeof *h);
}

void hybrid_ai_free(HybridAi *h) {
    size_t i;
    if (!h) return;
    for (i = 0; i < h->trace_count; i++) {
        free(h->traces[i].in);
        free(h->traces[i].out);
        free(h->traces[i].res_in);
        free(h->traces[i].res_out);
    }
    for (i = 0; i < h->coverage_count; i++) {
        free(h->coverage[i].rows);
        free(h->coverage[i].targets);
    }
    memset(h, 0, sizeof *h);
}

/* K distinct real pairs retained per port shape. */
static size_t reservoir_cap_env(void) {
    const char *e = getenv("CNET_RESIDUAL_RESERVOIR_K");
    if (e && e[0]) {
        long v = atol(e);
        if (v >= 1 && v <= 1024) return (size_t)v;
    }
    return HYBRID_RESERVOIR_K;
}

/* Retain a real (in,out) pair for this port shape.
 *
 * Same input seen again => refresh its target with the newer teacher label
 * rather than storing a duplicate, so the reservoir stays a set of DISTINCT
 * real inputs. Full => FIFO-evict the oldest. Allocation failure is not fatal:
 * the trace still works, the miner just falls back to synthetic expansion. */
static void reservoir_offer(HybridTrace *tr, const double *in, size_t in_dim,
                            const double *out, size_t out_dim) {
    size_t i, slot;
    if (!tr || !in || !out || in_dim != tr->in_dim || out_dim != tr->out_dim)
        return;
    tr->res_offered++;
    if (!tr->res_in || !tr->res_out) {
        size_t cap = reservoir_cap_env();
        double *ri = (double *)calloc(cap * in_dim, sizeof(double));
        double *ro = (double *)calloc(cap * out_dim, sizeof(double));
        if (!ri || !ro) {
            free(ri);
            free(ro);
            return;
        }
        tr->res_in = ri;
        tr->res_out = ro;
        tr->res_cap = cap;
        tr->res_count = 0;
        tr->res_next = 0;
    }
    for (i = 0; i < tr->res_count; i++) {
        if (memcmp(tr->res_in + i * in_dim, in, in_dim * sizeof(double)) == 0) {
            memcpy(tr->res_out + i * out_dim, out, out_dim * sizeof(double));
            return; /* distinct-input set: refresh label, do not grow */
        }
    }
    if (tr->res_count < tr->res_cap) {
        slot = tr->res_count++;
    } else {
        slot = tr->res_next;
        tr->res_next = (tr->res_next + 1) % tr->res_cap;
    }
    memcpy(tr->res_in + slot * in_dim, in, in_dim * sizeof(double));
    memcpy(tr->res_out + slot * out_dim, out, out_dim * sizeof(double));
}

/* Exact membership is only meaningful where inputs are discrete and
   canonicalised. PORT_RAW carries continuous values that never match bitwise,
   so gating it would abstain on everything. */
static int coverage_family_gated(Port p) {
    return p.family == PORT_ONEHOT || p.family == PORT_BINARY_MSB ||
           p.family == PORT_BINARY_LSB;
}

static HybridCoverage *coverage_find(HybridAi *h, uint64_t ik, uint64_t gk,
                                     Port in_port, Port out_port) {
    size_t i;
    for (i = 0; i < h->coverage_count; i++) {
        HybridCoverage *c = &h->coverage[i];
        if (!c->active || c->in_key != ik || c->goal_key != gk) continue;
        if (!port_match_keys(c->input_port, in_port, c->in_key, ik) ||
            !port_match_keys(c->goal_port, out_port, c->goal_key, gk))
            continue;
        return c;
    }
    return NULL;
}

int hybrid_coverage_record(HybridAi *h, Port in_port, Port out_port,
                           const char *unit, const double *inputs,
                           const double *targets, size_t n_rows, size_t in_dim,
                           size_t out_dim) {
    HybridCoverage *c;
    uint64_t ik, gk;
    double *rows, *tgts = NULL;
    if (!h || !unit || !inputs || n_rows == 0 || in_dim == 0) return -1;
    ik = port_key(in_port);
    gk = port_key(out_port);
    rows = (double *)malloc(n_rows * in_dim * sizeof(double));
    if (!rows) return -2;
    memcpy(rows, inputs, n_rows * in_dim * sizeof(double));
    /* Labels are optional: a record restored from the sidecar carries inputs
       only (it gates, it does not re-seal). A fresh mine carries both. */
    if (targets && out_dim > 0) {
        tgts = (double *)malloc(n_rows * out_dim * sizeof(double));
        if (!tgts) {
            free(rows);
            return -2;
        }
        memcpy(tgts, targets, n_rows * out_dim * sizeof(double));
    }
    c = coverage_find(h, ik, gk, in_port, out_port);
    if (!c) {
        if (h->coverage_count >= HYBRID_COVERAGE_MAX) {
            free(rows);
            free(tgts);
            return -3;
        }
        c = &h->coverage[h->coverage_count++];
        memset(c, 0, sizeof *c);
    } else {
        free(c->rows); /* a re-mine supersedes the older certified domain */
        free(c->targets);
    }
    c->input_port = in_port;
    c->goal_port = out_port;
    c->in_key = ik;
    c->goal_key = gk;
    snprintf(c->unit, sizeof c->unit, "%s", unit);
    c->rows = rows;
    c->targets = tgts;
    c->n_rows = n_rows;
    c->in_dim = in_dim;
    c->out_dim = tgts ? out_dim : 0;
    c->active = 1;
    return 0;
}

int hybrid_seal_mined_unit(HybridAi *h, struct CnetBase *base,
                           BinaryTransformNetwork *stu, int *reused_out) {
    const HybridCoverage *c;
    Contract ct;
    int rc, reused = 0;
    if (!h || !base || !stu) return -1;
    if (stu->input_port_count < 1 || stu->output_port_count < 1) return -1;
    c = coverage_find(h, port_key(stu->input_ports[0]),
                      port_key(stu->output_ports[0]), stu->input_ports[0],
                      stu->output_ports[0]);
    /* No record, or a record restored from disk without labels: nothing to
       seal from. Never invent rows — that is the bug this function replaces. */
    if (!c || !c->rows || !c->targets || c->n_rows == 0) return 1;
    memset(&ct, 0, sizeof ct);
    if (contract_init_borrowed(&ct, c->unit, stu, c->rows, c->targets,
                               c->n_rows) != 0)
        return -2;
    rc = cnb_add_unit(base, stu, &ct, &reused);
    contract_free(&ct);
    if (rc != 0) return -3;
    if (reused_out) *reused_out = reused;
    return 0;
}

int hybrid_coverage_admits(const HybridAi *h, Port in_port, Port out_port,
                           const double *in, size_t in_len) {
    const HybridCoverage *c;
    size_t i;
    if (!h || !in) return 1;
    if (!coverage_family_gated(in_port)) return 1;
    c = coverage_find((HybridAi *)h, port_key(in_port), port_key(out_port),
                      in_port, out_port);
    if (!c || !c->rows || c->in_dim != in_len) return 1; /* default-allow */
    for (i = 0; i < c->n_rows; i++) {
        if (memcmp(c->rows + i * c->in_dim, in,
                   c->in_dim * sizeof(double)) == 0)
            return 1;
    }
    return 0;
}

int hybrid_unit_is_mined(const char *unit) {
    static const char pfx[] = HYBRID_MINED_UNIT_PREFIX;
    if (!unit) return 0;
    return strncmp(unit, pfx, sizeof pfx - 1) == 0;
}

void hybrid_coverage_arm_fail_closed(HybridAi *h, int on) {
    if (h) h->coverage_fail_closed = on ? 1 : 0;
}

int hybrid_coverage_has_unit(const HybridAi *h, const char *unit) {
    size_t i;
    if (!h || !unit || !unit[0]) return 0;
    for (i = 0; i < h->coverage_count; i++) {
        if (h->coverage[i].active && h->coverage[i].rows &&
            strcmp(h->coverage[i].unit, unit) == 0)
            return 1;
    }
    return 0;
}

const char *hybrid_coverage_owner(const HybridAi *h, Port in_port,
                                  Port out_port) {
    const HybridCoverage *c;
    if (!h) return NULL;
    c = coverage_find((HybridAi *)h, port_key(in_port), port_key(out_port),
                      in_port, out_port);
    return (c && c->active && c->rows) ? c->unit : NULL;
}

/* Reclaims the slot rather than stranding it: zeroing in place left
   coverage_count high, so repeated failed imports could exhaust a fixed-size
   registry that was in fact empty. The last record is moved into the hole and
   the count drops, which keeps the array dense and preserves every remaining
   record. Callers must not hold a HybridCoverage* (or a name returned by
   hybrid_coverage_owner) across this call. */
int hybrid_coverage_forget_unit(HybridAi *h, const char *unit) {
    size_t i;
    if (!h || !unit || !unit[0]) return 0;
    for (i = 0; i < h->coverage_count; i++) {
        HybridCoverage *c = &h->coverage[i];
        if (!c->active || strcmp(c->unit, unit) != 0) continue;
        free(c->rows);
        free(c->targets);
        if (i + 1 < h->coverage_count)
            *c = h->coverage[h->coverage_count - 1];
        memset(&h->coverage[h->coverage_count - 1], 0, sizeof h->coverage[0]);
        h->coverage_count--;
        return 1;
    }
    return 0;
}

size_t hybrid_coverage_count(const HybridAi *h) {
    size_t i, n = 0;
    if (!h) return 0;
    for (i = 0; i < h->coverage_count; i++)
        if (h->coverage[i].active) n++;
    return n;
}

int hybrid_coverage_admits_unit(const HybridAi *h, const char *unit,
                                const double *in, size_t in_len) {
    size_t i, r;
    if (!h || !unit || !unit[0] || !in) return 1;
    for (i = 0; i < h->coverage_count; i++) {
        const HybridCoverage *c = &h->coverage[i];
        if (!c->active || !c->rows) continue;
        if (strcmp(c->unit, unit) != 0) continue;
        if (!coverage_family_gated(c->input_port)) return 1;
        if (c->in_dim != in_len) return 1;
        for (r = 0; r < c->n_rows; r++) {
            if (memcmp(c->rows + r * c->in_dim, in,
                       c->in_dim * sizeof(double)) == 0)
                return 1;
        }
        return 0;
    }
    /* No record. A mined unit MUST have one, so when fail-closed is armed its
       absence means the guard was lost (deleted or corrupt sidecar), not that
       the unit is unrestricted — refuse rather than serve it blind. */
    if (h->coverage_fail_closed && hybrid_unit_is_mined(unit)) return 0;
    return 1; /* hand-admitted / full-domain unit — default-allow */
}

size_t hybrid_coverage_rows(const HybridAi *h, Port in_port, Port out_port) {
    const HybridCoverage *c;
    if (!h) return 0;
    c = coverage_find((HybridAi *)h, port_key(in_port), port_key(out_port),
                      in_port, out_port);
    return c ? c->n_rows : 0;
}

/* Port tags are identifiers at every call site; whitespace would break the
   token-based reader, so fold it rather than emit a file we cannot parse back.
   Empty tags travel as "~" (a wildcard tag is meaningful — see port_compatible). */
static void coverage_tag_out(const char *tag, char *out, size_t cap) {
    size_t i;
    if (!tag || !tag[0]) {
        snprintf(out, cap, "~");
        return;
    }
    snprintf(out, cap, "%s", tag);
    for (i = 0; out[i]; i++)
        if (out[i] == ' ' || out[i] == '\t' || out[i] == '\n') out[i] = '_';
}

static void coverage_tag_in(const char *tok, char *out, size_t cap) {
    if (!strcmp(tok, "~")) {
        out[0] = '\0';
        return;
    }
    snprintf(out, cap, "%s", tok);
}

int hybrid_coverage_save(const HybridAi *h, const char *path) {
    FILE *fp;
    size_t i, r, j;
    char tmp[576];
    /* Write-then-rename: truncating the live file in place means a crash or a
       full disk mid-write leaves a corrupt sidecar, which loads as "no
       coverage" and silently reopens the confident-wrong hole. rename(2) over
       the same directory is atomic, so a reader sees either the old complete
       file or the new one. */
    if (!h || !path || !path[0]) return -1;
    if (strlen(path) + 5 >= sizeof tmp) return -1;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    fp = fopen(tmp, "w");
    if (!fp) return -2;
    fprintf(fp, "CNET_COVERAGE v1\n");
    for (i = 0; i < h->coverage_count; i++) {
        const HybridCoverage *c = &h->coverage[i];
        char itag[PORT_TAG_MAX + 4], gtag[PORT_TAG_MAX + 4];
        if (!c->active || !c->rows) continue;
        coverage_tag_out(c->input_port.tag, itag, sizeof itag);
        coverage_tag_out(c->goal_port.tag, gtag, sizeof gtag);
        fprintf(fp, "U %zu %zu %d %zu %zu %d %zu %zu %s %s %s\n",
                c->n_rows, c->in_dim, (int)c->input_port.family,
                c->input_port.field_width, c->input_port.field_count,
                (int)c->goal_port.family, c->goal_port.field_width,
                c->goal_port.field_count, itag, gtag,
                c->unit[0] ? c->unit : "~");
        for (r = 0; r < c->n_rows; r++) {
            fputc('R', fp);
            for (j = 0; j < c->in_dim; j++)
                fprintf(fp, " %.17g", c->rows[r * c->in_dim + j]);
            fputc('\n', fp);
        }
    }
    if (fflush(fp) != 0) {
        fclose(fp);
        remove(tmp);
        return -3;
    }
    if (fclose(fp) != 0) {
        remove(tmp);
        return -3;
    }
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -4;
    }
    return 0;
}

int hybrid_coverage_load(HybridAi *h, const char *path) {
    FILE *fp;
    char tok[64];
    int ver = 0;
    if (!h || !path || !path[0]) return -1;
    fp = fopen(path, "r");
    if (!fp) return 0; /* nothing mined yet is not an error */
    if (fscanf(fp, "%15s v%d", tok, &ver) != 2 ||
        strcmp(tok, "CNET_COVERAGE") != 0 || ver != 1) {
        fclose(fp);
        fprintf(stderr, "hybrid: unreadable coverage file %s — mined units "
                        "will default-allow until the next mine\n", path);
        return -4;
    }
    for (;;) {
        size_t n_rows, in_dim, iw, ic, gw, gc, r, j;
        int ifam, gfam;
        char itag[PORT_TAG_MAX], gtag[PORT_TAG_MAX], unit[80];
        Port pin, pout;
        double *rows;
        if (fscanf(fp, " %15s", tok) != 1) break; /* clean EOF */
        if (strcmp(tok, "U") != 0) break;
        if (fscanf(fp, " %zu %zu %d %zu %zu %d %zu %zu %31s %31s %79s",
                   &n_rows, &in_dim, &ifam, &iw, &ic, &gfam, &gw, &gc,
                   itag, gtag, unit) != 11)
            break;
        if (n_rows == 0 || in_dim == 0 || in_dim > 1u << 20 ||
            n_rows > 1u << 20 || n_rows > SIZE_MAX / in_dim)
            break;
        rows = (double *)calloc(n_rows * in_dim, sizeof(double));
        if (!rows) break;
        for (r = 0; r < n_rows; r++) {
            if (fscanf(fp, " %15s", tok) != 1 || strcmp(tok, "R") != 0) {
                free(rows);
                rows = NULL;
                break;
            }
            for (j = 0; j < in_dim; j++) {
                if (fscanf(fp, " %lf", &rows[r * in_dim + j]) != 1) {
                    free(rows);
                    rows = NULL;
                    break;
                }
            }
            if (!rows) break;
        }
        if (!rows) break;
        memset(&pin, 0, sizeof pin);
        memset(&pout, 0, sizeof pout);
        pin.family = (PortFamily)ifam;
        pin.field_width = iw;
        pin.field_count = ic;
        coverage_tag_in(itag, pin.tag, sizeof pin.tag);
        pout.family = (PortFamily)gfam;
        pout.field_width = gw;
        pout.field_count = gc;
        coverage_tag_in(gtag, pout.tag, sizeof pout.tag);
        (void)hybrid_coverage_record(h, pin, pout,
                                     strcmp(unit, "~") ? unit : "restored",
                                     rows, NULL, n_rows, in_dim, 0);
        free(rows);
    }
    fclose(fp);
    return 0;
}

size_t hybrid_reservoir_rows(const HybridAi *h) {
    size_t i, n = 0;
    if (!h) return 0;
    for (i = 0; i < h->trace_count; i++) n += h->traces[i].res_count;
    return n;
}

size_t hybrid_reservoir_rows_for(const HybridAi *h, Port in_port,
                                 Port out_port) {
    size_t i;
    uint64_t ik, gk;
    if (!h) return 0;
    ik = port_key(in_port);
    gk = port_key(out_port);
    for (i = 0; i < h->trace_count; i++) {
        const HybridTrace *tr = &h->traces[i];
        if (tr->in_key != ik || tr->goal_key != gk) continue;
        if (!port_match_keys(tr->input_port, in_port, tr->in_key, ik) ||
            !port_match_keys(tr->goal_port, out_port, tr->goal_key, gk))
            continue;
        return tr->res_count;
    }
    return 0;
}

static const char *const k_tier_names[] = {
    "A_certified", "B_soft", "C_residual"
};
static const char *const k_trust_names[] = {
    "certified", "provisional", "uncertified"
};

const char *hybrid_tier_name(HybridTier t) {
    unsigned u = (unsigned)t;
    if (u < sizeof k_tier_names / sizeof k_tier_names[0])
        return k_tier_names[u];
    return "unknown";
}

const char *hybrid_trust_name(HybridTrust t) {
    unsigned u = (unsigned)t;
    if (u < sizeof k_trust_names / sizeof k_trust_names[0])
        return k_trust_names[u];
    return "unknown";
}

int hybrid_bind_soft(HybridAi *h, const char *name, Port in, Port out,
                     CnetOracleFn fn, void *ctx, double min_margin) {
    HybridSoftSlot *s;
    if (!h || !name || !fn || h->soft_count >= HYBRID_SOFT_MAX) return -1;
    s = &h->soft[h->soft_count++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->input_port = in;
    s->output_port = out;
    s->in_key = port_key(in);
    s->out_key = port_key(out);
    s->fn = fn;
    s->ctx = ctx;
    s->min_margin = min_margin;
    s->enabled = 1;
    return 0;
}

int hybrid_try_soft(HybridAi *h, Port in_port, Port out_port,
                    const double *in, size_t in_len,
                    double *out, size_t out_cap,
                    char *name_out, size_t name_cap) {
    size_t i, out_dim = port_tot(out_port);
    uint64_t ik, ok;
    int any = 0;
    if (!h || !in || !out || in_len != port_tot(in_port) || out_cap < out_dim)
        return -1;
    if (out_dim > 256) return -1;
    ik = port_key(in_port);
    ok = port_key(out_port);
    for (i = 0; i < h->soft_count; i++) {
        HybridSoftSlot *s = &h->soft[i];
        double tmp[256];
        int rc;
        if (!s->enabled) continue;
        if (s->in_key != ik || s->out_key != ok) continue;
        if (!port_match_keys(s->input_port, in_port, s->in_key, ik) ||
            !port_match_keys(s->output_port, out_port, s->out_key, ok))
            continue;
        any = 1;
        rc = s->fn(in, tmp, s->ctx);
        if (rc > 0) {
            h->soft_abstains++;
            continue;
        }
        if (rc < 0) continue;
        if (s->min_margin > 0.0 && top_margin(tmp, out_dim) < s->min_margin) {
            h->soft_abstains++;
            continue;
        }
        memcpy(out, tmp, out_dim * sizeof(double));
        h->tier_b_hits++;
        if (name_out && name_cap)
            snprintf(name_out, name_cap, "%s", s->name);
        return 0;
    }
    return any ? 1 : -1;
}

int hybrid_bind_residual(HybridAi *h, const char *name, CnetOracleFn fn,
                         void *ctx) {
    if (!h || !fn) return -1;
    memset(&h->residual, 0, sizeof h->residual);
    snprintf(h->residual.name, sizeof h->residual.name, "%s",
             name ? name : "residual");
    h->residual.fn = fn;
    h->residual.ctx = ctx;
    h->residual.bound = 1;
    return 0;
}

int hybrid_try_residual(HybridAi *h, Port in_port, Port out_port,
                        const double *in, size_t in_len,
                        double *out, size_t out_cap) {
    size_t out_dim = port_tot(out_port);
    int rc;
    if (!h || !h->residual.bound || !in || !out) return -1;
    if (in_len != port_tot(in_port) || out_cap < out_dim) return -1;
    rc = h->residual.fn(in, out, h->residual.ctx);
    if (rc != 0) return -1;
    hybrid_adapter_apply(h, out, out_dim);
    h->tier_c_hits++;
    (void)hybrid_trace_residual(h, in_port, out_port, in, in_len, out, out_dim);
    return 0;
}

int hybrid_adapter_enable(HybridAi *h, const double *bias, size_t dim,
                          double scale) {
    size_t i;
    if (!h || !bias || dim == 0 || dim > HYBRID_ADAPTER_DIM) return -1;
    memset(&h->adapter, 0, sizeof h->adapter);
    h->adapter.dim = dim;
    h->adapter.scale = scale;
    for (i = 0; i < dim; i++) h->adapter.bias[i] = bias[i];
    h->adapter.enabled = (scale != 0.0);
    return 0;
}

void hybrid_adapter_apply(HybridAi *h, double *out, size_t out_dim) {
    size_t i, n;
    if (!h || !out || !h->adapter.enabled) return;
    n = out_dim < h->adapter.dim ? out_dim : h->adapter.dim;
    for (i = 0; i < n; i++)
        out[i] += h->adapter.scale * h->adapter.bias[i];
    h->adapter_applies++;
}

int hybrid_bind_medium(HybridAi *h, CnetResourceGovernor *gov,
                       const char *name, Port in, Port out,
                       CnetOracleFn fn, void *ctx, uint64_t resident_bytes) {
    HybridMediumSlot *m;
    if (!h || !name || !fn || h->medium_count >= HYBRID_MED_MAX) return -1;
    if (gov && resident_bytes) {
        if (cnet_gov_admit(gov, resident_bytes, 0) != CNET_GOV_OK)
            return -2;
    }
    m = &h->medium[h->medium_count++];
    memset(m, 0, sizeof *m);
    snprintf(m->name, sizeof m->name, "%s", name);
    m->input_port = in;
    m->output_port = out;
    m->in_key = port_key(in);
    m->out_key = port_key(out);
    m->fn = fn;
    m->ctx = ctx;
    m->resident_bytes = resident_bytes;
    m->enabled = 1;
    h->medium_resident_bytes += resident_bytes;
    return 0;
}

int hybrid_try_medium(HybridAi *h, Port in_port, Port out_port,
                      const double *in, size_t in_len,
                      double *out, size_t out_cap) {
    size_t i, out_dim = port_tot(out_port);
    uint64_t ik, ok;
    if (!h || !in || !out || in_len != port_tot(in_port) || out_cap < out_dim)
        return -1;
    ik = port_key(in_port);
    ok = port_key(out_port);
    for (i = 0; i < h->medium_count; i++) {
        HybridMediumSlot *m = &h->medium[i];
        if (!m->enabled) continue;
        if (m->in_key != ik || m->out_key != ok) continue;
        if (!port_match_keys(m->input_port, in_port, m->in_key, ik) ||
            !port_match_keys(m->output_port, out_port, m->out_key, ok))
            continue;
        if (m->fn(in, out, m->ctx) != 0) continue;
        h->tier_b_hits++;
        return 0;
    }
    return -1;
}

int hybrid_distill_plan(HybridAi *h, PrimitiveRegistry *reg,
                        const RoutePlan *plan, CnetResourceGovernor *gov,
                        BinaryTransformNetwork **chunk_out) {
    SelfImproveReport rep;
    int rc;
    if (!h || !reg || !plan) return -1;
    memset(&rep, 0, sizeof rep);
    rc = self_improve_distill_route(reg, plan, NULL, NULL, gov, chunk_out,
                                    &rep);
    if (rc == 0) h->distills++;
    return rc;
}

int hybrid_trace_residual(HybridAi *h, Port in_port, Port out_port,
                          const double *in, size_t in_dim,
                          const double *out, size_t out_dim) {
    size_t i;
    HybridTrace *tr;
    uint64_t ik, gk;
    if (!h || !in || !out || in_dim == 0 || out_dim == 0) return -1;
    ik = port_key(in_port);
    gk = port_key(out_port);
    for (i = 0; i < h->trace_count; i++) {
        tr = &h->traces[i];
        if (tr->in_key != ik || tr->goal_key != gk) continue;
        if (tr->in_dim != in_dim || tr->out_dim != out_dim) continue;
        if (!port_match_keys(tr->input_port, in_port, tr->in_key, ik) ||
            !port_match_keys(tr->goal_port, out_port, tr->goal_key, gk))
            continue;
        memcpy(tr->in, in, in_dim * sizeof(double));
        memcpy(tr->out, out, out_dim * sizeof(double));
        reservoir_offer(tr, in, in_dim, out, out_dim);
        tr->hits++;
        if (tr->heat < 0xffffff00u) tr->heat++;
        tr->last_tick = ++h->heat_clock;
        return 0;
    }
    if (h->trace_count >= HYBRID_TRACE_MAX) return -2;
    tr = &h->traces[h->trace_count++];
    memset(tr, 0, sizeof *tr);
    tr->input_port = in_port;
    tr->goal_port = out_port;
    tr->in_key = ik;
    tr->goal_key = gk;
    tr->in_dim = in_dim;
    tr->out_dim = out_dim;
    tr->in = (double *)malloc(in_dim * sizeof(double));
    tr->out = (double *)malloc(out_dim * sizeof(double));
    if (!tr->in || !tr->out) {
        free(tr->in);
        free(tr->out);
        h->trace_count--;
        return -3;
    }
    memcpy(tr->in, in, in_dim * sizeof(double));
    memcpy(tr->out, out, out_dim * sizeof(double));
    reservoir_offer(tr, in, in_dim, out, out_dim);
    tr->hits = 1;
    tr->heat = 1;
    tr->last_tick = ++h->heat_clock;
    return 0;
}

/* Fill expand_n one-hot rows; label via residual oracle or copy exemplar.
 * Rows are spread across the window (not only the first expand_n slots) so
 * large W (e.g. Bonsai 256) still get representative coverage. */
static int label_expand_rows(HybridAi *h, HybridTrace *tr, size_t n_rows,
                             double *inputs, double *targets) {
    size_t r, j;
    if (!h || !tr || !inputs || !targets || n_rows == 0) return -1;

    /* D: pilot-ordered batch residual labels when ctx is ResidualGguf. */
    if (h->residual.bound && h->residual.fn == residual_gguf_oracle &&
        h->residual.ctx) {
        ResidualGguf *rg = (ResidualGguf *)h->residual.ctx;
        int slots[256];
        int ns = (int)n_rows;
        if (ns > 256) ns = 256;
        residual_gguf_pilot_consume(rg, slots, ns);
        if (residual_gguf_label_batch(rg, slots, ns, inputs, targets,
                                      (int)tr->in_dim, (int)tr->out_dim) == 0) {
            h->batch_label_rows += (size_t)ns;
            return ns;
        }
        /* fall through to per-row residual */
    }

    for (r = 0; r < n_rows; r++) {
        size_t slot;
        if (n_rows >= tr->in_dim)
            slot = r % tr->in_dim;
        else
            slot = (r * tr->in_dim) / n_rows;
        for (j = 0; j < tr->in_dim; j++)
            inputs[r * tr->in_dim + j] = (j == slot) ? 1.0 : 0.0;
        if (h->residual.bound) {
            if (h->residual.fn(inputs + r * tr->in_dim,
                               targets + r * tr->out_dim,
                               h->residual.ctx) != 0)
                memcpy(targets + r * tr->out_dim, tr->out,
                       tr->out_dim * sizeof(double));
            h->batch_label_rows++;
        } else {
            memcpy(targets + r * tr->out_dim, tr->out,
                   tr->out_dim * sizeof(double));
        }
    }
    return (int)n_rows;
}

int hybrid_structure_mine(HybridAi *h, PrimitiveRegistry *reg, size_t min_hits,
                          BinaryTransformNetwork **student_out) {
    size_t i, best = (size_t)-1;
    uint64_t best_score = 0;
    HybridTrace *tr;
    ExternalTeacher teacher;
    CnetOracleIdentity id;
    double *inputs = NULL, *targets = NULL;
    char name[64];
    int rc;
    size_t n_rows = 1;

    if (!h || !reg || min_hits == 0 || !student_out) return -1;
    for (i = 0; i < h->trace_count; i++) {
        uint64_t sc;
        if (h->traces[i].hits < min_hits) continue;
        sc = cnet_lfru_score(h->traces[i].heat, h->traces[i].last_tick,
                             h->heat_clock);
        if (best == (size_t)-1 || sc > best_score) {
            best_score = sc;
            best = i;
        }
    }
    if (best == (size_t)-1) return 1;

    tr = &h->traces[best];
    /* A3: refuse to mine a port family whose coverage cannot be enforced.
     * Membership is an exact match, which is meaningless for PORT_RAW's
     * continuous values — so a RAW unit could never be gated and would answer
     * anything it was asked, confidently and wrongly, forever. Mining it at all
     * is the mistake; interval coverage is the prerequisite, not a workaround.
     * CNET_MINE_UNGATEABLE=1 is a break-glass override for experiments. */
    if (!coverage_family_gated(tr->input_port)) {
        const char *ov = getenv("CNET_MINE_UNGATEABLE");
        if (!(ov && ov[0] == '1' && ov[1] == '\0')) {
            fprintf(stderr,
                    "hybrid: refusing to mine port family %d (tag='%s') — "
                    "coverage cannot be enforced for it\n",
                    (int)tr->input_port.family, tr->input_port.tag);
            tr->hits = 0; /* do not re-attempt every tick */
            return 3;
        }
    }
    /* One mined unit per port shape, ever.
     *
     * Under CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE=1 every Tier-C miss can fire
     * a mine, and each one used to mint a fresh hyb_struct_N. Coverage is keyed
     * by SHAPE, so mine N+1 overwrote the record naming mine N — leaving N as a
     * sealed, certified, *unguarded* unit in the base. Measured: 15 serves
     * produced 7 mined units, 6 of them orphaned. Before fail-closed those
     * orphans answered anything asked of them.
     *
     * Re-mining the same shape cannot currently be made consistent: the base
     * has no unit-removal call, and cnb_add_unit refuses same-name-different-
     * bytes, so an improved unit can neither replace nor supersede the old one.
     * Until that exists, mining a shape once is the only state that keeps the
     * unit, its contract and its coverage in agreement. Improving an already
     * mined shape is backlog and needs base-level unit replacement. */
    if (coverage_find(h, port_key(tr->input_port), port_key(tr->goal_port),
                      tr->input_port, tr->goal_port)) {
        tr->hits = 0; /* do not re-attempt on every serve */
        return 4;
    }
    /* Fail closed: a unit we cannot gate must never exist. Reserve the coverage
       slot BEFORE admitting, because once external_teacher_mine_admit has put
       the unit in the registry it will serve, and with no coverage record the
       serve path default-allows — silently reopening the confident-wrong hole
       this whole mechanism exists to close. Demotion is not an alternative:
       PRIM_RESET only excludes from planning when reg->lifecycle_enabled, which
       registry_init leaves zero. */
    if (!coverage_find(h, port_key(tr->input_port), port_key(tr->goal_port),
                       tr->input_port, tr->goal_port) &&
        h->coverage_count >= HYBRID_COVERAGE_MAX) {
        fprintf(stderr, "hybrid: coverage table full (%d shapes) — refusing to "
                        "mine an ungated unit\n", HYBRID_COVERAGE_MAX);
        return 2;
    }
    {
        size_t expand_n = 0;
        size_t expand_cap = 64;
        size_t min_res_rows = 2;
        int from_reservoir = 0;
        {
            const char *ee = getenv("CNET_STRUCTURE_EXPAND_N");
            if (ee && ee[0]) {
                long v = atol(ee);
                if (v >= 4 && v <= 512) expand_cap = (size_t)v;
            }
        }
        {
            const char *me = getenv("CNET_RESERVOIR_MIN_ROWS");
            if (me && me[0]) {
                long v = atol(me);
                if (v >= 1 && v <= 1024) min_res_rows = (size_t)v;
            }
        }
        if (tr->input_port.family == PORT_ONEHOT &&
            tr->input_port.field_count == 1) {
            if (tr->in_dim <= 16)
                expand_n = tr->in_dim;
            else if (h->residual.bound) {
                /* Large residual windows (Bonsai 256): sample up to expand_cap
                 * rows spread across the alphabet for structure mine. */
                expand_n = tr->in_dim < expand_cap ? tr->in_dim : expand_cap;
            }
        }
        /* Real traffic beats a synthetic basis — but never at the cost of
         * coverage. For a small one-hot alphabet the synthetic expansion spans
         * the WHOLE input domain, so partial traffic there would admit a unit
         * with less coverage than before. Take the reservoir only when it is at
         * least as wide as the basis would have been; when no expansion exists
         * (non-ONEHOT, or no residual bound) any real rows beat one exemplar. */
        if (tr->res_count >= min_res_rows && tr->res_count >= expand_n &&
            tr->res_in && tr->res_out) {
            n_rows = tr->res_count;
            inputs = (double *)calloc(n_rows * tr->in_dim, sizeof(double));
            targets = (double *)calloc(n_rows * tr->out_dim, sizeof(double));
            if (!inputs || !targets) {
                free(inputs);
                free(targets);
                return -3;
            }
            memcpy(inputs, tr->res_in, n_rows * tr->in_dim * sizeof(double));
            memcpy(targets, tr->res_out, n_rows * tr->out_dim * sizeof(double));
            from_reservoir = 1;
            /* These rows are residual-produced labels just like the expanded
               basis — the reservoir captured them at serve time instead of
               re-deriving them here, so the mine consumed the same number of
               residual labels and batch_label_rows must still see them. */
            h->batch_label_rows += n_rows;
        }
        if (from_reservoir) {
            /* inputs/targets already filled from real traffic. */
        } else if (expand_n > 0) {
            int labeled;
            n_rows = expand_n;
            inputs = (double *)calloc(n_rows * tr->in_dim, sizeof(double));
            targets = (double *)calloc(n_rows * tr->out_dim, sizeof(double));
            if (!inputs || !targets) {
                free(inputs);
                free(targets);
                return -3;
            }
            labeled = label_expand_rows(h, tr, n_rows, inputs, targets);
            if (labeled <= 0) {
                free(inputs);
                free(targets);
                return -4;
            }
            n_rows = (size_t)labeled;
        } else {
            n_rows = 1;
            inputs = (double *)malloc(tr->in_dim * sizeof(double));
            targets = (double *)malloc(tr->out_dim * sizeof(double));
            if (!inputs || !targets) {
                free(inputs);
                free(targets);
                return -3;
            }
            memcpy(inputs, tr->in, tr->in_dim * sizeof(double));
            memcpy(targets, tr->out, tr->out_dim * sizeof(double));
        }

        memset(&id, 0, sizeof id);
        id.abi_version = CNET_ORACLE_ABI_VERSION;
        id.struct_size = (uint32_t)sizeof id;
        id.artifact_digest = 0x53545255435401ULL; /* STRUCT */
        id.contract_digest = 0x4D494E45ULL;
        external_teacher_init(&teacher);
        /* Prefer table teacher from labeled expand rows for large windows —
         * callback residual over full domain is too slow/unbounded for admit.
         * Reservoir rows are already real labelled pairs, so always table:
         * re-labelling a synthetic domain would discard the traffic we kept. */
        if (from_reservoir || n_rows >= 4) {
            rc = external_teacher_bind_table(
                &teacher, CNET_MODALITY_TEXT, "structure_table",
                tr->input_port, tr->goal_port, inputs, targets, n_rows, &id,
                0);
        } else if (h->residual.bound) {
            rc = external_teacher_bind_callback(
                &teacher, CNET_MODALITY_TEXT, "structure_teacher",
                tr->input_port, tr->goal_port, h->residual.fn, h->residual.ctx,
                &id, 0);
        } else {
            rc = external_teacher_bind_table(
                &teacher, CNET_MODALITY_TEXT, "structure_table",
                tr->input_port, tr->goal_port, inputs, targets, n_rows, &id,
                0);
        }
        if (rc != 0) {
            free(inputs);
            free(targets);
            return -4;
        }
        if (getenv("CNET_MINE_DEBUG"))
            fprintf(stderr,
                    "[mine] bind rc=%d n_rows=%zu in_dim=%zu out_dim=%zu "
                    "from_reservoir=%d fam=%d fc=%zu fw=%zu\n",
                    rc, n_rows, tr->in_dim, tr->out_dim, from_reservoir,
                    (int)tr->input_port.family, tr->input_port.field_count,
                    tr->input_port.field_width);
        snprintf(name, sizeof name, "hyb_struct_%zu", h->structure_mines);
        {
            size_t ih = tr->in_dim > 16 ? 64 : 8;
            size_t mh = tr->in_dim > 16 ? 256 : 64;
            size_t ep = tr->in_dim > 16 ? 40000 : 12000;
            /* registry_add stores the name POINTER, not a copy
               (src/router/registry.c). Passing this function's stack buffer
               left every mined entry with a dangling name: undefined behaviour
               on any later read, and find_named could never match it, which
               silently disabled MoE hard-expert dispatch for mined units. The
               registry does not own names and can outlive this HybridAi, so the
               only safe lifetime is "never freed" — one small allocation per
               successful mine, and mines are bounded by the coverage table. */
            size_t nlen = strlen(name) + 1;
            char *stable = (char *)malloc(nlen);
            if (!stable) {
                external_teacher_unbind(&teacher);
                free(inputs);
                free(targets);
                return -3;
            }
            memcpy(stable, name, nlen);
            rc = external_teacher_mine_admit(
                &teacher, reg, inputs, targets, n_rows, ih, mh, ep, 99u, stable,
                student_out);
            if (rc != 0) free(stable); /* nothing borrowed it */
        }
        external_teacher_unbind(&teacher);
        if (rc == 0) {
            /* The unit is certified over exactly these rows — remember them so
               the serve path can refuse to claim certified authority outside
               the domain the contract actually covers. */
            if (hybrid_coverage_record(h, tr->input_port, tr->goal_port, name,
                                       inputs, targets, n_rows, tr->in_dim,
                                       tr->out_dim) != 0) {
                /* Slot was reserved above, so this is allocation failure. The
                   unit is already admitted and would serve ungated; say so
                   loudly rather than let it look like a clean mine. */
                fprintf(stderr, "hybrid: unit '%s' admitted but coverage NOT "
                                "recorded — it will serve ungated\n", name);
                rc = -5;
            }
        }
        free(inputs);
        free(targets);
        if (rc != 0) return rc;
        tr->hits = 0;
        h->structure_mines++;
        /* Count admitted units, not attempts — a counter that ticks on failure
           would overstate how much of the library came from real traffic. */
        if (from_reservoir) h->reservoir_mines++;
        else h->synthetic_mines++;
        return 0;
    }
}

int hybrid_hermetic_residual(const double *in, double *out, void *ctx) {
    size_t n = ctx ? (size_t)(uintptr_t)ctx : 4;
    size_t i, hot = 0;
    if (!in || !out) return -1;
    if (n == 0) n = 4;
    for (i = 1; i < n; i++)
        if (in[i] > in[hot]) hot = i;
    memset(out, 0, n * sizeof(double));
    out[(hot + 1) % n] = 1.0;
    return 0;
}

int hybrid_hermetic_soft(const double *in, double *out, void *ctx) {
    HybridSoftCtx *sc = (HybridSoftCtx *)ctx;
    size_t n = 4, i, hot = 0;
    double tmp[8];
    if (!in || !out) return -1;
    if (sc && sc->force_abstain) return 1;
    for (i = 1; i < n; i++)
        if (in[i] > in[hot]) hot = i;
    memset(tmp, 0, n * sizeof(double));
    tmp[hot] = 0.55;
    tmp[(hot + 1) % n] = 0.45;
    if (sc && sc->min_margin > 0.0 && top_margin(tmp, n) < sc->min_margin)
        return 1;
    if (!sc || sc->min_margin <= 0.0) {
        memset(out, 0, n * sizeof(double));
        out[(hot + 1) % n] = 1.0;
    } else {
        memcpy(out, tmp, n * sizeof(double));
    }
    return 0;
}
