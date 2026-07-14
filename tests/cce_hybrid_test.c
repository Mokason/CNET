/* cce_hybrid: native hybrid attention+SSM runner, born exact.
 *
 * A hybrid (jamba/zamba-class) model interleaves llama/qwen2-style attention
 * layers with mamba-1 selective-SSM layers over ONE shared residual stream.
 * This test proves the runner runs a genuine MIXED forward — not two runners
 * with concatenated logits — by:
 *
 *  1. Generating deterministic weights for a 3-layer model in order
 *     [ATTN, SSM, ATTN] (>=1 of each; the SSM layer sits between two attention
 *     layers so model order is exercised end to end).
 *  2. Running an INDEPENDENT double-precision reference (plain loops written
 *     straight from the definitions — no CCE code) that threads one residual
 *     stream through the layers in order, keeping a per-attention-layer KV
 *     cache and per-SSM-layer conv/scan state side by side.
 *  3. Writing the weights as a GGUF (llama.cpp jamba naming), loading through
 *     cce_hybrid_load (forest decomposition + cascades), and comparing every
 *     token's logits against the reference. Logits must be finite and
 *     non-degenerate.
 *  4. Mutation: perturbing an ATTENTION sublayer weight changes the final
 *     logits, and (independently) perturbing an SSM sublayer weight changes
 *     them again — proving BOTH sublayers are actually in the token path.
 *  5. Detection: the hybrid gguf is classified CCE_ARCH_FAMILY_HYBRID and is
 *     runnable via cce_hybrid_load; cce_anymodel_open autoloads it into ->hybrid.
 *  6. Pure transformer and pure SSM still dispatch to their own runners, and
 *     incompatible loaders REFUSE structural mismatches (the hybrid loader
 *     refuses pure-attn / pure-ssm; the ssm + transformer loaders refuse the
 *     hybrid) instead of silently dropping a sublayer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../include/cce/cce_hybrid.h"
#include "../include/cce/cce_ssm.h"
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_detect.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

/* ---- dims (small, non-degenerate, mutually distinct) ---- */
#define L_     3
#define D_     8         /* residual width */
#define NH_    2         /* attn query heads */
#define HD_    4         /* head dim */
#define QD_    (NH_ * HD_)   /* 8 */
#define NKV_   1         /* kv heads (GQA group 2) */
#define KD_    (NKV_ * HD_)  /* 4 */
#define VD_    (NKV_ * HD_)  /* 4 */
#define FFN_   16        /* attn MLP hidden */
#define E_     10        /* ssm inner */
#define N_     3         /* ssm state */
#define K_     3         /* ssm conv width */
#define R_     2         /* ssm dt rank */
#define XDB_   (R_ + 2 * N_) /* 8 */
#define V_     12        /* vocab */
#define ROPE_BASE 10000.0
#define EPS_   1e-5

enum { ATTN = 0, SSM = 1 };
static const int g_kind[L_] = { ATTN, SSM, ATTN };

/* ---- deterministic weights (all layers carry both kinds' fields; only the
   fields matching the layer kind are ever read, by both ref and loader) ---- */

static uint64_t g_seed = 0x9E3779B97F4A7C15ULL;
static float rndf(void) {
    g_seed = g_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((double)(g_seed >> 33) / 2147483648.0 - 1.0) * 0.5f;
}

typedef struct {
    /* shared */
    float emb[V_][D_];
    float out_norm[D_];
    float mixer_norm[L_][D_];       /* attn_norm — pre-mixer, both kinds */
    /* attention */
    float wq[L_][QD_][D_];
    float wk[L_][KD_][D_];
    float wv[L_][VD_][D_];
    float wo[L_][D_][QD_];
    float ffn_norm[L_][D_];
    float wgate[L_][FFN_][D_];
    float wup[L_][FFN_][D_];
    float wdown[L_][D_][FFN_];
    /* ssm */
    float in_w[L_][2 * E_][D_];
    float conv_w[L_][E_][K_];
    float conv_b[L_][E_];
    float x_w[L_][XDB_][E_];
    float dt_w[L_][E_][R_];
    float dt_b[L_][E_];
    float a_log[L_][E_][N_];
    float dvec[L_][E_];
    float out_w[L_][D_][E_];
} hyb_weights;

static void gen_weights(hyb_weights* w) {
    float* p = (float*)w;
    size_t n = sizeof(*w) / sizeof(float);
    for (size_t i = 0; i < n; i++) p[i] = rndf();
    /* norms near 1 like trained models */
    for (int i = 0; i < D_; i++) w->out_norm[i] = 1.0f + 0.1f * w->out_norm[i];
    for (int l = 0; l < L_; l++)
        for (int i = 0; i < D_; i++) {
            w->mixer_norm[l][i] = 1.0f + 0.1f * w->mixer_norm[l][i];
            w->ffn_norm[l][i]   = 1.0f + 0.1f * w->ffn_norm[l][i];
        }
}

/* ---- independent double-precision reference ---- */

static double ref_silu(double x)     { return x / (1.0 + exp(-x)); }
static double ref_softplus(double x) { return (x > 20.0) ? x : log1p(exp(x)); }

static void ref_rmsnorm(const double* in, const float* w, int d, double* out) {
    double ss = 0.0;
    for (int i = 0; i < d; i++) ss += in[i] * in[i];
    ss = 1.0 / sqrt(ss / d + EPS_);
    for (int i = 0; i < d; i++) out[i] = in[i] * ss * (double)w[i];
}

/* NEOX rotary, mirroring gguf_apply_rope (pair i with i+half). */
static void ref_rope(double* vec, int head_dim, int pos) {
    int half = head_dim / 2;
    for (int i = 0; i < half; i++) {
        double freq = 1.0 / pow(ROPE_BASE, (double)(2 * i) / head_dim);
        double val = (double)pos * freq;
        double c = cos(val), s = sin(val);
        double v0 = vec[i], v1 = vec[i + half];
        vec[i]        = v0 * c - v1 * s;
        vec[i + half] = v0 * s + v1 * c;
    }
}

static void ref_forward(const hyb_weights* w, const int* tokens, int n_tokens,
                        double logits[][V_]) {
    /* state, indexed per layer (unused slots for the other kind are ignored) */
    static double kc[L_][16][KD_];   /* KV cache (>= n_tokens rows) */
    static double vc[L_][16][VD_];
    double conv_state[L_][E_][K_];
    double ssm_state[L_][E_][N_];
    memset(conv_state, 0, sizeof(conv_state));
    memset(ssm_state, 0, sizeof(ssm_state));

    for (int t = 0; t < n_tokens; t++) {
        double x[D_];
        for (int i = 0; i < D_; i++) x[i] = w->emb[tokens[t]][i];

        for (int l = 0; l < L_; l++) {
            double ln[D_];
            ref_rmsnorm(x, w->mixer_norm[l], D_, ln);

            if (g_kind[l] == ATTN) {
                /* q/k/v projections */
                double q[QD_], k[KD_], v[VD_];
                for (int o = 0; o < QD_; o++) { double a = 0; for (int i = 0; i < D_; i++) a += (double)w->wq[l][o][i] * ln[i]; q[o] = a; }
                for (int o = 0; o < KD_; o++) { double a = 0; for (int i = 0; i < D_; i++) a += (double)w->wk[l][o][i] * ln[i]; k[o] = a; }
                for (int o = 0; o < VD_; o++) { double a = 0; for (int i = 0; i < D_; i++) a += (double)w->wv[l][o][i] * ln[i]; v[o] = a; }

                /* rope each q head + each k head at position t */
                for (int h = 0; h < NH_; h++)  ref_rope(q + h * HD_, HD_, t);
                for (int h = 0; h < NKV_; h++) ref_rope(k + h * HD_, HD_, t);

                for (int i = 0; i < KD_; i++) kc[l][t][i] = k[i];
                for (int i = 0; i < VD_; i++) vc[l][t][i] = v[i];

                /* GQA causal attention, scale 1/sqrt(head_dim) */
                double attn_out[QD_];
                double scale = 1.0 / sqrt((double)HD_);
                for (int h = 0; h < NH_; h++) {
                    int kvh = h / (NH_ / NKV_);
                    const double* qh = q + h * HD_;
                    double sc[16], mx = -1e300, sum = 0.0;
                    for (int j = 0; j <= t; j++) {
                        double a = 0; for (int d = 0; d < HD_; d++) a += qh[d] * kc[l][j][kvh * HD_ + d];
                        sc[j] = a * scale; if (sc[j] > mx) mx = sc[j];
                    }
                    for (int j = 0; j <= t; j++) { sc[j] = exp(sc[j] - mx); sum += sc[j]; }
                    for (int d = 0; d < HD_; d++) {
                        double acc = 0; for (int j = 0; j <= t; j++) acc += (sc[j] / sum) * vc[l][j][kvh * HD_ + d];
                        attn_out[h * HD_ + d] = acc;
                    }
                }

                /* o_proj + residual */
                for (int o = 0; o < D_; o++) { double a = 0; for (int i = 0; i < QD_; i++) a += (double)w->wo[l][o][i] * attn_out[i]; x[o] += a; }

                /* MLP: RMSNorm -> SwiGLU */
                double ln2[D_];
                ref_rmsnorm(x, w->ffn_norm[l], D_, ln2);
                double gate[FFN_], up[FFN_];
                for (int o = 0; o < FFN_; o++) { double a = 0; for (int i = 0; i < D_; i++) a += (double)w->wgate[l][o][i] * ln2[i]; gate[o] = a; }
                for (int o = 0; o < FFN_; o++) { double a = 0; for (int i = 0; i < D_; i++) a += (double)w->wup[l][o][i]   * ln2[i]; up[o]   = a; }
                double mid[FFN_];
                for (int o = 0; o < FFN_; o++) mid[o] = ref_silu(gate[o]) * up[o];
                for (int o = 0; o < D_; o++) { double a = 0; for (int i = 0; i < FFN_; i++) a += (double)w->wdown[l][o][i] * mid[i]; x[o] += a; }
            } else { /* SSM: mamba-1 selective scan (mirrors cce_ssm) */
                double xz[2 * E_];
                for (int o = 0; o < 2 * E_; o++) { double a = 0; for (int i = 0; i < D_; i++) a += (double)w->in_w[l][o][i] * ln[i]; xz[o] = a; }

                double xc[E_];
                for (int e = 0; e < E_; e++) {
                    for (int kk = 0; kk < K_ - 1; kk++) conv_state[l][e][kk] = conv_state[l][e][kk + 1];
                    conv_state[l][e][K_ - 1] = xz[e];
                    double a = w->conv_b[l][e];
                    for (int kk = 0; kk < K_; kk++) a += (double)w->conv_w[l][e][kk] * conv_state[l][e][kk];
                    xc[e] = ref_silu(a);
                }

                double xdb[XDB_];
                for (int o = 0; o < XDB_; o++) { double a = 0; for (int e = 0; e < E_; e++) a += (double)w->x_w[l][o][e] * xc[e]; xdb[o] = a; }
                const double* B = xdb + R_;
                const double* C = xdb + R_ + N_;

                double dt[E_];
                for (int e = 0; e < E_; e++) {
                    double a = w->dt_b[l][e];
                    for (int r = 0; r < R_; r++) a += (double)w->dt_w[l][e][r] * xdb[r];
                    dt[e] = ref_softplus(a);
                }

                double y[E_];
                for (int e = 0; e < E_; e++) {
                    double acc = 0;
                    for (int n = 0; n < N_; n++) {
                        double dA = exp(dt[e] * -exp((double)w->a_log[l][e][n]));
                        ssm_state[l][e][n] = dA * ssm_state[l][e][n] + dt[e] * B[n] * xc[e];
                        acc += ssm_state[l][e][n] * C[n];
                    }
                    y[e] = (acc + (double)w->dvec[l][e] * xc[e]) * ref_silu(xz[E_ + e]);
                }

                for (int o = 0; o < D_; o++) { double a = 0; for (int e = 0; e < E_; e++) a += (double)w->out_w[l][o][e] * y[e]; x[o] += a; }
            }
        }

        /* final norm + tied head */
        double xf[D_];
        ref_rmsnorm(x, w->out_norm, D_, xf);
        for (int vv = 0; vv < V_; vv++) { double a = 0; for (int i = 0; i < D_; i++) a += (double)w->emb[vv][i] * xf[i]; logits[t][vv] = a; }
    }
}

/* ---- gguf writer (F32, real data, llama.cpp jamba naming) ---- */

typedef struct { char name[128]; int shape[4]; int ndim; const float* data; size_t numel; } gg_entry;

static void gg_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void gg_u64(FILE* f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void gg_str(FILE* f, const char* s) { gg_u64(f, strlen(s)); fwrite(s, 1, strlen(s), f); }
static void gg_kv_str(FILE* f, const char* k, const char* v) { gg_str(f, k); gg_u32(f, 8); gg_str(f, v); }
static void gg_kv_u32(FILE* f, const char* k, uint32_t v) { gg_str(f, k); gg_u32(f, 4); gg_u32(f, v); }

static void gguf_write(const char* path, const gg_entry* ents, int n_ents) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    gg_u32(f, 3);
    gg_u64(f, (uint64_t)n_ents);
    gg_u64(f, 6);   /* n kv */
    gg_kv_str(f, "general.architecture", "jamba");
    gg_kv_u32(f, "jamba.block_count", L_);
    gg_kv_u32(f, "jamba.embedding_length", D_);
    gg_kv_u32(f, "jamba.attention.head_count", NH_);
    gg_kv_u32(f, "jamba.attention.head_count_kv", NKV_);
    gg_kv_u32(f, "jamba.feed_forward_length", FFN_);
    /* GGUF records dims in ggml ne order (innermost first) — the REVERSE of
       torch — with identical row-major bytes; emit reversed so the fixture
       matches reality (same convention as cce_ssm_test). */
    uint64_t off = 0;
    for (int i = 0; i < n_ents; i++) {
        gg_str(f, ents[i].name);
        gg_u32(f, (uint32_t)ents[i].ndim);
        for (int d = ents[i].ndim - 1; d >= 0; d--) gg_u64(f, (uint64_t)ents[i].shape[d]);
        gg_u32(f, 0 /* F32 */);
        gg_u64(f, off);
        off += ents[i].numel * 4;
    }
    /* pad to the general.alignment (32) boundary before tensor data */
    {
        long here = ftell(f);
        long pad = (long)((32 - (here % 32)) % 32);
        for (long p = 0; p < pad; p++) fputc(0, f);
    }
    for (int i = 0; i < n_ents; i++) fwrite(ents[i].data, 4, ents[i].numel, f);
    fclose(f);
}

/* Build tensor entries for the given per-layer kinds (attn tensors for ATTN
   layers, ssm tensors for SSM layers; shared embd/norm/head always). */
static int build_entries(const hyb_weights* w, gg_entry* e, const int* kinds, int n_layer) {
    int n = 0;
#define ADD(nm, d0, d1, d2, nd, ptr, cnt) do { \
        snprintf(e[n].name, sizeof(e[n].name), "%s", nm); \
        e[n].shape[0] = d0; e[n].shape[1] = d1; e[n].shape[2] = d2; \
        e[n].ndim = nd; e[n].data = ptr; e[n].numel = cnt; n++; } while (0)
    ADD("token_embd.weight", V_, D_, 0, 2, &w->emb[0][0], (size_t)V_ * D_);
    ADD("output_norm.weight", D_, 0, 0, 1, w->out_norm, D_);
    char nm[128];
    for (int l = 0; l < n_layer; l++) {
        snprintf(nm, sizeof(nm), "blk.%d.attn_norm.weight", l);   ADD(nm, D_, 0, 0, 1, w->mixer_norm[l], D_);
        if (kinds[l] == ATTN) {
            snprintf(nm, sizeof(nm), "blk.%d.attn_q.weight", l);       ADD(nm, QD_, D_, 0, 2, &w->wq[l][0][0], (size_t)QD_ * D_);
            snprintf(nm, sizeof(nm), "blk.%d.attn_k.weight", l);       ADD(nm, KD_, D_, 0, 2, &w->wk[l][0][0], (size_t)KD_ * D_);
            snprintf(nm, sizeof(nm), "blk.%d.attn_v.weight", l);       ADD(nm, VD_, D_, 0, 2, &w->wv[l][0][0], (size_t)VD_ * D_);
            snprintf(nm, sizeof(nm), "blk.%d.attn_output.weight", l);  ADD(nm, D_, QD_, 0, 2, &w->wo[l][0][0], (size_t)D_ * QD_);
            snprintf(nm, sizeof(nm), "blk.%d.ffn_norm.weight", l);     ADD(nm, D_, 0, 0, 1, w->ffn_norm[l], D_);
            snprintf(nm, sizeof(nm), "blk.%d.ffn_gate.weight", l);     ADD(nm, FFN_, D_, 0, 2, &w->wgate[l][0][0], (size_t)FFN_ * D_);
            snprintf(nm, sizeof(nm), "blk.%d.ffn_up.weight", l);       ADD(nm, FFN_, D_, 0, 2, &w->wup[l][0][0], (size_t)FFN_ * D_);
            snprintf(nm, sizeof(nm), "blk.%d.ffn_down.weight", l);     ADD(nm, D_, FFN_, 0, 2, &w->wdown[l][0][0], (size_t)D_ * FFN_);
        } else {
            snprintf(nm, sizeof(nm), "blk.%d.ssm_in.weight", l);       ADD(nm, 2 * E_, D_, 0, 2, &w->in_w[l][0][0], (size_t)2 * E_ * D_);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_conv1d.weight", l);   ADD(nm, E_, K_, 0, 2, &w->conv_w[l][0][0], (size_t)E_ * K_);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_conv1d.bias", l);     ADD(nm, E_, 0, 0, 1, w->conv_b[l], E_);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_x.weight", l);        ADD(nm, XDB_, E_, 0, 2, &w->x_w[l][0][0], (size_t)XDB_ * E_);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_dt.weight", l);       ADD(nm, E_, R_, 0, 2, &w->dt_w[l][0][0], (size_t)E_ * R_);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_dt.bias", l);         ADD(nm, E_, 0, 0, 1, w->dt_b[l], E_);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_a", l);               ADD(nm, E_, N_, 0, 2, &w->a_log[l][0][0], (size_t)E_ * N_);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_d", l);               ADD(nm, E_, 0, 0, 1, w->dvec[l], E_);
            snprintf(nm, sizeof(nm), "blk.%d.ssm_out.weight", l);      ADD(nm, D_, E_, 0, 2, &w->out_w[l][0][0], (size_t)D_ * E_);
        }
    }
#undef ADD
    return n;
}

/* reach into the forest and return a branch's first-block weight buffer */
static cce_cascade* find_branch(cce_forest* f, const char* name) {
    for (int i = 0; i < f->num_branches; i++)
        if (strcmp(f->branches[i].name, name) == 0) return f->branches[i].cascade;
    return NULL;
}

static double maxdiff(const float* a, const float* b, int n) {
    double d = 0; for (int i = 0; i < n; i++) { double e = fabs((double)a[i] - b[i]); if (e > d) d = e; }
    return d;
}

int main(void) {
    printf("=== cce_hybrid: native attention+ssm mixed runner, born exact ===\n");

    hyb_weights* w = (hyb_weights*)malloc(sizeof(hyb_weights));
    gen_weights(w);

    static const int tokens[4] = { 2, 5, 1, 9 };
    const int NT = 4;

    /* reference */
    double ref[4][V_];
    ref_forward(w, tokens, NT, ref);

    /* hybrid gguf fixture [ATTN, SSM, ATTN] */
    gg_entry ents[128];
    int n_h = build_entries(w, ents, g_kind, L_);
    gguf_write("hybrid_test.gguf", ents, n_h);

    /* 1. load + derived structure */
    cce_hybrid_model* m = NULL;
    CHECK(cce_hybrid_load(&m, "hybrid_test.gguf") == CCE_OK && m, "hybrid gguf loads");
    if (!m) {
        printf("cannot continue (load failed)\n");
        printf("\n%d checks, %d failed -> %s\n", checks, fails + 1, "FAIL");
        return 1;
    }
    CHECK(m->n_layer == L_ && m->d_model == D_ && m->vocab_size == V_, "core dims derived");
    CHECK(m->n_attn_layer == 2 && m->n_ssm_layer == 1, "two attn + one ssm layer");
    CHECK(m->layer_kind[0] == CCE_HYBRID_LAYER_ATTN &&
          m->layer_kind[1] == CCE_HYBRID_LAYER_SSM &&
          m->layer_kind[2] == CCE_HYBRID_LAYER_ATTN, "per-layer kind in model order");
    CHECK(m->n_head == NH_ && m->n_kv_head == NKV_ && m->head_dim == HD_, "attention geometry derived");
    CHECK(m->d_inner == E_ && m->d_state == N_ && m->d_conv == K_ && m->dt_rank == R_, "ssm geometry derived");
    CHECK(m->head_cas != NULL, "shared lm head decomposed into a specialist");

    /* 2. per-token logits vs the independent double reference */
    float hy[4][V_];
    double worst = 0, worst_abs = 0;
    int all_finite = 1, degenerate = 0;
    cce_hybrid_reset(m);
    for (int t = 0; t < NT; t++) {
        CHECK(cce_hybrid_forward(m, &tokens[t], 1, hy[t], V_) == CCE_OK, "mixed forward ok");
        float lo = hy[t][0], hi = hy[t][0];
        for (int v = 0; v < V_; v++) {
            if (!isfinite(hy[t][v])) all_finite = 0;
            if (hy[t][v] < lo) lo = hy[t][v];
            if (hy[t][v] > hi) hi = hy[t][v];
            double diff = fabs((double)hy[t][v] - ref[t][v]);
            double ratio = diff / (1e-4 + 5e-4 * fabs(ref[t][v]));
            if (ratio > worst) worst = ratio;
            if (diff > worst_abs) worst_abs = diff;
        }
        if (hi - lo < 1e-3f) degenerate = 1;
    }
    printf("  vs double reference: max |diff| %.3g, worst tolerance ratio %.3g\n", worst_abs, worst);
    CHECK(all_finite, "all logits finite");
    CHECK(!degenerate, "logits non-degenerate (not a flat distribution)");
    CHECK(worst < 1.0, "matches independent double-precision reference (atol 1e-4, rtol 5e-4)");

    /* 3. batch == incremental (single mixed pass over the whole prompt) */
    cce_hybrid_reset(m);
    float batch[V_];
    CHECK(cce_hybrid_forward(m, tokens, NT, batch, V_) == CCE_OK, "batch forward ok");
    CHECK(maxdiff(batch, hy[NT - 1], V_) == 0.0, "one batch call == token-by-token (mixed state discipline)");

    /* 4. mutation proves BOTH sublayers are in the path.
       Baseline first (fresh state), then scale one attention branch, then one
       ssm branch, re-running the whole prompt each time. */
    cce_hybrid_reset(m);
    float base[V_];
    for (int t = 0; t < NT; t++) cce_hybrid_forward(m, &tokens[t], 1, base, V_);

    cce_cascade* attn_o = find_branch(m->forest, "hybrid.blk.0.o_proj");   /* layer 0 = ATTN */
    CHECK(attn_o != NULL, "attention o_proj specialist present");
    if (attn_o) { cce_block* b = &attn_o->blocks[0]; for (size_t i = 0; i < b->weights.numel; i++) b->weights.data[i] *= 1.5f; }
    cce_hybrid_reset(m);
    float mut_a[V_];
    for (int t = 0; t < NT; t++) cce_hybrid_forward(m, &tokens[t], 1, mut_a, V_);
    CHECK(maxdiff(mut_a, base, V_) > 1e-3, "mutating an ATTENTION sublayer weight changes final logits");

    cce_cascade* ssm_out = find_branch(m->forest, "hybrid.blk.1.out_proj"); /* layer 1 = SSM */
    CHECK(ssm_out != NULL, "ssm out_proj specialist present");
    if (ssm_out) { cce_block* b = &ssm_out->blocks[0]; for (size_t i = 0; i < b->weights.numel; i++) b->weights.data[i] *= 1.5f; }
    cce_hybrid_reset(m);
    float mut_s[V_];
    for (int t = 0; t < NT; t++) cce_hybrid_forward(m, &tokens[t], 1, mut_s, V_);
    CHECK(maxdiff(mut_s, mut_a, V_) > 1e-3, "mutating an SSM sublayer weight changes final logits");

    cce_hybrid_free(m); m = NULL;

    /* 5. detection + universal dispatch */
    {
        cce_model_info info;
        CHECK(cce_detect_file("hybrid_test.gguf", &info) == CCE_OK &&
              info.family == CCE_ARCH_FAMILY_HYBRID && info.runnable == 1 &&
              strcmp(info.runner, "cce_hybrid_load") == 0,
              "detect: hybrid gguf runnable via cce_hybrid_load (not misclassified mamba)");

        cce_anymodel* am = NULL;
        CHECK(cce_anymodel_open(&am, "hybrid_test.gguf") == CCE_OK && am && am->hybrid,
              "anymodel autoloads a hybrid gguf into the hybrid runner");
        if (am) {
            float lg[V_];
            CHECK(cce_hybrid_forward(am->hybrid, tokens, NT, lg, V_) == CCE_OK &&
                  maxdiff(lg, hy[NT - 1], V_) == 0.0,
                  "anymodel hybrid forward reproduces the loader's logits");
            cce_anymodel_free(am);
        }
    }

    /* 6. pure transformer + pure SSM still dispatch; incompatible loaders refuse */
    {
        /* pure-attention model: every layer ATTN */
        int kinds_attn[L_]; for (int l = 0; l < L_; l++) kinds_attn[l] = ATTN;
        int n_a = build_entries(w, ents, kinds_attn, L_);
        gguf_write("hybrid_pure_attn.gguf", ents, n_a);

        /* pure-ssm model: every layer SSM */
        int kinds_ssm[L_]; for (int l = 0; l < L_; l++) kinds_ssm[l] = SSM;
        int n_s = build_entries(w, ents, kinds_ssm, L_);
        gguf_write("hybrid_pure_ssm.gguf", ents, n_s);

        cce_model_info info;
        CHECK(cce_detect_file("hybrid_pure_attn.gguf", &info) == CCE_OK &&
              info.family == CCE_ARCH_FAMILY_LLAMA && info.runnable == 1 &&
              strcmp(info.runner, "cce_gguf_load_model") == 0,
              "pure-attention gguf still dispatches to the transformer runner");
        CHECK(cce_detect_file("hybrid_pure_ssm.gguf", &info) == CCE_OK &&
              info.family == CCE_ARCH_FAMILY_MAMBA && info.runnable == 1 &&
              strcmp(info.runner, "cce_ssm_load") == 0,
              "pure-ssm gguf still dispatches to the ssm runner (not hybrid)");

        /* pure transformer dispatch works end-to-end via the universal opener */
        cce_anymodel* at = NULL;
        CHECK(cce_anymodel_open(&at, "hybrid_pure_attn.gguf") == CCE_OK && at && at->transformer,
              "anymodel loads the pure-attention model through the transformer runner");
        if (at) cce_anymodel_free(at);

        /* pure ssm dispatch works end-to-end */
        cce_ssm_model* sm = NULL;
        CHECK(cce_ssm_load(&sm, "hybrid_pure_ssm.gguf") == CCE_OK && sm, "pure-ssm model loads via cce_ssm_load");
        if (sm) { float lg[V_]; CHECK(cce_ssm_forward(sm, tokens, NT, lg, V_) == CCE_OK, "pure-ssm forward runs"); cce_ssm_free(sm); }

        /* incompatible loaders REFUSE structural mismatches (no silent drop) */
        cce_hybrid_model* hm = NULL;
        CHECK(cce_hybrid_load(&hm, "hybrid_pure_attn.gguf") != CCE_OK && hm == NULL,
              "hybrid loader refuses a pure-attention model (no ssm layer)");
        CHECK(cce_hybrid_load(&hm, "hybrid_pure_ssm.gguf") != CCE_OK && hm == NULL,
              "hybrid loader refuses a pure-ssm model (no attention layer)");

        cce_ssm_model* sbad = NULL;
        CHECK(cce_ssm_load(&sbad, "hybrid_test.gguf") != CCE_OK && sbad == NULL,
              "ssm loader refuses the hybrid (attention layers are not ssm scans)");
        cce_gguf_qwen2* qbad = NULL;
        CHECK(cce_gguf_load_model(&qbad, "hybrid_test.gguf") == CCE_ERR_UNSUPPORTED && qbad == NULL,
              "transformer loader refuses the hybrid instead of dropping the ssm layer");

        remove("hybrid_pure_attn.gguf");
        remove("hybrid_pure_ssm.gguf");
    }

    remove("hybrid_test.gguf");
    free(w);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
