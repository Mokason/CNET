#include "../include/hybrid_ai.h"
#include "../include/external_teacher.h"
#include "../include/cnet_lfru.h"
#include "../include/residual_gguf.h"

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
    }
    memset(h, 0, sizeof *h);
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
    tr->hits = 1;
    tr->heat = 1;
    tr->last_tick = ++h->heat_clock;
    return 0;
}

/* Fill expand_n one-hot rows; label via residual oracle or copy exemplar. */
static int label_expand_rows(HybridAi *h, HybridTrace *tr, size_t n_rows,
                             double *inputs, double *targets) {
    size_t r, j;
    if (!h || !tr || !inputs || !targets || n_rows == 0) return -1;

    /* D: pilot-ordered batch residual labels when ctx is ResidualGguf. */
    if (h->residual.bound && h->residual.fn == residual_gguf_oracle &&
        h->residual.ctx) {
        ResidualGguf *rg = (ResidualGguf *)h->residual.ctx;
        int slots[64];
        int ns = (int)n_rows;
        if (ns > 64) ns = 64;
        residual_gguf_pilot_consume(rg, slots, ns);
        if (residual_gguf_label_batch(rg, slots, ns, inputs, targets,
                                      (int)tr->in_dim, (int)tr->out_dim) == 0) {
            h->batch_label_rows += (size_t)ns;
            return ns;
        }
        /* fall through to per-row residual */
    }

    for (r = 0; r < n_rows; r++) {
        for (j = 0; j < tr->in_dim; j++)
            inputs[r * tr->in_dim + j] = (j == r) ? 1.0 : 0.0;
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
    {
        size_t expand_n = 0;
        if (tr->input_port.family == PORT_ONEHOT &&
            tr->input_port.field_count == 1) {
            if (tr->in_dim <= 16)
                expand_n = tr->in_dim;
            else if (h->residual.bound)
                expand_n = 16;
        }
        if (expand_n > 0) {
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
        if (h->residual.bound) {
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
        snprintf(name, sizeof name, "hyb_struct_%zu", h->structure_mines);
        {
            size_t ih = tr->in_dim > 16 ? 64 : 8;
            size_t mh = tr->in_dim > 16 ? 256 : 64;
            size_t ep = tr->in_dim > 16 ? 30000 : 12000;
            rc = external_teacher_mine_admit(
                &teacher, reg, inputs, targets, n_rows, ih, mh, ep, 99u, name,
                student_out);
        }
        external_teacher_unbind(&teacher);
        free(inputs);
        free(targets);
        if (rc != 0) return rc;
        tr->hits = 0;
        h->structure_mines++;
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
