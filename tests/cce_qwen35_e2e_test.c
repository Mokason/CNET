/* cce_qwen35_e2e: hermetic end-to-end test of the Qwen3.5 hybrid runner
 * (cce_gguf_qwen35.c) on a tiny synthetic GGUF the test writes itself —
 * no real model, no network, runs in seconds.
 *
 * The fixture is a real qwen35-arch GGUFv3: 4 trunk layers (3 Gated-DeltaNet
 * + 1 gated-attention at (i+1)%4==0) + 1 nextn/MTP block that must be
 * SKIPPED, fused per-head-interleaved [q|gate] attention projection, fused
 * qkv + z-gate + alpha/beta DeltaNet projections, causal depthwise conv
 * (kernel 4 > tokens-per-call: ring carry exercised), partial rotary
 * (rope_dim 8 of head_dim 16) with YaRN factor 2 (mscale != 1 exercised),
 * IMROPE sections [2,1,1,0], GQA 2:1 in attention and a 2:4 TILED K->V
 * broadcast in DeltaNet.
 *
 * The reference is an independent double-precision forward written straight
 * from the pinned llama.cpp/ggml semantics. Every 2D weight is snapped to a
 * qs/64 grid so the Q8_0 fixture (f16 d = 1/64 exactly) dequants to
 * BIT-IDENTICAL floats — the two containers must produce byte-equal logits.
 *
 * Invariants pinned here are exactly the oracle-harness contract:
 * reset+replay, batch==serial, prefix-rewind checkpoint (the flagship
 * fs_prefix protocol), forward_probes==serial with live state untouched,
 * arbitrary-rewind refusal, layer-tap coverage on both layer kinds,
 * CNET_ORACLE_INT8 head/nextn FP-skip, head-window bit-identity, and
 * honest refusals (wrong-arch loaders, missing ssm_a). */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_detect.h"

static int g_checks = 0, g_fails = 0;
#define CHECK(cond, msg) do { \
    g_checks++; \
    if (cond) { printf("  ok  %s\n", (msg)); } \
    else { g_fails++; printf("  FAIL %s\n", (msg)); } } while (0)

/* ---- geometry (small but nothing degenerate) ---- */
#define D_    32
#define V_    64
#define FF_   32
#define LT_   4          /* trunk layers: 0,1,2 deltanet; 3 attn */
#define LA_   5          /* blocks incl. the nextn/MTP block (blk.4) */
#define NH_   2          /* attn query heads */
#define NKV_  1          /* attn kv heads (GQA 2:1) */
#define HD_   16         /* attn head dim */
#define ROPED_ 8         /* rotated dims (partial rotary) */
#define HK_   2          /* deltanet key heads */
#define HV_   4          /* deltanet value heads (tiled 2->4) */
#define DK_   8
#define DV_   8
#define CONVK_ 4
#define KEYD_ (HK_ * DK_)          /* 16 */
#define QKVD_ (2 * KEYD_ + HV_ * DV_) /* 48 */
#define EPS_  1e-6f
#define BASE_ 10000.0f
#define YFACT_ 2.0f
#define YORIG_ 32
#define NT_   6

typedef struct {
    float emb[V_][D_], out_w[V_][D_], out_norm[D_];
    float attn_norm[LA_][D_], post_norm[LA_][D_];
    float ffn_gate[LA_][FF_][D_], ffn_up[LA_][FF_][D_], ffn_down[LA_][D_][FF_];
    /* attn layers (used for blk.3 and the nextn blk.4) */
    float attn_q[LA_][2 * NH_ * HD_][D_];   /* per-head interleaved [q|gate] */
    float attn_k[LA_][NKV_ * HD_][D_], attn_v[LA_][NKV_ * HD_][D_];
    float attn_o[LA_][D_][NH_ * HD_];
    float q_norm[LA_][HD_], k_norm[LA_][HD_];
    /* deltanet layers */
    float qkv[LA_][QKVD_][D_], zg[LA_][HV_ * DV_][D_];
    float alpha[LA_][HV_][D_], beta[LA_][HV_][D_];
    float ssm_out[LA_][D_][HV_ * DV_];
    float conv[LA_][QKVD_][CONVK_];         /* [channel][tap], tap 0 oldest */
    float ssm_a[LA_][HV_], dt_bias[LA_][HV_], ssm_norm[LA_][DV_];
    /* nextn side tensors (blk.4 only; must be ignored by the loader) */
    float eh_proj[D_][2 * D_], enorm[D_], hnorm[D_], sh_norm[D_];
} weights;

static unsigned g_rng = 0x5eed1234u;
static float frnd(float s) {
    g_rng = g_rng * 1664525u + 1013904223u;
    return s * (((float)(g_rng >> 8) / (float)(1u << 24)) - 0.5f);
}
/* snap to the Q8_0-exact grid qs/64, qs in [-127,127] */
static float snap(float x) {
    int q = (int)lrintf(x * 64.0f);
    if (q > 127) q = 127;
    if (q < -127) q = -127;
    return (float)q / 64.0f;
}

static void gen_weights(weights *w) {
    size_t i;
    float *p = (float *)w;
    for (i = 0; i < sizeof(*w) / sizeof(float); i++) p[i] = frnd(0.6f);
    /* norms near 1 (RMSNorm weights) */
    for (i = 0; i < D_; i++) w->out_norm[i] = 1.0f + frnd(0.2f);
    {
        int l, j;
        for (l = 0; l < LA_; l++) {
            for (j = 0; j < D_; j++) {
                w->attn_norm[l][j] = 1.0f + frnd(0.2f);
                w->post_norm[l][j] = 1.0f + frnd(0.2f);
            }
            for (j = 0; j < HD_; j++) {
                w->q_norm[l][j] = 1.0f + frnd(0.2f);
                w->k_norm[l][j] = 1.0f + frnd(0.2f);
            }
            for (j = 0; j < DV_; j++) w->ssm_norm[l][j] = 1.0f + frnd(0.2f);
            for (j = 0; j < HV_; j++) {
                w->ssm_a[l][j] = -(0.5f + 0.5f * fabsf(frnd(1.0f)));  /* < 0 */
                w->dt_bias[l][j] = frnd(0.4f);
            }
        }
        for (j = 0; j < D_; j++) {
            w->enorm[j] = 1.0f; w->hnorm[j] = 1.0f; w->sh_norm[j] = 1.0f;
        }
    }
    /* snap every 2D projection + embedding + head to the Q8_0-exact grid */
    {
        int l, a, b, v;
        for (v = 0; v < V_; v++)
            for (a = 0; a < D_; a++) {
                w->emb[v][a] = snap(w->emb[v][a]);
                w->out_w[v][a] = snap(w->out_w[v][a]);
            }
        for (l = 0; l < LA_; l++) {
            for (a = 0; a < 2 * NH_ * HD_; a++)
                for (b = 0; b < D_; b++) w->attn_q[l][a][b] = snap(w->attn_q[l][a][b]);
            for (a = 0; a < NKV_ * HD_; a++)
                for (b = 0; b < D_; b++) {
                    w->attn_k[l][a][b] = snap(w->attn_k[l][a][b]);
                    w->attn_v[l][a][b] = snap(w->attn_v[l][a][b]);
                }
            for (a = 0; a < D_; a++)
                for (b = 0; b < NH_ * HD_; b++) w->attn_o[l][a][b] = snap(w->attn_o[l][a][b]);
            for (a = 0; a < QKVD_; a++)
                for (b = 0; b < D_; b++) w->qkv[l][a][b] = snap(w->qkv[l][a][b]);
            for (a = 0; a < HV_ * DV_; a++)
                for (b = 0; b < D_; b++) w->zg[l][a][b] = snap(w->zg[l][a][b]);
            for (a = 0; a < HV_; a++)
                for (b = 0; b < D_; b++) {
                    w->alpha[l][a][b] = snap(w->alpha[l][a][b]);
                    w->beta[l][a][b] = snap(w->beta[l][a][b]);
                }
            for (a = 0; a < D_; a++)
                for (b = 0; b < HV_ * DV_; b++) w->ssm_out[l][a][b] = snap(w->ssm_out[l][a][b]);
            for (a = 0; a < FF_; a++)
                for (b = 0; b < D_; b++) {
                    w->ffn_gate[l][a][b] = snap(w->ffn_gate[l][a][b]);
                    w->ffn_up[l][a][b] = snap(w->ffn_up[l][a][b]);
                }
            for (a = 0; a < D_; a++)
                for (b = 0; b < FF_; b++) w->ffn_down[l][a][b] = snap(w->ffn_down[l][a][b]);
        }
        for (a = 0; a < D_; a++)
            for (b = 0; b < 2 * D_; b++) w->eh_proj[a][b] = snap(w->eh_proj[a][b]);
    }
}

/* ---- double-precision reference (independent of the runner) ---- */

static double ref_silu(double x) { return x / (1.0 + exp(-x)); }
static double ref_sig(double x)  { return 1.0 / (1.0 + exp(-x)); }
static double ref_softplus(double x) { return log1p(exp(x)); }

static void ref_rms(const double *x, const float *w, int n, double *o) {
    double ss = 0;
    int i;
    for (i = 0; i < n; i++) ss += x[i] * x[i];
    ss = 1.0 / sqrt(ss / n + (double)EPS_);
    for (i = 0; i < n; i++) o[i] = x[i] * ss * (double)w[i];
}

/* YaRN NEOX rope, ggml semantics (iterative theta chain; ramp in pair units) */
static void ref_rope(double *h, int pos) {
    const int half = ROPED_ / 2;
    const double fs = 1.0 / (double)YFACT_;
    const double corr_lo_raw =
        floor((double)ROPED_ * log((double)YORIG_ / (32.0 * 2.0 * M_PI)) /
              (2.0 * log((double)BASE_)));
    const double corr_hi_raw =
        ceil((double)ROPED_ * log((double)YORIG_ / (1.0 * 2.0 * M_PI)) /
             (2.0 * log((double)BASE_)));
    const double lo = corr_lo_raw > 0 ? corr_lo_raw : 0;
    const double hi = corr_hi_raw < (double)(ROPED_ - 1) ? corr_hi_raw
                                                         : (double)(ROPED_ - 1);
    const double theta_scale = pow((double)BASE_, -2.0 / (double)ROPED_);
    double theta = (double)pos;
    int i;
    for (i = 0; i < half; i++) {
        double extrap = theta, interp = fs * extrap, th = interp;
        double mscale = 1.0;                        /* post-cancel attn factor */
        double y = ((double)i - lo) / (hi - lo > 0.001 ? hi - lo : 0.001);
        double ramp = 1.0 - (y < 0 ? 0 : (y > 1 ? 1 : y));
        th = interp * (1.0 - ramp) + extrap * ramp;
        mscale *= 1.0 + 0.1 * log(1.0 / fs);
        {
            double c = cos(th) * mscale, s = sin(th) * mscale;
            double x0 = h[i], x1 = h[i + half];
            h[i]        = x0 * c - x1 * s;
            h[i + half] = x0 * s + x1 * c;
        }
        theta *= theta_scale;
    }
}

/* full reference forward over NT_ tokens; logits[t][V_] at every position */
static void ref_forward(const weights *w, const int *toks, int nt,
                        double logits[][V_]) {
    static double Kc[NT_][NKV_ * HD_], Vc[NT_][NKV_ * HD_];
    static double S[LT_][HV_][DK_][DV_];
    static double ring[LT_][CONVK_ - 1][QKVD_];
    double x[D_];
    int t, l, i, j, h;
    memset(S, 0, sizeof S);
    memset(ring, 0, sizeof ring);
    for (t = 0; t < nt; t++) {
        for (i = 0; i < D_; i++) x[i] = (double)w->emb[toks[t]][i];
        for (l = 0; l < LT_; l++) {
            double ln1[D_], after[D_];
            ref_rms(x, w->attn_norm[l], D_, ln1);
            if (l == 3) {                       /* gated attention layer */
                double qg[2 * NH_ * HD_], k[NKV_ * HD_], v[NKV_ * HD_];
                double ao[NH_ * HD_];
                for (j = 0; j < 2 * NH_ * HD_; j++) {
                    double a = 0;
                    for (i = 0; i < D_; i++) a += (double)w->attn_q[l][j][i] * ln1[i];
                    qg[j] = a;
                }
                for (j = 0; j < NKV_ * HD_; j++) {
                    double a = 0, b = 0;
                    for (i = 0; i < D_; i++) {
                        a += (double)w->attn_k[l][j][i] * ln1[i];
                        b += (double)w->attn_v[l][j][i] * ln1[i];
                    }
                    k[j] = a; v[j] = b;
                }
                for (h = 0; h < NH_; h++) {     /* q norm + rope on Q halves */
                    double *qh = qg + h * 2 * HD_;
                    double ss = 0;
                    for (i = 0; i < HD_; i++) ss += qh[i] * qh[i];
                    ss = 1.0 / sqrt(ss / HD_ + (double)EPS_);
                    for (i = 0; i < HD_; i++) qh[i] = qh[i] * ss * (double)w->q_norm[l][i];
                    ref_rope(qh, t);
                }
                for (h = 0; h < NKV_; h++) {
                    double *kh = k + h * HD_;
                    double ss = 0;
                    for (i = 0; i < HD_; i++) ss += kh[i] * kh[i];
                    ss = 1.0 / sqrt(ss / HD_ + (double)EPS_);
                    for (i = 0; i < HD_; i++) kh[i] = kh[i] * ss * (double)w->k_norm[l][i];
                    ref_rope(kh, t);
                }
                memcpy(Kc[t], k, sizeof k);
                memcpy(Vc[t], v, sizeof v);
                for (h = 0; h < NH_; h++) {
                    const double *qh = qg + h * 2 * HD_;
                    const double *gh = qg + h * 2 * HD_ + HD_;
                    int kh_i = h / (NH_ / NKV_);
                    double sc[NT_], mx = -1e30, sm = 0;
                    for (j = 0; j <= t; j++) {
                        double a = 0;
                        for (i = 0; i < HD_; i++)
                            a += qh[i] * Kc[j][kh_i * HD_ + i];
                        sc[j] = a / sqrt((double)HD_);
                        if (sc[j] > mx) mx = sc[j];
                    }
                    for (j = 0; j <= t; j++) { sc[j] = exp(sc[j] - mx); sm += sc[j]; }
                    for (i = 0; i < HD_; i++) {
                        double a = 0;
                        for (j = 0; j <= t; j++)
                            a += (sc[j] / sm) * Vc[j][kh_i * HD_ + i];
                        ao[h * HD_ + i] = a * ref_sig(gh[i]);
                    }
                }
                for (i = 0; i < D_; i++) {
                    double a = 0;
                    for (j = 0; j < NH_ * HD_; j++)
                        a += (double)w->attn_o[l][i][j] * ao[j];
                    after[i] = a;
                }
            } else {                            /* Gated-DeltaNet layer */
                double qkv[QKVD_], z[HV_ * DV_], al[HV_], be[HV_];
                double conv[QKVD_], gn[HV_ * DV_];
                for (j = 0; j < QKVD_; j++) {
                    double a = 0;
                    for (i = 0; i < D_; i++) a += (double)w->qkv[l][j][i] * ln1[i];
                    qkv[j] = a;
                }
                for (j = 0; j < HV_ * DV_; j++) {
                    double a = 0;
                    for (i = 0; i < D_; i++) a += (double)w->zg[l][j][i] * ln1[i];
                    z[j] = a;
                }
                for (j = 0; j < HV_; j++) {
                    double a = 0, b = 0;
                    for (i = 0; i < D_; i++) {
                        a += (double)w->alpha[l][j][i] * ln1[i];
                        b += (double)w->beta[l][j][i] * ln1[i];
                    }
                    al[j] = a; be[j] = b;
                }
                for (j = 0; j < QKVD_; j++) {   /* conv: tap 0 = oldest */
                    double a = 0;
                    for (i = 0; i < CONVK_ - 1; i++)
                        a += (double)w->conv[l][j][i] * ring[l][i][j];
                    a += (double)w->conv[l][j][CONVK_ - 1] * qkv[j];
                    conv[j] = ref_silu(a);
                }
                for (i = 0; i < CONVK_ - 2; i++)
                    memcpy(ring[l][i], ring[l][i + 1], sizeof ring[l][i]);
                memcpy(ring[l][CONVK_ - 2], qkv, sizeof qkv);
                for (h = 0; h < HV_; h++) {     /* TILED broadcast h % HK_ */
                    double qe[DK_], ke[DK_];
                    double sq = 0, sk = 0, g, decay, bs;
                    const double *vh = conv + 2 * KEYD_ + h * DV_;
                    for (i = 0; i < DK_; i++) {
                        qe[i] = conv[(h % HK_) * DK_ + i];
                        ke[i] = conv[KEYD_ + (h % HK_) * DK_ + i];
                        sq += qe[i] * qe[i];
                        sk += ke[i] * ke[i];
                    }
                    sq = 1.0 / sqrt(sq + (double)EPS_);   /* core l2 semantics */
                    sk = 1.0 / sqrt(sk + (double)EPS_);
                    for (i = 0; i < DK_; i++) {
                        qe[i] = qe[i] * sq / sqrt((double)DK_);
                        ke[i] *= sk;
                    }
                    g = (double)w->ssm_a[l][h] *
                        ref_softplus(al[h] + (double)w->dt_bias[l][h]);
                    decay = exp(g);
                    bs = ref_sig(be[h]);
                    {
                        double kvm[DV_], delta[DV_];
                        int a2, b2;
                        for (a2 = 0; a2 < DK_; a2++)
                            for (b2 = 0; b2 < DV_; b2++) S[l][h][a2][b2] *= decay;
                        for (b2 = 0; b2 < DV_; b2++) {
                            kvm[b2] = 0;
                            for (a2 = 0; a2 < DK_; a2++)
                                kvm[b2] += S[l][h][a2][b2] * ke[a2];
                        }
                        for (b2 = 0; b2 < DV_; b2++)
                            delta[b2] = (vh[b2] - kvm[b2]) * bs;
                        for (a2 = 0; a2 < DK_; a2++)
                            for (b2 = 0; b2 < DV_; b2++)
                                S[l][h][a2][b2] += ke[a2] * delta[b2];
                        for (b2 = 0; b2 < DV_; b2++) {
                            double a3 = 0;
                            for (a2 = 0; a2 < DK_; a2++)
                                a3 += S[l][h][a2][b2] * qe[a2];
                            /* gated norm happens outside the per-head loop
                               readout in the runner; keep the value here */
                            gn[h * DV_ + b2] = a3;
                        }
                    }
                }
                for (h = 0; h < HV_; h++) {     /* RMSNorm(out)*SiLU(z) */
                    double ss = 0;
                    for (i = 0; i < DV_; i++)
                        ss += gn[h * DV_ + i] * gn[h * DV_ + i];
                    ss = 1.0 / sqrt(ss / DV_ + (double)EPS_);
                    for (i = 0; i < DV_; i++)
                        gn[h * DV_ + i] = gn[h * DV_ + i] * ss *
                                          (double)w->ssm_norm[l][i] *
                                          ref_silu(z[h * DV_ + i]);
                }
                for (i = 0; i < D_; i++) {
                    double a = 0;
                    for (j = 0; j < HV_ * DV_; j++)
                        a += (double)w->ssm_out[l][i][j] * gn[j];
                    after[i] = a;
                }
            }
            for (i = 0; i < D_; i++) after[i] += x[i];      /* residual */
            {
                double ln2[D_], mid[FF_];
                ref_rms(after, w->post_norm[l], D_, ln2);
                for (j = 0; j < FF_; j++) {
                    double a = 0, b = 0;
                    for (i = 0; i < D_; i++) {
                        a += (double)w->ffn_gate[l][j][i] * ln2[i];
                        b += (double)w->ffn_up[l][j][i] * ln2[i];
                    }
                    mid[j] = ref_silu(a) * b;
                }
                for (i = 0; i < D_; i++) {
                    double a = 0;
                    for (j = 0; j < FF_; j++)
                        a += (double)w->ffn_down[l][i][j] * mid[j];
                    x[i] = after[i] + a;
                }
            }
        }
        {
            double fn[D_];
            int v;
            ref_rms(x, w->out_norm, D_, fn);
            for (v = 0; v < V_; v++) {
                double a = 0;
                for (i = 0; i < D_; i++) a += (double)w->out_w[v][i] * fn[i];
                logits[t][v] = a;
            }
        }
    }
}

/* ---- GGUF fixture writer ---- */

typedef struct {
    char name[128];
    int shape[4];       /* torch order [out, in] / 1D */
    int ndim;
    const float *data;
    size_t numel;
    int q8;             /* 1 = write as Q8_0 (numel % 32 == 0, grid-snapped) */
} entry;

static void gg_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void gg_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void gg_f32(FILE *f, float v) { fwrite(&v, 4, 1, f); }
static void gg_str(FILE *f, const char *s) { gg_u64(f, strlen(s)); fwrite(s, 1, strlen(s), f); }
static void kv_u32(FILE *f, const char *k, uint32_t v) { gg_str(f, k); gg_u32(f, 4); gg_u32(f, v); }
static void kv_f32(FILE *f, const char *k, float v) { gg_str(f, k); gg_u32(f, 6); gg_f32(f, v); }
static void kv_str(FILE *f, const char *k, const char *v) { gg_str(f, k); gg_u32(f, 8); gg_str(f, v); }
static void kv_arr_i32(FILE *f, const char *k, const int *v, int n) {
    int i;
    gg_str(f, k); gg_u32(f, 9); gg_u32(f, 5); gg_u64(f, (uint64_t)n);
    for (i = 0; i < n; i++) { int32_t x = v[i]; fwrite(&x, 4, 1, f); }
}

static size_t entry_bytes(const entry *e) {
    return e->q8 ? (e->numel / 32) * 34 : e->numel * 4;
}

static void write_fixture(const char *path, const entry *ents, int n_ents) {
    FILE *f = fopen(path, "wb");
    int sections[4] = { 2, 1, 1, 0 };
    int i;
    uint64_t off = 0;
    fwrite("GGUF", 1, 4, f);
    gg_u32(f, 3);
    gg_u64(f, (uint64_t)n_ents);
    gg_u64(f, 23);                                /* exact KV count below */
    kv_str(f, "general.architecture", "qwen35");
    kv_u32(f, "qwen35.block_count", LA_);
    kv_u32(f, "qwen35.embedding_length", D_);
    kv_u32(f, "qwen35.feed_forward_length", FF_);
    kv_u32(f, "qwen35.context_length", 64);
    kv_u32(f, "qwen35.attention.head_count", NH_);
    kv_u32(f, "qwen35.attention.head_count_kv", NKV_);
    kv_u32(f, "qwen35.attention.key_length", HD_);
    kv_u32(f, "qwen35.attention.value_length", HD_);
    kv_f32(f, "qwen35.attention.layer_norm_rms_epsilon", EPS_);
    kv_u32(f, "qwen35.full_attention_interval", 4);
    kv_u32(f, "qwen35.nextn_predict_layers", 1);
    kv_f32(f, "qwen35.rope.freq_base", BASE_);
    kv_u32(f, "qwen35.rope.dimension_count", ROPED_);
    kv_arr_i32(f, "qwen35.rope.dimension_sections", sections, 4);
    kv_u32(f, "qwen35.ssm.conv_kernel", CONVK_);
    kv_u32(f, "qwen35.ssm.state_size", DK_);
    kv_u32(f, "qwen35.ssm.group_count", HK_);
    kv_u32(f, "qwen35.ssm.time_step_rank", HV_);
    kv_u32(f, "qwen35.ssm.inner_size", HV_ * DV_);
    kv_str(f, "qwen35.rope.scaling.type", "yarn");
    kv_f32(f, "qwen35.rope.scaling.factor", YFACT_);
    kv_u32(f, "qwen35.rope.scaling.original_context_length", YORIG_);

    /* tensor table (ggml ne order: reversed torch dims) */
    for (i = 0; i < n_ents; i++) {
        int d;
        gg_str(f, ents[i].name);
        gg_u32(f, (uint32_t)ents[i].ndim);
        for (d = ents[i].ndim - 1; d >= 0; d--)
            gg_u64(f, (uint64_t)ents[i].shape[d]);
        gg_u32(f, ents[i].q8 ? 8u : 0u);
        gg_u64(f, off);
        off += entry_bytes(&ents[i]);
        off = (off + 31) / 32 * 32;             /* per-tensor align-up */
    }
    /* data section starts at the next 32-boundary after the table */
    {
        long here = ftell(f);
        long pad = (long)((32 - (here % 32)) % 32);
        long p;
        for (p = 0; p < pad; p++) fputc(0, f);
    }
    for (i = 0; i < n_ents; i++) {
        size_t nb = entry_bytes(&ents[i]);
        size_t pad = ((nb + 31) / 32 * 32) - nb, p;
        if (!ents[i].q8) {
            fwrite(ents[i].data, 4, ents[i].numel, f);
        } else {
            /* Q8_0 blocks: f16 d = 1/64 (0x2400, exact), qs = w*64 (exact by
               construction — every eligible weight was grid-snapped) */
            size_t b, nb2 = ents[i].numel / 32;
            for (b = 0; b < nb2; b++) {
                uint16_t d16 = 0x2400;
                int8_t qs[32];
                int k2;
                fwrite(&d16, 2, 1, f);
                for (k2 = 0; k2 < 32; k2++) {
                    float x = ents[i].data[b * 32 + k2] * 64.0f;
                    qs[k2] = (int8_t)lrintf(x);
                }
                fwrite(qs, 1, 32, f);
            }
        }
        for (p = 0; p < pad; p++) fputc(0, f);
    }
    fclose(f);
}

static int build_entries(const weights *w, entry *e, int q8, int with_nextn) {
    int n = 0, l;
    char nm[128];
#define ADD2(NM, D0, D1, PTR, Q) do { \
        snprintf(e[n].name, sizeof e[n].name, "%s", NM); \
        e[n].shape[0] = (D0); e[n].shape[1] = (D1); e[n].ndim = 2; \
        e[n].data = (PTR); e[n].numel = (size_t)(D0) * (D1); e[n].q8 = (Q); n++; \
    } while (0)
#define ADD1(NM, D0, PTR) do { \
        snprintf(e[n].name, sizeof e[n].name, "%s", NM); \
        e[n].shape[0] = (D0); e[n].ndim = 1; \
        e[n].data = (PTR); e[n].numel = (size_t)(D0); e[n].q8 = 0; n++; \
    } while (0)
    ADD2("token_embd.weight", V_, D_, &w->emb[0][0], q8);
    ADD2("output.weight", V_, D_, &w->out_w[0][0], q8);
    ADD1("output_norm.weight", D_, w->out_norm);
    for (l = 0; l < (with_nextn ? LA_ : LT_); l++) {
        int is_attn = (l == 3 || l == 4);
        snprintf(nm, sizeof nm, "blk.%d.attn_norm.weight", l);
        ADD1(nm, D_, w->attn_norm[l]);
        snprintf(nm, sizeof nm, "blk.%d.post_attention_norm.weight", l);
        ADD1(nm, D_, w->post_norm[l]);
        if (is_attn) {
            snprintf(nm, sizeof nm, "blk.%d.attn_q.weight", l);
            ADD2(nm, 2 * NH_ * HD_, D_, &w->attn_q[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.attn_k.weight", l);
            ADD2(nm, NKV_ * HD_, D_, &w->attn_k[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.attn_v.weight", l);
            ADD2(nm, NKV_ * HD_, D_, &w->attn_v[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.attn_output.weight", l);
            ADD2(nm, D_, NH_ * HD_, &w->attn_o[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.attn_q_norm.weight", l);
            ADD1(nm, HD_, w->q_norm[l]);
            snprintf(nm, sizeof nm, "blk.%d.attn_k_norm.weight", l);
            ADD1(nm, HD_, w->k_norm[l]);
        } else {
            snprintf(nm, sizeof nm, "blk.%d.attn_qkv.weight", l);
            ADD2(nm, QKVD_, D_, &w->qkv[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.attn_gate.weight", l);
            ADD2(nm, HV_ * DV_, D_, &w->zg[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.ssm_alpha.weight", l);
            ADD2(nm, HV_, D_, &w->alpha[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.ssm_beta.weight", l);
            ADD2(nm, HV_, D_, &w->beta[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.ssm_out.weight", l);
            ADD2(nm, D_, HV_ * DV_, &w->ssm_out[l][0][0], q8);
            snprintf(nm, sizeof nm, "blk.%d.ssm_conv1d.weight", l);
            ADD2(nm, QKVD_, CONVK_, &w->conv[l][0][0], 0);
            snprintf(nm, sizeof nm, "blk.%d.ssm_a", l);
            ADD1(nm, HV_, w->ssm_a[l]);
            snprintf(nm, sizeof nm, "blk.%d.ssm_dt.bias", l);
            ADD1(nm, HV_, w->dt_bias[l]);
            snprintf(nm, sizeof nm, "blk.%d.ssm_norm.weight", l);
            ADD1(nm, DV_, w->ssm_norm[l]);
        }
        snprintf(nm, sizeof nm, "blk.%d.ffn_gate.weight", l);
        ADD2(nm, FF_, D_, &w->ffn_gate[l][0][0], q8);
        snprintf(nm, sizeof nm, "blk.%d.ffn_up.weight", l);
        ADD2(nm, FF_, D_, &w->ffn_up[l][0][0], q8);
        snprintf(nm, sizeof nm, "blk.%d.ffn_down.weight", l);
        ADD2(nm, D_, FF_, &w->ffn_down[l][0][0], q8);
        if (l == 4 && with_nextn) {
            ADD2("blk.4.nextn.eh_proj.weight", D_, 2 * D_, &w->eh_proj[0][0], q8);
            ADD1("blk.4.nextn.enorm.weight", D_, w->enorm);
            ADD1("blk.4.nextn.hnorm.weight", D_, w->hnorm);
            ADD1("blk.4.nextn.shared_head_norm.weight", D_, w->sh_norm);
        }
    }
#undef ADD2
#undef ADD1
    return n;
}

/* ---- tap instrumentation ---- */
static int g_tap_count = 0, g_tap_dim_ok = 1, g_tap_last_layer = -1;
static void count_tap(int layer, const float *x, int n_tokens, int dim, void *u) {
    (void)x; (void)n_tokens; (void)u;
    g_tap_count++;
    g_tap_last_layer = layer;
    if (dim != D_) g_tap_dim_ok = 0;
}

int main(void) {
    static const int toks[NT_] = { 3, 17, 5, 42, 1, 33 };
    static double ref[NT_][V_];
    weights *w = (weights *)malloc(sizeof *w);
    float *lg = (float *)malloc((size_t)V_ * sizeof(float));
    float *lg2 = (float *)malloc((size_t)V_ * sizeof(float));
    float *lgp = (float *)malloc((size_t)2 * V_ * sizeof(float));
    cce_gguf_qwen2 *m = NULL;
    int i;

    printf("=== cce_qwen35_e2e: hybrid runner, hermetic fixture ===\n");
    gen_weights(w);
    ref_forward(w, toks, NT_, ref);

    /* f16 grid premise: d = 1/64 must round-trip exactly */
    CHECK(cce_gguf_f16_to_f32(0x2400) == 0.015625f, "f16 1/64 exact");

    {
        entry ents[96];
        int n = build_entries(w, ents, 0, 1);
        write_fixture("qwen35_e2e.gguf", ents, n);
        n = build_entries(w, ents, 1, 1);
        write_fixture("qwen35_e2e.q8.gguf", ents, n);
        n = build_entries(w, ents, 0, 0);       /* no nextn block at all */
        write_fixture("qwen35_e2e.nonextn.gguf", ents, n);
    }

    /* detect classification */
    {
        cce_model_info info;
        CHECK(cce_detect_file("qwen35_e2e.gguf", &info) == CCE_OK, "detect ok");
        CHECK(info.family == CCE_ARCH_FAMILY_HYBRID, "family hybrid");
        CHECK(strcmp(info.naming, "qwen35-blk") == 0, "naming qwen35-blk");
        CHECK(info.runnable == 1, "runnable");
        CHECK(strcmp(info.runner, "cce_gguf_load_qwen35") == 0,
              "runner cce_gguf_load_qwen35");
    }

    /* wrong-runner refusals */
    {
        cce_ssm_model *sm = NULL;
        cce_hybrid_model *hm = NULL;
        CHECK(cce_ssm_load(&sm, "qwen35_e2e.gguf") != CCE_OK,
              "cce_ssm_load refuses qwen35");
        CHECK(cce_hybrid_load(&hm, "qwen35_e2e.gguf") != CCE_OK,
              "cce_hybrid_load refuses qwen35");
    }

    /* FP load + reference parity at every position */
    CHECK(cce_gguf_load_qwen35(&m, "qwen35_e2e.gguf") == CCE_OK, "fp load");
    if (!m) { printf("FATAL: no model\n"); return 1; }
    CHECK(m->n_layer == LT_, "trunk layers = 4 (nextn skipped)");
    CHECK(m->bos_token_id == -1, "no bos synthesized");
    {
        int worst_t = -1;
        double worst = 0;
        int ok = 1;
        m->cur_pos = 0;
        for (i = 0; i < NT_; i++) {
            int v;
            if (cce_gguf_qwen2_forward(m, &toks[i], 1, lg, V_) != CCE_OK) {
                ok = 0;
                break;
            }
            for (v = 0; v < V_; v++) {
                double d = fabs((double)lg[v] - ref[i][v]);
                double tol = 1e-4 + 5e-4 * fabs(ref[i][v]);
                if (d > worst) { worst = d; worst_t = i; }
                if (d > tol) ok = 0;
            }
        }
        printf("  (worst |diff| %.3g at t=%d)\n", worst, worst_t);
        CHECK(ok, "fp logits match double reference (atol 1e-4 rtol 5e-4)");
    }

    /* reset + replay bit-identical */
    {
        float first[V_];
        m->cur_pos = 0;
        for (i = 0; i < NT_; i++) cce_gguf_qwen2_forward(m, &toks[i], 1, lg, V_);
        memcpy(first, lg, sizeof first);
        m->cur_pos = 0;
        for (i = 0; i < NT_; i++) cce_gguf_qwen2_forward(m, &toks[i], 1, lg, V_);
        CHECK(memcmp(first, lg, sizeof first) == 0, "reset+replay bit-identical");
        /* batch == serial */
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, toks, NT_, lg2, V_);
        CHECK(memcmp(first, lg2, (size_t)V_ * 4) == 0, "batch == serial bit-identical");
    }

    /* prefix rewind protocol (the flagship fs_prefix contract) */
    {
        float fresh[V_], sufA[V_], sufA2[V_];
        int two[2] = { toks[0], 9 };
        int a = 9, b = 21;
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, two, 2, fresh, V_);       /* fresh [t0,a] */
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, &toks[0], 1, lg, V_);     /* prefix [t0] */
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &a, 1, sufA, V_);         /* suffix a */
        m->cur_pos = 1;                                     /* rewind */
        cce_gguf_qwen2_forward(m, &b, 1, lg, V_);           /* suffix b */
        m->cur_pos = 1;                                     /* rewind again */
        cce_gguf_qwen2_forward(m, &a, 1, sufA2, V_);        /* suffix a again */
        CHECK(memcmp(sufA, sufA2, sizeof sufA) == 0,
              "rewind suffix replay bit-identical");
        CHECK(memcmp(sufA, fresh, sizeof sufA) == 0,
              "prefix+suffix == fresh-context bit-identical");
        /* arbitrary rewind target refuses */
        m->cur_pos = 5;
        CHECK(cce_gguf_qwen2_forward(m, &a, 1, lg, V_) != CCE_OK,
              "arbitrary rewind refused");
    }

    /* UNIT-BOUNDARY rewind (the flagship fs_prefix pattern across units):
       prefix t0 + suffixes with rewinds, then a RESET + NEW prefix t1 at
       the SAME position, then suffixes with rewinds again. The stale-
       checkpoint bug restored t0's recurrent state into t1's suffixes
       (chimera oracle, mined 2026-07-15's poisoned base) — the reset must
       invalidate the checkpoint so t1's first suffix re-snapshots. */
    {
        float freshB[V_], sufX[V_], sufX2[V_];
        int t0 = toks[0], t1 = toks[3], xx = 7, yy = 30;
        int twoB[2];
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, &t0, 1, lg, V_);      /* unit A prefix */
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &xx, 1, lg, V_);      /* A suffix (ckpt@1) */
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &yy, 1, lg, V_);      /* A suffix (restore) */
        twoB[0] = t1; twoB[1] = xx;
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, twoB, 2, freshB, V_); /* truth for unit B */
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, &t1, 1, lg, V_);      /* unit B prefix */
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &xx, 1, sufX, V_);    /* B suffix 1 */
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &yy, 1, lg, V_);      /* B suffix 2 (restore) */
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &xx, 1, sufX2, V_);   /* B suffix 3 (restore) */
        CHECK(memcmp(sufX, freshB, sizeof sufX) == 0,
              "unit-boundary: first suffix of the NEW unit == fresh context");
        CHECK(memcmp(sufX2, freshB, sizeof sufX2) == 0,
              "unit-boundary: RESTORED suffix of the new unit == fresh "
              "context (stale-checkpoint regression)");
    }

    /* forward_probes == serial, live state untouched */
    {
        float serialA[V_], serialB[V_], after_c[V_], after_c2[V_];
        int probes[2] = { 9, 21 };
        int c = 2;
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, &toks[0], 1, lg, V_);
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &probes[0], 1, serialA, V_);
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &probes[1], 1, serialB, V_);
        m->cur_pos = 1;
        cce_gguf_qwen2_forward(m, &c, 1, after_c, V_);      /* control run */
        /* replay with a probe batch interleaved */
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, &toks[0], 1, lg, V_);
        CHECK(cce_gguf_qwen2_forward_probes(m, probes, 2, lgp, V_) == CCE_OK,
              "probe batch runs");
        CHECK(memcmp(lgp, serialA, (size_t)V_ * 4) == 0 &&
              memcmp(lgp + V_, serialB, (size_t)V_ * 4) == 0,
              "probe rows == serial rewind rows bit-identical");
        cce_gguf_qwen2_forward(m, &c, 1, after_c2, V_);
        CHECK(memcmp(after_c, after_c2, sizeof after_c) == 0,
              "probe batch left live state untouched");
    }

    /* layer tap coverage (both kinds) */
    {
        float notap[V_];
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, toks, NT_, notap, V_);
        g_tap_count = 0; g_tap_dim_ok = 1; g_tap_last_layer = -1;
        cce_gguf_set_layer_tap(count_tap, NULL);
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, toks, NT_, lg, V_);
        cce_gguf_set_layer_tap(NULL, NULL);
        CHECK(g_tap_count == LT_ && g_tap_last_layer == LT_ - 1 && g_tap_dim_ok,
              "tap fires once per trunk layer with dim D");
        CHECK(memcmp(notap, lg, sizeof notap) == 0, "tap on/off bit-identical");
    }

    /* head window bit-identity */
    {
        static const int win[5] = { 3, 12, 42, 55, 63 };
        float full[V_];
        int k2, ok = 1;
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, toks, NT_, full, V_);
        cce_gguf_qwen2_set_head_window(m, win, 5);
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, toks, NT_, lg, V_);
        cce_gguf_qwen2_set_head_window(m, NULL, 0);
        for (k2 = 0; k2 < 5; k2++) {
            union { float f; uint32_t u; } x, y;
            x.f = full[win[k2]]; y.f = lg[win[k2]];
            if (x.u != y.u) ok = 0;
        }
        CHECK(ok, "head window bit-identical on window ids");
    }

    /* Q8_0 container == FP container, bit-identical (grid-snapped weights) */
    {
        cce_gguf_qwen2 *mq = NULL;
        float fp_l[V_];
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, toks, NT_, fp_l, V_);
        CHECK(cce_gguf_load_qwen35(&mq, "qwen35_e2e.q8.gguf") == CCE_OK, "q8 load");
        if (mq) {
            mq->cur_pos = 0;
            cce_gguf_qwen2_forward(mq, toks, NT_, lg, V_);
            CHECK(memcmp(fp_l, lg, sizeof fp_l) == 0,
                  "q8_0 dequant forward bit-identical to fp32");
            cce_gguf_qwen2_free(mq);
        }
    }

    /* MTP skip: fixture without the nextn block is the same model */
    {
        cce_gguf_qwen2 *mn = NULL;
        float with_l[V_];
        m->cur_pos = 0;
        cce_gguf_qwen2_forward(m, toks, NT_, with_l, V_);
        CHECK(cce_gguf_load_qwen35(&mn, "qwen35_e2e.nonextn.gguf") == CCE_OK,
              "no-nextn load");
        if (mn) {
            CHECK(mn->n_layer == LT_, "no-nextn trunk layers = 4");
            mn->cur_pos = 0;
            cce_gguf_qwen2_forward(mn, toks, NT_, lg, V_);
            CHECK(memcmp(with_l, lg, sizeof with_l) == 0,
                  "nextn block skip is exact (bit-identical logits)");
            cce_gguf_qwen2_free(mn);
        }
    }

    /* CNET_ORACLE_INT8: layers quantize, head + everything named lm_head
       stays FP; forward still runs and produces finite logits */
    {
        cce_gguf_qwen2 *mi = NULL;
        setenv("CNET_ORACLE_INT8", "1", 1);
        CHECK(cce_gguf_load_qwen35(&mi, "qwen35_e2e.gguf") == CCE_OK, "int8 load");
        unsetenv("CNET_ORACLE_INT8");
        if (mi) {
            cce_cascade *head = cce_forest_get_resident(mi->forest, "qwen2.lm_head");
            cce_cascade *lay = cce_forest_get_resident(mi->forest, "qwen35.blk.0.qkv");
            int v, finite = 1;
            CHECK(head && head->num_blocks == 1 && !head->blocks[0].w_q &&
                  head->blocks[0].weights.data, "int8 mode: head stays FP");
            CHECK(lay && lay->num_blocks == 1 && lay->blocks[0].w_q != NULL,
                  "int8 mode: layer specialists quantized");
            mi->cur_pos = 0;
            CHECK(cce_gguf_qwen2_forward(mi, toks, NT_, lg, V_) == CCE_OK,
                  "int8 forward runs");
            for (v = 0; v < V_; v++) if (!isfinite(lg[v])) finite = 0;
            CHECK(finite, "int8 logits finite");
            cce_gguf_qwen2_free(mi);
        }
    }

    /* honest refusal: a fixture missing ssm_a must not load */
    {
        entry ents[96];
        int n = build_entries(w, ents, 0, 1);
        int k2, n2 = 0;
        entry pruned[96];
        cce_gguf_qwen2 *mr = NULL;
        for (k2 = 0; k2 < n; k2++)
            if (!strstr(ents[k2].name, "ssm_a")) pruned[n2++] = ents[k2];
        write_fixture("qwen35_e2e.noa.gguf", pruned, n2);
        CHECK(cce_gguf_load_qwen35(&mr, "qwen35_e2e.noa.gguf") != CCE_OK,
              "missing ssm_a refused");
    }

    /* honest refusal: sparse KV (CNET_SPARSE_KV / set_sparse_kv) exists only
       on the classic transformer attention path — the hybrid runner must
       refuse an armed knob rather than silently run full attention */
    {
        cce_gguf_qwen2 *ms = NULL;
        setenv("CNET_SPARSE_KV", "0.25", 1);
        CHECK(cce_gguf_load_qwen35(&ms, "qwen35_e2e.gguf") != CCE_OK,
              "armed CNET_SPARSE_KV refused by the qwen35 loader");
        setenv("CNET_SPARSE_KV", "0", 1);
        ms = NULL;
        CHECK(cce_gguf_load_qwen35(&ms, "qwen35_e2e.gguf") == CCE_OK &&
              ms != NULL, "CNET_SPARSE_KV=0 still loads");
        unsetenv("CNET_SPARSE_KV");
        if (ms) {
            CHECK(cce_gguf_qwen2_set_sparse_kv(ms, 0.5f) ==
                  CCE_ERR_UNSUPPORTED,
                  "sparse-KV setter refuses the qwen35 hybrid");
            cce_gguf_qwen2_free(ms);
        }
    }

    cce_gguf_qwen2_free(m);
    remove("qwen35_e2e.gguf");
    remove("qwen35_e2e.q8.gguf");
    remove("qwen35_e2e.nonextn.gguf");
    remove("qwen35_e2e.noa.gguf");

    printf("=== %d checks, %d failures ===\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
