#include "../include/cnet_brain_sidecar.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int32_t flags;
    int32_t parent;
    float ema;
    float *center;
    float *W;
    float *b;
} SidecarPiece;

struct CnetBrainSidecar {
    int in_dim;
    int out_dim;
    uint32_t n;
    uint32_t maxh;
    float rstop;
    float yvar;
    SidecarPiece *P;
    float *C;
    float *W;
    float *B;
    float *scratch;
    char path[512];
};

static CnetBrainSidecar *g_env_sc;
static int g_env_tried;

void cnet_brain_sidecar_free(CnetBrainSidecar *s) {
    if (!s) return;
    free(s->P);
    free(s->C);
    free(s->W);
    free(s->B);
    free(s->scratch);
    free(s);
}

int cnet_brain_sidecar_load(CnetBrainSidecar **out, const char *pieces_path) {
    FILE *pf;
    uint32_t magic = 0, ver = 0, idim = 0, odim = 0, n = 0, maxh = 3;
    float rstop = 0.08f, yvar = 1.f;
    CnetBrainSidecar *s;
    size_t csz, wsz, bsz;
    uint32_t i;
    int ok;

    if (!out || !pieces_path || !pieces_path[0]) return -2;
    *out = NULL;
    pf = fopen(pieces_path, "rb");
    if (!pf) return -1;

    if (fread(&magic, 4, 1, pf) != 1 || magic != 0x43504243u /* CBPC */ ||
        fread(&ver, 4, 1, pf) != 1 || ver != 1 || fread(&idim, 4, 1, pf) != 1 ||
        fread(&odim, 4, 1, pf) != 1 || idim == 0 || odim == 0 || idim > 4096 ||
        odim > 4096 || fread(&n, 4, 1, pf) != 1 || n == 0 || n > 4096 ||
        fread(&maxh, 4, 1, pf) != 1 || fread(&rstop, 4, 1, pf) != 1 ||
        fread(&yvar, 4, 1, pf) != 1) {
        fclose(pf);
        return -1;
    }

    s = (CnetBrainSidecar *)calloc(1, sizeof *s);
    if (!s) {
        fclose(pf);
        return -3;
    }
    s->in_dim = (int)idim;
    s->out_dim = (int)odim;
    s->n = n;
    s->maxh = maxh ? maxh : 3;
    s->rstop = rstop;
    s->yvar = yvar > 1e-6f ? yvar : 1.f;
    snprintf(s->path, sizeof s->path, "%s", pieces_path);

    csz = (size_t)n * (size_t)idim;
    wsz = (size_t)n * (size_t)odim * (size_t)idim;
    bsz = (size_t)n * (size_t)odim;
    s->P = (SidecarPiece *)calloc(n, sizeof(SidecarPiece));
    s->C = (float *)malloc(csz * sizeof(float));
    s->W = (float *)malloc(wsz * sizeof(float));
    s->B = (float *)malloc(bsz * sizeof(float));
    s->scratch = (float *)malloc((size_t)odim * sizeof(float));
    ok = (s->P && s->C && s->W && s->B && s->scratch);
    if (ok) {
        for (i = 0; i < n; i++) {
            if (fread(&s->P[i].flags, 4, 1, pf) != 1 ||
                fread(&s->P[i].parent, 4, 1, pf) != 1 ||
                fread(&s->P[i].ema, 4, 1, pf) != 1) {
                ok = 0;
                break;
            }
            s->P[i].center = s->C + (size_t)i * (size_t)idim;
            s->P[i].W = s->W + (size_t)i * (size_t)odim * (size_t)idim;
            s->P[i].b = s->B + (size_t)i * (size_t)odim;
            if (fread(s->P[i].center, sizeof(float), (size_t)idim, pf) != (size_t)idim ||
                fread(s->P[i].W, sizeof(float), (size_t)odim * (size_t)idim, pf) !=
                    (size_t)odim * (size_t)idim ||
                fread(s->P[i].b, sizeof(float), (size_t)odim, pf) != (size_t)odim) {
                ok = 0;
                break;
            }
        }
    }
    fclose(pf);
    if (!ok) {
        cnet_brain_sidecar_free(s);
        return -1;
    }
    *out = s;
    return 0;
}

int cnet_brain_sidecar_dims(const CnetBrainSidecar *s, int *in_dim, int *out_dim) {
    if (!s) return -1;
    if (in_dim) *in_dim = s->in_dim;
    if (out_dim) *out_dim = s->out_dim;
    return 0;
}

int cnet_brain_sidecar_n_pieces(const CnetBrainSidecar *s) {
    return s ? (int)s->n : 0;
}

int cnet_brain_sidecar_serve_f(const CnetBrainSidecar *s, const float *x, int in_dim,
                               float *y, int out_dim) {
    float best = 1e30f;
    int best_i = -1;
    uint32_t i;
    int hops, h, r, c;
    float stop;

    if (!s || !x || !y) return -2;
    if (in_dim != s->in_dim || out_dim != s->out_dim) return -2;

    for (i = 0; i < s->n; i++) {
        double acc = 0;
        float sc;
        if (s->P[i].flags & 1) continue; /* residual */
        for (c = 0; c < in_dim; c++) {
            double d = (double)x[c] - (double)s->P[i].center[c];
            acc += d * d;
        }
        sc = (float)(acc / (double)in_dim);
        sc *= (1.f + s->P[i].ema / (s->yvar + 1e-6f));
        if (s->P[i].flags & 2) sc *= 0.72f;
        else sc *= 1.25f;
        if (sc < best) {
            best = sc;
            best_i = (int)i;
        }
    }
    if (best_i < 0 || best > 40.f) return -1; /* abstain */

    memset(y, 0, (size_t)out_dim * sizeof(float));
    for (r = 0; r < out_dim; r++) {
        float sum = s->P[best_i].b[r];
        const float *row = s->P[best_i].W + (size_t)r * (size_t)in_dim;
        for (c = 0; c < in_dim; c++) sum += row[c] * x[c];
        y[r] = sum;
    }

    stop = s->rstop * s->yvar;
    hops = (int)s->maxh;
    if (hops < 1) hops = 1;
    for (h = 1; h < hops; h++) {
        float br = 1e30f;
        int rk = -1;
        if (s->P[best_i].ema < stop) break;
        for (i = 0; i < s->n; i++) {
            double acc = 0;
            float sc;
            if (!(s->P[i].flags & 1)) continue;
            if (s->P[i].parent != best_i && s->P[i].parent >= 0) continue;
            for (c = 0; c < in_dim; c++) {
                double d = (double)x[c] - (double)s->P[i].center[c];
                acc += d * d;
            }
            sc = (float)(acc / (double)in_dim);
            sc *= (1.f + s->P[i].ema / (s->yvar + 1e-6f));
            if (s->P[i].flags & 2) sc *= 0.9f;
            if (sc < br) {
                br = sc;
                rk = (int)i;
            }
        }
        if (rk < 0 || br > 30.f) break;
        for (r = 0; r < out_dim; r++) {
            float sum = s->P[rk].b[r];
            const float *row = s->P[rk].W + (size_t)r * (size_t)in_dim;
            for (c = 0; c < in_dim; c++) sum += row[c] * x[c];
            s->scratch[r] = sum;
        }
        {
            float alpha = (s->P[rk].flags & 2) ? 0.65f : 0.40f;
            for (r = 0; r < out_dim; r++) y[r] += alpha * s->scratch[r];
        }
        if (s->P[rk].ema < stop) break;
    }
    return 0;
}

int cnet_brain_sidecar_serve(const CnetBrainSidecar *s, const double *x, size_t in_dim,
                             double *y, size_t out_dim) {
    float xf[256], yf[256];
    size_t i;
    int rc;
    if (!s || !x || !y || in_dim == 0 || out_dim == 0) return -2;
    if (in_dim > 256 || out_dim > 256) return -2;
    if ((int)in_dim != s->in_dim || (int)out_dim != s->out_dim) return -2;
    for (i = 0; i < in_dim; i++) xf[i] = (float)x[i];
    rc = cnet_brain_sidecar_serve_f(s, xf, (int)in_dim, yf, (int)out_dim);
    if (rc != 0) return rc;
    for (i = 0; i < out_dim; i++) y[i] = (double)yf[i];
    return 0;
}

const CnetBrainSidecar *cnet_brain_sidecar_env(void) {
    const char *e;
    char path[600];
    CnetBrainSidecar *s = NULL;
    if (g_env_tried) return g_env_sc;
    g_env_tried = 1;
    e = getenv("CNET_BRAIN_SIDECAR");
    if (!e || !e[0]) return NULL;
    /* accept dir or pieces.bin path */
    if (strstr(e, "pieces.bin"))
        snprintf(path, sizeof path, "%s", e);
    else
        snprintf(path, sizeof path, "%s/pieces.bin", e);
    if (cnet_brain_sidecar_load(&s, path) != 0) return NULL;
    g_env_sc = s;
    return g_env_sc;
}
