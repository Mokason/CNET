/* Decision-saturation depth probe — the make-or-break measurement for the
 * "layers as on-demand substrates" idea.
 *
 * The flagship oracle consumes ONLY window-restricted decisions (argmax /
 * ordered top-3 over |V| candidate tokens). This tool measures, over probe
 * contexts drawn from the campaign's own query distribution ([t, w] with
 * both tokens in the window), at which layer depth those decisions stop
 * changing: after every layer it projects the last row of the residual
 * stream through the final norm + the head's WINDOW COLUMNS and records the
 * ordered top-3.
 *
 * Verdict table: for each candidate cap K, the fraction of probes whose
 * decisions at depth K already equal full depth. 100% at K < n_layer means
 * a capped oracle is worth gating (flagship still re-verifies at startup
 * and refuses on mismatch); anything less at every K means the idea is DEAD
 * for this model and this tool just saved a corrupted campaign.
 *
 * Usage: depth_probe <model> [V window] [N probes]
 *   (CNET_ORACLE_INT8=1 recommended for 12B-class models, exactly like the
 *    campaign; the head stays FP either way, which is what the probe reads.)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cce/cce_detect.h"
#include "window_discover.h"

#define DP_MAX_LAYERS 128
#define DP_TOPK 3

typedef struct {
    /* window head: packed [Vw x D] slice of the head (float, already
       including int8 dequant codes when the oracle's head is int8), plus a
       per-window-column scale (1.0 for FP heads) and the final norm.
       logit[j] = scale[j] * sum_d normed[d] * w_win[j*D+d]  — the same
       accumulation order and dequant math as the oracle's own head. */
    float *w_win;
    float *scale_win;
    const float *norm_w;
    float eps;
    int D;
    int vw;
    /* per-layer ordered top-k of the CURRENT probe (last row only) */
    int top[DP_MAX_LAYERS][DP_TOPK];
    int layers_seen;
} DepthProbe;

static int g_forensic = 0;   /* first probe: per-layer stream stats */

static void probe_tap(int layer, const float *x, int n_tokens, int dim,
                      void *uctx) {
    DepthProbe *p = (DepthProbe *)uctx;
    const float *row = x + (size_t)(n_tokens - 1) * dim;
    if (g_forensic) {
        float mn = row[0], mx = row[0];
        int nans = 0, d2;
        for (d2 = 0; d2 < dim; ++d2) {
            if (row[d2] != row[d2]) nans++;
            if (row[d2] < mn) mn = row[d2];
            if (row[d2] > mx) mx = row[d2];
        }
        printf("  layer %2d residual: min=%.4g max=%.4g nans=%d/%d\n",
               layer, (double)mn, (double)mx, nans, dim);
    }
    float ss = 0.0f, inv;
    float *tmp;
    float best[DP_TOPK];
    int k, j, d;
    if (layer >= DP_MAX_LAYERS || dim != p->D) return;
    tmp = (float *)malloc((size_t)dim * sizeof *tmp);
    if (!tmp) return;
    /* same RMSNorm the real head sees (no mean subtraction) */
    for (d = 0; d < dim; ++d) ss += row[d] * row[d];
    inv = 1.0f / sqrtf(ss / dim + p->eps);
    for (d = 0; d < dim; ++d) tmp[d] = (row[d] * inv) * p->norm_w[d];
    /* window logits + ordered top-3 (same tie-breaking as the oracle:
       strict > keeps the earlier index on ties) */
    {
        float *lg = (float *)malloc((size_t)p->vw * sizeof *lg);
        if (!lg) { free(tmp); return; }
        for (j = 0; j < p->vw; ++j) {
            float acc = 0.0f;
            const float *wcol = p->w_win + (size_t)j * dim;
            for (d = 0; d < dim; ++d) acc += tmp[d] * wcol[d];
            lg[j] = p->scale_win[j] * acc;
        }
        for (k = 0; k < DP_TOPK; ++k) {
            int bi = -1;
            for (j = 0; j < p->vw; ++j) {
                int taken = 0, t2;
                for (t2 = 0; t2 < k; ++t2)
                    if (p->top[layer][t2] == j) taken = 1;
                if (taken) continue;
                if (bi == -1 || lg[j] > best[k]) { best[k] = lg[j]; bi = j; }
            }
            p->top[layer][k] = bi;
        }
        free(lg);
    }
    if (layer + 1 > p->layers_seen) p->layers_seen = layer + 1;
    free(tmp);
}

static double wall_s(void) {
#ifdef _WIN32
    return (double)clock() / CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

int main(int argc, char **argv) {
    size_t V = 256, N = 128, i;
    cce_anymodel *am = NULL;
    cce_gguf_qwen2 *m;
    DepthProbe p;
    int *vocab;
    float *logits;
    int *sat_top1, *sat_top3;
    size_t *agree1, *agree3;
    int *finals;
    size_t probe_head_mismatch = 0;
    int L, D, l;
    double t0;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <model> [V] [N]\n", argv[0]);
        return 2;
    }
    if (argc > 2) V = (size_t)atoi(argv[2]);
    if (argc > 3) N = (size_t)atoi(argv[3]);
    if (V < 4 || V > 4096) { fprintf(stderr, "V out of range\n"); return 2; }

    if (cce_anymodel_open(&am, argv[1]) != CCE_OK || am->transformer == NULL) {
        fprintf(stderr, "cannot open transformer for %s\n", argv[1]);
        return 1;
    }
    m = am->transformer;
    L = m->n_layer;
    D = m->n_embd;
    if (L > DP_MAX_LAYERS) { fprintf(stderr, "too many layers\n"); return 1; }
    printf("model: %s  layers=%d hidden=%d vocab=%d  window=%lu probes=%lu\n",
           argv[1], L, D, m->vocab_size, (unsigned long)V, (unsigned long)N);

    /* the window under test: CNET_WINDOW_FILE if set (the campaign's
       explicit-window mode — mandatory on BOS-less models, where anchored
       discovery is unusable and the histogram fallback mines junk-context
       attractors), else DISCOVERED by default; argv[4] = "fixed" measures
       the legacy 2000..2000+V window */
    vocab = (int *)malloc(V * sizeof *vocab);
    logits = (float *)malloc((size_t)m->vocab_size * sizeof *logits);
    if (!vocab || !logits) return 1;
    for (i = 0; i < V; ++i) vocab[i] = 2000 + (int)i;
    if (getenv("CNET_WINDOW_FILE")) {
        /* same loader contract as flagship_run: decimal ids, one per line,
           all unique and inside the model vocab, exactly V of them — the
           probe MUST measure the exact window the campaign mines */
        const char *wf = getenv("CNET_WINDOW_FILE");
        FILE *f = fopen(wf, "r");
        size_t got = 0, j;
        if (!f) {
            fprintf(stderr, "CNET_WINDOW_FILE %s: cannot open\n", wf);
            return 1;
        }
        while (got < V && fscanf(f, "%d", &vocab[got]) == 1) {
            if (vocab[got] < 0 || vocab[got] >= m->vocab_size) {
                fprintf(stderr, "CNET_WINDOW_FILE: id %d outside model "
                                "vocab\n", vocab[got]);
                fclose(f);
                return 1;
            }
            got++;
        }
        fclose(f);
        if (got != V) {
            fprintf(stderr, "CNET_WINDOW_FILE: need %lu ids, got %lu\n",
                    (unsigned long)V, (unsigned long)got);
            return 1;
        }
        for (i = 0; i < V; ++i)
            for (j = i + 1; j < V; ++j)
                if (vocab[i] == vocab[j]) {
                    fprintf(stderr, "CNET_WINDOW_FILE: duplicate id %d\n",
                            vocab[i]);
                    return 1;
                }
        printf("window: %lu tokens from %s (first: %d %d %d %d)\n",
               (unsigned long)V, wf, vocab[0], vocab[1], vocab[2], vocab[3]);
    } else if (!(argc > 4 && strcmp(argv[4], "fixed") == 0)) {
        int bos = m->bos_token_id;
        if (getenv("CNET_ORACLE_BOS") && getenv("CNET_ORACLE_BOS")[0] == '0')
            bos = -1;
        printf("bos anchor: %d\n", bos);
        size_t placed = cnet_window_discover(m, logits, vocab, V, 2, bos);
        printf("window: %lu/%lu discovered (first: %d %d %d %d)\n",
               (unsigned long)placed, (unsigned long)V, vocab[0], vocab[1],
               vocab[2], vocab[3]);
    } else {
        printf("window: FIXED legacy 2000..%d\n", (int)(2000 + V - 1));
    }

    /* pack the head's window columns: w_win[j][d] = "the weight the head
       applies to hidden dim d for window token j". Sources tried in order:
       the lm_head forest cascade ([D x V] as cce_gguf_add_linear_branch
       builds), then m->output / m->tok_emb in either orientation (tied-
       embedding models have no output.weight — the head IS the embedding). */
    {
        cce_cascade *head =
            cce_forest_get_resident(m->forest, "qwen2.lm_head");
        const float *src = NULL;
        int s0 = 0, s1 = 0;   /* src is [s0 x s1] row-major */
        p.scale_win = (float *)malloc((size_t)V * sizeof *p.scale_win);
        p.w_win = (float *)malloc((size_t)V * D * sizeof *p.w_win);
        if (!p.scale_win || !p.w_win) return 1;
        for (i = 0; i < V; ++i) p.scale_win[i] = 1.0f;
        if (head && head->num_blocks >= 1) {
            const cce_block *blk = &head->blocks[0];
            fprintf(stderr,
                    "head cascade: blocks=%d ndim=%d shape=[%d,%d] data=%s "
                    "w_q=%s w_trit=%s\n",
                    head->num_blocks, blk->weights.ndim,
                    blk->weights.shape[0], blk->weights.shape[1],
                    blk->weights.data ? "yes" : "NULL",
                    blk->w_q ? "yes" : "no", blk->w_trit ? "yes" : "no");
            if (blk->w_q && blk->w_scale && blk->weights.ndim == 2 &&
                blk->weights.shape[0] == D &&
                blk->weights.shape[1] > 0) {
                /* the ORACLE's head in CNET_ORACLE_INT8 campaigns: int8
                   codes [D x vocab] + per-output scale. Pack the window
                   columns as float codes; scale applied after the same
                   d-ascending float accumulation the oracle uses. */
                int vfull = blk->weights.shape[1];
                size_t j; int d;
                printf("head source: int8 oracle head (scale per column)\n");
                for (j = 0; j < V; ++j) {
                    if (vocab[j] >= vfull) { fprintf(stderr, "window id out of vocab\n"); return 1; }
                    for (d = 0; d < D; ++d)
                        p.w_win[j * (size_t)D + d] =
                            (float)blk->w_q[(size_t)d * vfull + vocab[j]];
                    p.scale_win[j] = blk->w_scale[vocab[j]];
                }
                goto head_ready;
            }
            if (blk->weights.data && blk->weights.ndim == 2 && !blk->w_q &&
                !blk->w_trit) {
                src = blk->weights.data;
                s0 = blk->weights.shape[0];
                s1 = blk->weights.shape[1];
            }
        }
        if (!src && m->output.data && m->output.ndim == 2) {
            fprintf(stderr, "falling back to m->output [%d,%d]\n",
                    m->output.shape[0], m->output.shape[1]);
            src = m->output.data;
            s0 = m->output.shape[0];
            s1 = m->output.shape[1];
        }
        if (!src && m->tok_emb.data && m->tok_emb.ndim == 2) {
            fprintf(stderr, "falling back to tied tok_emb [%d,%d]\n",
                    m->tok_emb.shape[0], m->tok_emb.shape[1]);
            src = m->tok_emb.data;
            s0 = m->tok_emb.shape[0];
            s1 = m->tok_emb.shape[1];
        }
        if (!src) {
            fprintf(stderr, "no usable head source found — cannot probe\n");
            return 1;
        }
        if (s0 == D) {
            /* [D x vocab]: column slice */
            size_t j; int d;
            for (j = 0; j < V; ++j) {
                if (vocab[j] >= s1) { fprintf(stderr, "window id out of vocab\n"); return 1; }
                for (d = 0; d < D; ++d)
                    p.w_win[j * (size_t)D + d] =
                        src[(size_t)d * s1 + vocab[j]];
            }
        } else if (s1 == D) {
            /* [vocab x D]: row slice (tied-embedding orientation) */
            size_t j;
            for (j = 0; j < V; ++j) {
                if (vocab[j] >= s0) { fprintf(stderr, "window id out of vocab\n"); return 1; }
                memcpy(p.w_win + j * (size_t)D,
                       src + (size_t)vocab[j] * D,
                       (size_t)D * sizeof(float));
            }
        } else {
            fprintf(stderr, "head source [%d,%d] fits neither orientation "
                            "for D=%d\n", s0, s1, D);
            return 1;
        }
    head_ready:;
    }
    p.norm_w = m->output_norm.data;
    p.eps = (m->rms_eps > 0.0f) ? m->rms_eps : 1e-6f;
    p.D = D;
    p.vw = (int)V;

    sat_top1 = (int *)calloc(N, sizeof *sat_top1);
    sat_top3 = (int *)calloc(N, sizeof *sat_top3);
    agree1 = (size_t *)calloc((size_t)L, sizeof *agree1);
    agree3 = (size_t *)calloc((size_t)L, sizeof *agree3);
    finals = (int *)calloc(N * DP_TOPK, sizeof *finals);
    if (!logits || !sat_top1 || !sat_top3 || !agree1 || !agree3 || !finals)
        return 1;

    /* pre-forward forensics: NaNs in LOADED tensors = loader/file problem;
       clean tensors + NaN stream = layer-0 compute problem */
    {
        struct { const char *name; const cce_tensor *t; } scan[4];
        int si;
        scan[0].name = "tok_emb";     scan[0].t = &m->tok_emb;
        scan[1].name = "output_norm"; scan[1].t = &m->output_norm;
        scan[2].name = "attn_norm0";  scan[2].t = m->attn_norm ? &m->attn_norm[0] : NULL;
        scan[3].name = "ffn_norm0";   scan[3].t = m->ffn_norm ? &m->ffn_norm[0] : NULL;
        for (si = 0; si < 4; ++si) {
            size_t nn = 0, k3;
            float mn = 0, mx = 0;
            if (!scan[si].t || !scan[si].t->data) {
                printf("tensor %-12s: NO DATA\n", scan[si].name);
                continue;
            }
            mn = mx = scan[si].t->data[0];
            for (k3 = 0; k3 < scan[si].t->numel; ++k3) {
                float v = scan[si].t->data[k3];
                if (v != v) nn++;
                else { if (v < mn) mn = v; if (v > mx) mx = v; }
            }
            printf("tensor %-12s: numel=%zu nans=%zu min=%.4g max=%.4g\n",
                   scan[si].name, scan[si].t->numel, nn, (double)mn,
                   (double)mx);
        }
        if (m->tok_emb.data && m->tok_emb.numel >= (size_t)3 * D) {
            size_t nn2 = 0; int d3;
            for (d3 = 0; d3 < D; ++d3)
                if (m->tok_emb.data[(size_t)2 * D + d3] !=
                    m->tok_emb.data[(size_t)2 * D + d3]) nn2++;
            printf("embedding row for token 2 (bos): nans=%zu/%d\n", nn2, D);
        }
    }

    cce_gguf_set_layer_tap(probe_tap, &p);

    t0 = wall_s();
    for (i = 0; i < N; ++i) {
        /* the campaign's actual query shape: [t, w], both in the window */
        int tokens[3];
        int nt = 0;
        int fl;
        if (m->bos_token_id >= 0 &&
            !(getenv("CNET_ORACLE_BOS") &&
              getenv("CNET_ORACLE_BOS")[0] == '0'))
            tokens[nt++] = m->bos_token_id;
        tokens[nt++] = vocab[(i * 37u + 11u) % V];
        tokens[nt++] = vocab[(i * 101u + 3u) % V];
        memset(p.top, -1, sizeof p.top);
        p.layers_seen = 0;
        g_forensic = (i == 0);
        m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, tokens, nt, logits,
                                   m->vocab_size) != CCE_OK) {
            fprintf(stderr, "forward failed at probe %lu\n", (unsigned long)i);
            return 1;
        }
        if (i == 0) {
            /* logit forensics: FLAT logits mean the forward is degenerate
               (argmax=0 is then the strict-> tie-break, not a prediction) */
            float mn = logits[0], mx = logits[0];
            double mean = 0.0;
            int vs2 = m->vocab_size, jj2;
            for (jj2 = 0; jj2 < vs2; ++jj2) {
                if (logits[jj2] < mn) mn = logits[jj2];
                if (logits[jj2] > mx) mx = logits[jj2];
                mean += logits[jj2];
            }
            printf("probe 0 full-vocab logits: min=%.6f max=%.6f mean=%.6f "
                   "spread=%.6f\n", (double)mn, (double)mx,
                   mean / (double)vs2, (double)(mx - mn));
        }
        if (p.layers_seen != L) {
            fprintf(stderr, "tap saw %d layers, expected %d\n",
                    p.layers_seen, L);
            return 1;
        }
        fl = L - 1;
        /* VALIDITY: the probe's own final-layer decision must equal the
           decision computed from the forward's returned logits (same head,
           same math) — if not, the probe is broken, not the model shallow. */
        {
            int chk[DP_TOPK];
            int taken[4096] = {0};
            int k2, j2;
            for (k2 = 0; k2 < DP_TOPK; ++k2) {
                int bi = -1;
                float bl = 0.0f;
                for (j2 = 0; j2 < (int)V; ++j2) {
                    if (taken[j2]) continue;
                    if (bi == -1 || logits[vocab[j2]] > bl) {
                        bl = logits[vocab[j2]];
                        bi = j2;
                    }
                }
                taken[bi] = 1;
                chk[k2] = bi;
            }
            if (memcmp(chk, p.top[fl], sizeof chk) != 0) {
                probe_head_mismatch++;
                if (probe_head_mismatch <= 4)
                    fprintf(stderr,
                            "probe %lu: HEAD MISMATCH probe=[%d,%d,%d] "
                            "model=[%d,%d,%d]\n", (unsigned long)i,
                            p.top[fl][0], p.top[fl][1], p.top[fl][2],
                            chk[0], chk[1], chk[2]);
            }
        }
        if (i < 12)
            printf("probe %3lu (t=%d,w=%d): L1=[%d,%d,%d] L%d=[%d,%d,%d] "
                   "L%d=[%d,%d,%d]\n", (unsigned long)i, tokens[nt - 2],
                   tokens[nt - 1], p.top[0][0], p.top[0][1], p.top[0][2], L / 2,
                   p.top[L / 2 - 1][0], p.top[L / 2 - 1][1],
                   p.top[L / 2 - 1][2], L, p.top[fl][0], p.top[fl][1],
                   p.top[fl][2]);
        memcpy(finals + i * DP_TOPK, p.top[fl], sizeof p.top[fl]);
        /* per-layer agreement with FULL depth + saturation depths */
        for (l = 0; l < L; ++l) {
            if (p.top[l][0] == p.top[fl][0]) agree1[l]++;
            if (memcmp(p.top[l], p.top[fl], sizeof p.top[l]) == 0)
                agree3[l]++;
        }
        for (l = fl; l >= 0; --l) {
            if (p.top[l][0] != p.top[fl][0]) break;
        }
        sat_top1[i] = l + 2;              /* layers needed for stable top-1 */
        for (l = fl; l >= 0; --l) {
            if (memcmp(p.top[l], p.top[fl], sizeof p.top[l]) != 0) break;
        }
        sat_top3[i] = l + 2;              /* layers needed for stable top-3 */
    }
    printf("probe wall: %.1fs (%.2fs/probe, full %d-layer forwards)\n",
           wall_s() - t0, (wall_s() - t0) / (double)N, L);

    cce_gguf_set_layer_tap(NULL, NULL);

    /* verdict table */
    printf("\n cap K | argmax==full | top3==full   (over %lu probes)\n",
           (unsigned long)N);
    for (l = 0; l < L; ++l) {
        if (agree3[l] == N || agree1[l] == N || (l + 1) % 4 == 0 ||
            l == L - 1) {
            printf("  %4d | %6lu/%lu     | %6lu/%lu %s\n", l + 1,
                   (unsigned long)agree1[l], (unsigned long)N,
                   (unsigned long)agree3[l], (unsigned long)N,
                   (agree3[l] == N && l < L - 1) ? "  <-- decision-complete"
                                                 : "");
        }
    }
    {
        /* DEGENERACY check: a constant ranking trivially "saturates" at
           layer 1 and means the WINDOW is dead on these probes, not that
           the model is shallow. Count distinct full-depth decisions. */
        size_t distinct = 0, a, b;
        for (a = 0; a < N; ++a) {
            int dup = 0;
            for (b = 0; b < a && !dup; ++b)
                if (memcmp(finals + a * DP_TOPK, finals + b * DP_TOPK,
                           DP_TOPK * sizeof(int)) == 0) dup = 1;
            if (!dup) distinct++;
        }
        printf("\nprobe-head vs model-head mismatches: %lu/%lu%s\n",
               (unsigned long)probe_head_mismatch, (unsigned long)N,
               probe_head_mismatch ? "  <-- PROBE INVALID" : "  (probe valid)");
        printf("distinct full-depth top-3 decisions: %lu/%lu%s\n",
               (unsigned long)distinct, (unsigned long)N,
               distinct <= 2 ? "  <-- WINDOW DEGENERATE: constant slice, "
                               "saturation depth is meaningless" : "");
    }
    {
        int max1 = 0, max3 = 0;
        for (i = 0; i < N; ++i) {
            if (sat_top1[i] > max1) max1 = sat_top1[i];
            if (sat_top3[i] > max3) max3 = sat_top3[i];
        }
        printf("\nsaturation depth (max over probes): top-1 %d/%d, "
               "top-3 %d/%d\n", max1, L, max3, L);
        if (max3 < L)
            printf("VERDICT: decisions saturate at depth %d — a gated cap "
                   "of %d layers is worth trying (%.1fx layer work)\n",
                   max3, max3, (double)L / (double)max3);
        else
            printf("VERDICT: decisions change up to the last layer — a "
                   "capped oracle would CHANGE mined knowledge; idea is "
                   "dead for this model\n");
    }

    free(p.w_win);
    free(p.scale_win);
    free(finals);
    free(vocab);
    free(logits);
    free(sat_top1);
    free(sat_top3);
    free(agree1);
    free(agree3);
    cce_anymodel_free(am);
    return 0;
}
