#include "../../include/cnet_sparse_serve.h"
#include "../../include/router.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static double row_l2(const double *a, const double *b, size_t n) {
    double acc = 0.0;
    size_t i;
    for (i = 0; i < n; i++) {
        double d = a[i] - b[i];
        acc += d * d;
    }
    return n ? acc / (double)n : 0.0;
}

static int row_exact(const double *a, const double *b, size_t n) {
    return memcmp(a, b, n * sizeof(double)) == 0;
}

static void row_mean(const double *rows, size_t n_rows, size_t dim, double *mean) {
    size_t r, j;
    memset(mean, 0, dim * sizeof(double));
    if (n_rows == 0) return;
    for (r = 0; r < n_rows; r++)
        for (j = 0; j < dim; j++) mean[j] += rows[r * dim + j];
    for (j = 0; j < dim; j++) mean[j] /= (double)n_rows;
}

static int port_match_loose(Port a, Port b) {
    /* Shape match used by hybrid coverage find: family+width+count; tag empty wildcard */
    if (a.family != b.family || a.field_width != b.field_width ||
        a.field_count != b.field_count)
        return 0;
    if (a.tag[0] && b.tag[0] && strcmp(a.tag, b.tag) != 0) return 0;
    return 1;
}

int cnet_sparse_unit_center(const HybridAi *h, const char *unit, double *center,
                            size_t in_dim) {
    size_t i;
    if (!h || !unit || !unit[0] || !center || in_dim == 0) return -1;
    for (i = 0; i < h->coverage_count; i++) {
        const HybridCoverage *c = &h->coverage[i];
        if (!c->active || !c->rows || strcmp(c->unit, unit) != 0) continue;
        if (c->in_dim != in_dim) return -1;
        row_mean(c->rows, c->n_rows, c->in_dim, center);
        return 0;
    }
    return -1;
}

int cnet_sparse_rank_covered(const HybridAi *h, Port in_port, Port out_port,
                             const double *in, size_t in_len,
                             CnetSparseCandidate *out, size_t cap, size_t *n_out) {
    size_t i, n = 0;
    double center_buf[256];
    if (n_out) *n_out = 0;
    if (!h || !in || !out || cap == 0 || in_len == 0) return -1;

    for (i = 0; i < h->coverage_count && n < cap; i++) {
        const HybridCoverage *c = &h->coverage[i];
        size_t r;
        int admits = 0;
        double best_row = 1e300;
        double score;
        if (!c->active || !c->rows || c->n_rows == 0) continue;
        if (c->in_dim != in_len) continue;
        if (!port_match_loose(c->input_port, in_port)) continue;
        /* out_port: empty tag = wildcard over matching family/shape */
        if (out_port.tag[0]) {
            if (!port_match_loose(c->goal_port, out_port)) continue;
        } else if (out_port.family != 0 || out_port.field_width != 0) {
            if (c->goal_port.family != out_port.family ||
                c->goal_port.field_width != out_port.field_width ||
                c->goal_port.field_count != out_port.field_count)
                continue;
        }

        for (r = 0; r < c->n_rows; r++) {
            const double *row = c->rows + r * c->in_dim;
            if (row_exact(row, in, in_len)) {
                admits = 1;
                best_row = 0.0;
                break;
            }
            {
                double d = row_l2(row, in, in_len);
                if (d < best_row) best_row = d;
            }
        }
        /* Fail-closed: only exact certified membership admits (same as coverage gate). */
        if (!admits) continue;

        if (c->in_dim <= 256) {
            row_mean(c->rows, c->n_rows, c->in_dim, center_buf);
            score = row_l2(center_buf, in, in_len);
            if (best_row == 0.0) score = 0.0;
        } else {
            score = best_row;
        }

        memset(&out[n], 0, sizeof out[n]);
        snprintf(out[n].unit, sizeof out[n].unit, "%s", c->unit);
        out[n].score = score;
        out[n].n_rows = c->n_rows;
        out[n].admits = 1;
        n++;
    }

    /* sort by score ascending (simple insertion) */
    {
        size_t a, b;
        for (a = 1; a < n; a++) {
            CnetSparseCandidate key = out[a];
            b = a;
            while (b > 0 && out[b - 1].score > key.score) {
                out[b] = out[b - 1];
                b--;
            }
            out[b] = key;
        }
    }
    if (n_out) *n_out = n;
    return 0;
}

int cnet_sparse_pick_covered_unit(const HybridAi *h, Port in_port, Port out_port,
                                  const double *in, size_t in_len, char *name_out,
                                  size_t name_cap, double *score_out) {
    CnetSparseCandidate cand[CNET_SPARSE_MAX_CAND];
    size_t n = 0;
    if (cnet_sparse_rank_covered(h, in_port, out_port, in, in_len, cand,
                                 CNET_SPARSE_MAX_CAND, &n) != 0)
        return -1;
    if (n == 0) return -1; /* abstain */
    if (name_out && name_cap)
        snprintf(name_out, name_cap, "%s", cand[0].unit);
    if (score_out) *score_out = cand[0].score;
    return 0;
}

static const RegistryEntry *find_cert_entry(const PrimitiveRegistry *reg,
                                            const char *name) {
    size_t i;
    if (!reg || !name || !name[0]) return NULL;
    for (i = 0; i < reg->count; i++) {
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0 &&
            reg->entries[i].certified && reg->entries[i].btn)
            return &reg->entries[i];
    }
    return NULL;
}

static int unit_admits_or_ungated(const HybridAi *h, const char *unit, Port in_port,
                                  Port out_port, const double *x, size_t in_len) {
    /* If no coverage record for unit â†’ allow (hand-admitted whole-domain).
     * If record exists â†’ must admit exact row (fail-closed). */
    if (!h || !unit) return 0;
    if (!hybrid_coverage_has_unit(h, unit)) return 1;
    if (out_port.tag[0])
        return hybrid_coverage_admits_exact(h, unit, in_port, out_port, x, in_len);
    /* wildcard out: any record for this unit that admits the row */
    {
        size_t i, r;
        for (i = 0; i < h->coverage_count; i++) {
            const HybridCoverage *c = &h->coverage[i];
            if (!c->active || !c->rows || strcmp(c->unit, unit) != 0) continue;
            if (c->in_dim != in_len) continue;
            if (!port_match_loose(c->input_port, in_port)) continue;
            for (r = 0; r < c->n_rows; r++)
                if (row_exact(c->rows + r * c->in_dim, x, in_len)) return 1;
        }
        return 0;
    }
}

int cnet_sparse_run_accum(const HybridAi *h, const PrimitiveRegistry *reg,
                          Port in_port, Port out_port, const char *peak_unit,
                          const char *resid_unit, const double *x, size_t in_len,
                          double *y, size_t out_cap, double alpha,
                          int *used_residual_out) {
    const RegistryEntry *peak_e, *res_e = NULL;
    BinaryTransformNetwork *pbtn, *rbtn;
    const double *py, *ry;
    size_t out_dim, j;
    double a = (alpha > 0.0 && alpha <= 2.0) ? alpha : 0.65;

    if (used_residual_out) *used_residual_out = 0;
    if (!reg || !peak_unit || !x || !y || in_len == 0 || out_cap == 0) return -2;

    peak_e = find_cert_entry(reg, peak_unit);
    if (!peak_e) return -1;
    pbtn = peak_e->btn;
    if (!pbtn || pbtn->input_count != in_len) return -1;
    out_dim = pbtn->output_count;
    if (out_dim == 0 || out_dim > out_cap) return -1;

    if (h && !unit_admits_or_ungated(h, peak_unit, in_port, out_port, x, in_len))
        return -1; /* coverage refuse peak */

    py = btn_forward(pbtn, x);
    if (!py) return -1;
    memcpy(y, py, out_dim * sizeof(double));

    if (!resid_unit || !resid_unit[0]) return 0;

    res_e = find_cert_entry(reg, resid_unit);
    if (!res_e) return -1; /* residual requested but missing â†’ refuse (fail closed) */
    rbtn = res_e->btn;
    if (!rbtn || rbtn->input_count != in_len || rbtn->output_count != out_dim)
        return -1;

    if (h && !unit_admits_or_ungated(h, resid_unit, in_port, out_port, x, in_len))
        return -1;

    ry = btn_forward(rbtn, x);
    if (!ry) return -1;
    for (j = 0; j < out_dim; j++) y[j] += a * ry[j];
    if (used_residual_out) *used_residual_out = 1;
    return 0;
}
