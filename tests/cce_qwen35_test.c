/* cce_qwen35: native Qwen3.5 (attention + Gated-DeltaNet) execution core.
 *
 * Verification strategy (non-circular, hermetic):
 *  1. Deterministic inputs/params as plain float arrays (no model files).
 *  2. INDEPENDENT double-precision references, written directly from the
 *     recurrence/attention definitions (plain loops, no cce_qwen35 code):
 *       - ref_deltanet_step: exactly torch_recurrent_gated_delta_rule
 *         (l2norm q/k, scale q by 1/sqrt(Dk), g = ssm_a*softplus(alpha+dt_bias),
 *          beta = sigmoid(b), decay/delta/readout with persistent state).
 *       - ref_gated_attention: per-head Q/K RMSNorm, causal GQA softmax,
 *         context, then out = context * sigmoid(gate).
 *  3. Assertions (behavioural, never "output is finite"):
 *       - default 3:1 schedule (RRRA...) and explicit-mask override
 *       - invalid schedule/contract rejection
 *       - two-step stateful DeltaNet forward: state changes AND matches ref
 *       - alpha gate and beta gate each materially change the result
 *       - Qwen full-attention gate materially changes the result, matches ref
 *       - no mutation of caller-owned input buffers (DeltaNet + attention)
 *       - honest capability boundary (no GGUF loader, no e2e runner)
 *
 * Exit status is the pass/fail signal for `make cce_qwen35` (0 == all pass).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>
#include <math.h>

#include "../include/cce/cce_qwen35.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; \
    printf("  FAIL: %s\n", msg); } } while (0)

/* ---- deterministic RNG (same LCG style as cce_ssm_test) ---- */
static uint64_t g_seed = 0x243F6A8885A308D3ULL;
static float rndf(void) {
    g_seed = g_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((double)(g_seed >> 33) / 2147483648.0 - 1.0) * 0.5f;
}

/* ---- geometry (small but non-degenerate: Hv!=Hk, Dk!=Dv, GQA groups) ---- */
#define HV 4   /* value heads   */
#define HK 2   /* key heads (rep = HV/HK = 2) */
#define DK 3   /* key head dim   */
#define DV 5   /* value head dim */

#define NH  4  /* attention query heads */
#define NKV 2  /* attention kv heads (group = NH/NKV = 2) */
#define HD  3  /* attention head dim */
#define NT  3  /* attention tokens */

#define MAXD 16

/* =========================================================================
 * Independent double-precision references
 * ======================================================================= */

static double d_softplus(double x) { return (x > 20.0) ? x : log1p(exp(x)); }
static double d_sigmoid(double x)  { return 1.0 / (1.0 + exp(-x)); }

/* One Gated-DeltaNet recurrent step. state is [Hv*Dk*Dv], head-major, each head
 * stored row-major as [Dk][Dv]. Mirrors torch_recurrent_gated_delta_rule. */
static void ref_deltanet_step(int Hv, int Hk, int Dk, int Dv, double eps,
                              const double *dt_bias, const double *ssm_a,
                              double *state,
                              const double *q, const double *k, const double *v,
                              const double *alpha, const double *beta,
                              double *out) {
    int rep = Hv / Hk;
    double scale = 1.0 / sqrt((double)Dk);
    for (int h = 0; h < Hv; h++) {
        int kh = h / rep;
        double qn[MAXD], kn[MAXD];
        double sq = 0.0, sk = 0.0;
        for (int a = 0; a < Dk; a++) {
            double qa = q[kh * Dk + a], ka = k[kh * Dk + a];
            sq += qa * qa; sk += ka * ka;
        }
        double invq = 1.0 / sqrt(sq + eps), invk = 1.0 / sqrt(sk + eps);
        for (int a = 0; a < Dk; a++) {
            qn[a] = q[kh * Dk + a] * invq * scale;
            kn[a] = k[kh * Dk + a] * invk;
        }
        double g = ssm_a[h] * d_softplus(alpha[h] + dt_bias[h]);
        double decay = exp(g);
        double b = d_sigmoid(beta[h]);
        double *Sh = state + (size_t)h * Dk * Dv;
        for (int a = 0; a < Dk; a++)
            for (int bb = 0; bb < Dv; bb++) Sh[a * Dv + bb] *= decay;
        double kvm[MAXD];
        for (int bb = 0; bb < Dv; bb++) {
            double s = 0.0;
            for (int a = 0; a < Dk; a++) s += Sh[a * Dv + bb] * kn[a];
            kvm[bb] = s;
        }
        double delta[MAXD];
        for (int bb = 0; bb < Dv; bb++)
            delta[bb] = (v[h * Dv + bb] - kvm[bb]) * b;
        for (int a = 0; a < Dk; a++)
            for (int bb = 0; bb < Dv; bb++) Sh[a * Dv + bb] += kn[a] * delta[bb];
        for (int bb = 0; bb < Dv; bb++) {
            double s = 0.0;
            for (int a = 0; a < Dk; a++) s += Sh[a * Dv + bb] * qn[a];
            out[h * Dv + bb] = s;
        }
    }
}

/* Gated causal attention over a full NT-token sequence. Layouts match the API:
 * q/gate [NT*NH*HD], k/v [NT*NKV*HD], out [NT*NH*HD]. */
static void ref_gated_attention(int nt, int nh, int nkv, int hd, double eps,
                                const double *q, const double *gate,
                                const double *k, const double *v,
                                const double *qn_w, const double *kn_w,
                                double *out) {
    int group = nh / nkv;
    double scale = 1.0 / sqrt((double)hd);
    for (int t = 0; t < nt; t++) {
        for (int h = 0; h < nh; h++) {
            int kvh = h / group;
            /* RMSNorm(q) over head dim */
            double qn[MAXD];
            double ss = 0.0;
            for (int d = 0; d < hd; d++) {
                double x = q[(t * nh + h) * hd + d];
                ss += x * x;
            }
            double inv = 1.0 / sqrt(ss / hd + eps);
            for (int d = 0; d < hd; d++)
                qn[d] = q[(t * nh + h) * hd + d] * inv * qn_w[d];
            /* scores over keys 0..t (causal) */
            double sc[NT];
            double mx = -1e30;
            for (int j = 0; j <= t; j++) {
                double kn[MAXD];
                double ks = 0.0;
                for (int d = 0; d < hd; d++) {
                    double x = k[(j * nkv + kvh) * hd + d];
                    ks += x * x;
                }
                double kinv = 1.0 / sqrt(ks / hd + eps);
                for (int d = 0; d < hd; d++)
                    kn[d] = k[(j * nkv + kvh) * hd + d] * kinv * kn_w[d];
                double dot = 0.0;
                for (int d = 0; d < hd; d++) dot += qn[d] * kn[d];
                sc[j] = dot * scale;
                if (sc[j] > mx) mx = sc[j];
            }
            double den = 0.0;
            for (int j = 0; j <= t; j++) { sc[j] = exp(sc[j] - mx); den += sc[j]; }
            for (int d = 0; d < hd; d++) {
                double ctx = 0.0;
                for (int j = 0; j <= t; j++)
                    ctx += (sc[j] / den) * v[(j * nkv + kvh) * hd + d];
                double gsig = d_sigmoid(gate[(t * nh + h) * hd + d]);
                out[(t * nh + h) * hd + d] = ctx * gsig;
            }
        }
    }
}

static int close_fa(float a, double b) {
    double d = fabs((double)a - b);
    return d <= 1e-4 + 1e-4 * fabs(b);
}

/* =========================================================================
 * Schedule / dispatch
 * ======================================================================= */
static void test_schedule(void) {
    printf("[schedule]\n");
    cce_qwen35_layer_kind kinds[8];

    /* default interval=4 over 8 layers -> R R R A  R R R A (3:1) */
    CHECK(cce_qwen35_default_schedule(kinds, 8, 4) == CCE_OK, "default sched ok");
    cce_qwen35_layer_kind want[8] = {
        CCE_QWEN35_LAYER_DELTANET, CCE_QWEN35_LAYER_DELTANET,
        CCE_QWEN35_LAYER_DELTANET, CCE_QWEN35_LAYER_FULL_ATTN,
        CCE_QWEN35_LAYER_DELTANET, CCE_QWEN35_LAYER_DELTANET,
        CCE_QWEN35_LAYER_DELTANET, CCE_QWEN35_LAYER_FULL_ATTN };
    int ok = 1, n_recr = 0, n_full = 0;
    for (int i = 0; i < 8; i++) {
        if (kinds[i] != want[i]) ok = 0;
        if (kinds[i] == CCE_QWEN35_LAYER_DELTANET) n_recr++; else n_full++;
    }
    CHECK(ok, "default schedule exact RRRA pattern");
    CHECK(n_recr == 6 && n_full == 2, "default schedule is 3:1 recurrent:full");
    CHECK(cce_qwen35_layer_is_recurrent(kinds[0]) == 1, "layer0 recurrent");
    CHECK(cce_qwen35_layer_is_recurrent(kinds[3]) == 0, "layer3 full-attn");

    /* explicit mask overrides the interval schedule */
    int mask[8] = { 1, 0, 0, 1, 1, 0, 1, 1 };
    CHECK(cce_qwen35_schedule_from_mask(kinds, mask, 8) == CCE_OK, "mask sched ok");
    int mok = 1;
    for (int i = 0; i < 8; i++) {
        int recr = (kinds[i] == CCE_QWEN35_LAYER_DELTANET);
        if (recr != (mask[i] != 0)) mok = 0;
    }
    CHECK(mok, "explicit mask honored exactly");
    /* the mask must differ from the default at some layer (real override) */
    cce_qwen35_layer_kind def[8];
    cce_qwen35_default_schedule(def, 8, 4);
    int differs = 0;
    for (int i = 0; i < 8; i++) if (def[i] != kinds[i]) differs = 1;
    CHECK(differs, "explicit mask actually overrides default");

    /* invalid contracts */
    CHECK(cce_qwen35_default_schedule(NULL, 8, 4) == CCE_ERR_INVALID_ARG, "null buf");
    CHECK(cce_qwen35_default_schedule(kinds, 0, 4) == CCE_ERR_INVALID_ARG, "n<=0");
    CHECK(cce_qwen35_default_schedule(kinds, 8, 0) == CCE_ERR_INVALID_ARG, "interval<=0");
    CHECK(cce_qwen35_schedule_from_mask(kinds, NULL, 8) == CCE_ERR_INVALID_ARG, "null mask");
    CHECK(cce_qwen35_schedule_from_mask(kinds, mask, -1) == CCE_ERR_INVALID_ARG, "neg n");
}

/* =========================================================================
 * Gated-DeltaNet recurrent core
 * ======================================================================= */
static void test_deltanet(void) {
    printf("[deltanet]\n");

    /* deterministic per-head gate params in a non-degenerate decay regime */
    float dt_bias[HV], ssm_a[HV];
    for (int h = 0; h < HV; h++) {
        dt_bias[h] = 1.0f + 0.1f * rndf();
        float A = 0.5f + (float)(h + 1) * 0.4f;   /* A in [0.9, 2.1] */
        ssm_a[h] = -A;                            /* == -exp(A_log) */
    }

    /* two tokens of inputs */
    float q1[HK * DK], k1[HK * DK], v1[HV * DV], a1[HV], b1[HV];
    float q2[HK * DK], k2[HK * DK], v2[HV * DV], a2[HV], b2[HV];
    for (int i = 0; i < HK * DK; i++) { q1[i] = rndf(); k1[i] = rndf(); }
    for (int i = 0; i < HK * DK; i++) { q2[i] = rndf(); k2[i] = rndf(); }
    for (int i = 0; i < HV * DV; i++) { v1[i] = rndf(); v2[i] = rndf(); }
    for (int h = 0; h < HV; h++) { a1[h] = rndf(); b1[h] = rndf(); }
    for (int h = 0; h < HV; h++) { a2[h] = rndf(); b2[h] = rndf(); }

    cce_qwen35_deltanet_config cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.num_v_heads = HV; cfg.num_k_heads = HK;
    cfg.head_k_dim = DK; cfg.head_v_dim = DV;
    cfg.l2_eps = 1e-6f; cfg.dt_bias = dt_bias; cfg.ssm_a = ssm_a;

    cce_qwen35_deltanet_state st;
    memset(&st, 0, sizeof st);
    CHECK(cce_qwen35_deltanet_init(&st, &cfg) == CCE_OK, "deltanet init");

    /* independent reference (double), shared state across the two steps */
    double dt_b[HV], sa[HV];
    for (int h = 0; h < HV; h++) { dt_b[h] = dt_bias[h]; sa[h] = ssm_a[h]; }
    double dq1[HK*DK], dk1[HK*DK], dv1[HV*DV], da1[HV], db1[HV];
    double dq2[HK*DK], dk2[HK*DK], dv2[HV*DV], da2[HV], db2[HV];
    for (int i = 0; i < HK*DK; i++){ dq1[i]=q1[i]; dk1[i]=k1[i]; dq2[i]=q2[i]; dk2[i]=k2[i]; }
    for (int i = 0; i < HV*DV; i++){ dv1[i]=v1[i]; dv2[i]=v2[i]; }
    for (int h = 0; h < HV; h++){ da1[h]=a1[h]; db1[h]=b1[h]; da2[h]=a2[h]; db2[h]=b2[h]; }

    double rstate[HV*DK*DV];
    memset(rstate, 0, sizeof rstate);
    double rout1[HV*DV], rout2[HV*DV];
    ref_deltanet_step(HV,HK,DK,DV,1e-6, dt_b, sa, rstate, dq1,dk1,dv1,da1,db1, rout1);
    /* snapshot ref state after step 1 to compare "state changes" */
    double rstate_after1[HV*DK*DV];
    memcpy(rstate_after1, rstate, sizeof rstate);
    ref_deltanet_step(HV,HK,DK,DV,1e-6, dt_b, sa, rstate, dq2,dk2,dv2,da2,db2, rout2);

    /* snapshot inputs to prove no mutation */
    float q1s[HK*DK], k1s[HK*DK], v1s[HV*DV], a1s[HV], b1s[HV];
    memcpy(q1s,q1,sizeof q1); memcpy(k1s,k1,sizeof k1); memcpy(v1s,v1,sizeof v1);
    memcpy(a1s,a1,sizeof a1); memcpy(b1s,b1,sizeof b1);

    /* step 1 through the library */
    float out1[HV*DV];
    CHECK(cce_qwen35_deltanet_step(&st, q1,k1,v1,a1,b1, out1) == CCE_OK, "step1 ok");
    CHECK(st.steps == 1, "step counter advanced");

    /* state must be non-zero after a step (it started zeroed) */
    int nonzero = 0;
    for (int i = 0; i < HV*DK*DV; i++) if (st.state[i] != 0.0f) nonzero = 1;
    CHECK(nonzero, "state is non-zero after step 1");

    /* no input mutation */
    CHECK(memcmp(q1s,q1,sizeof q1)==0 && memcmp(k1s,k1,sizeof k1)==0 &&
          memcmp(v1s,v1,sizeof v1)==0 && memcmp(a1s,a1,sizeof a1)==0 &&
          memcmp(b1s,b1,sizeof b1)==0, "step does not mutate inputs");

    /* library state after step 1 must match the reference */
    int s1ok = 1;
    for (int i = 0; i < HV*DK*DV; i++) if (!close_fa(st.state[i], rstate_after1[i])) s1ok = 0;
    CHECK(s1ok, "state after step 1 matches reference");

    /* step 1 output matches reference */
    int o1ok = 1;
    for (int i = 0; i < HV*DV; i++) if (!close_fa(out1[i], rout1[i])) o1ok = 0;
    CHECK(o1ok, "step 1 output matches reference");

    /* capture state to prove step 2 CHANGES it */
    float state_after1[HV*DK*DV];
    memcpy(state_after1, st.state, sizeof state_after1);

    /* step 2 */
    float out2[HV*DV];
    CHECK(cce_qwen35_deltanet_step(&st, q2,k2,v2,a2,b2, out2) == CCE_OK, "step2 ok");
    CHECK(st.steps == 2, "step counter is 2");
    int changed = 0;
    for (int i = 0; i < HV*DK*DV; i++) if (state_after1[i] != st.state[i]) changed = 1;
    CHECK(changed, "step 2 changes the persistent state");
    int o2ok = 1;
    for (int i = 0; i < HV*DV; i++) if (!close_fa(out2[i], rout2[i])) o2ok = 0;
    CHECK(o2ok, "step 2 output matches reference (stateful)");

    /* reset zeroes state and counter */
    cce_qwen35_deltanet_reset(&st);
    int zeroed = 1;
    for (int i = 0; i < HV*DK*DV; i++) if (st.state[i] != 0.0f) zeroed = 0;
    CHECK(zeroed && st.steps == 0, "reset zeroes state and counter");

    /* --- alpha gate materially affects the (step-2) result --- */
    /* identical step 1, then step 2 with a strongly perturbed alpha (changes
     * the decay of the step-1 state, hence the readout). */
    cce_qwen35_deltanet_state sA, sB;
    memset(&sA,0,sizeof sA); memset(&sB,0,sizeof sB);
    cce_qwen35_deltanet_init(&sA, &cfg);
    cce_qwen35_deltanet_init(&sB, &cfg);
    float oA[HV*DV], oB[HV*DV], tmp[HV*DV];
    cce_qwen35_deltanet_step(&sA, q1,k1,v1,a1,b1, tmp);
    cce_qwen35_deltanet_step(&sB, q1,k1,v1,a1,b1, tmp);
    float a2p[HV];
    for (int h = 0; h < HV; h++) a2p[h] = a2[h] + 2.0f; /* big alpha shift */
    cce_qwen35_deltanet_step(&sA, q2,k2,v2,a2,  b2, oA);
    cce_qwen35_deltanet_step(&sB, q2,k2,v2,a2p, b2, oB);
    double amax = 0.0;
    for (int i = 0; i < HV*DV; i++) { double d = fabs((double)oA[i]-oB[i]); if (d>amax) amax=d; }
    CHECK(amax > 1e-3, "alpha gate materially affects result");
    cce_qwen35_deltanet_free(&sA);
    cce_qwen35_deltanet_free(&sB);

    /* --- beta gate materially affects the (step-1) result --- */
    cce_qwen35_deltanet_state sC, sD;
    memset(&sC,0,sizeof sC); memset(&sD,0,sizeof sD);
    cce_qwen35_deltanet_init(&sC, &cfg);
    cce_qwen35_deltanet_init(&sD, &cfg);
    float b1p[HV];
    for (int h = 0; h < HV; h++) b1p[h] = b1[h] + 2.0f; /* changes write gate */
    float oC[HV*DV], oD[HV*DV];
    cce_qwen35_deltanet_step(&sC, q1,k1,v1,a1,b1,  oC);
    cce_qwen35_deltanet_step(&sD, q1,k1,v1,a1,b1p, oD);
    double bmax = 0.0;
    for (int i = 0; i < HV*DV; i++) { double d = fabs((double)oC[i]-oD[i]); if (d>bmax) bmax=d; }
    CHECK(bmax > 1e-3, "beta gate materially affects result");
    cce_qwen35_deltanet_free(&sC);
    cce_qwen35_deltanet_free(&sD);

    cce_qwen35_deltanet_free(&st);

    /* --- invalid contracts --- */
    cce_qwen35_deltanet_state bad;
    cce_qwen35_deltanet_config bcfg = cfg;
    bcfg.num_k_heads = 3; /* 3 does not divide HV=4 */
    memset(&bad,0,sizeof bad);
    CHECK(cce_qwen35_deltanet_init(&bad, &bcfg) == CCE_ERR_INVALID_ARG,
          "num_k_heads must divide num_v_heads");
    bcfg = cfg; bcfg.head_k_dim = 0;
    CHECK(cce_qwen35_deltanet_init(&bad, &bcfg) == CCE_ERR_INVALID_ARG, "dim<=0 rejected");
    bcfg = cfg; bcfg.ssm_a = NULL;
    CHECK(cce_qwen35_deltanet_init(&bad, &bcfg) == CCE_ERR_INVALID_ARG, "null ssm_a rejected");
    bcfg = cfg; bcfg.dt_bias = NULL;
    CHECK(cce_qwen35_deltanet_init(&bad, &bcfg) == CCE_ERR_INVALID_ARG, "null dt_bias rejected");
    bcfg = cfg; bcfg.num_v_heads = INT_MAX; bcfg.num_k_heads = 1;
    bcfg.head_k_dim = INT_MAX; bcfg.head_v_dim = INT_MAX;
    CHECK(cce_qwen35_deltanet_init(&bad, &bcfg) == CCE_ERR_INVALID_ARG,
          "overflowing state geometry rejected");
    CHECK(cce_qwen35_deltanet_init(NULL, &cfg) == CCE_ERR_INVALID_ARG, "null state rejected");
    /* NULL runtime args on a valid state */
    cce_qwen35_deltanet_state st2; memset(&st2,0,sizeof st2);
    cce_qwen35_deltanet_init(&st2, &cfg);
    float o[HV*DV];
    CHECK(cce_qwen35_deltanet_step(&st2, NULL,k1,v1,a1,b1,o) == CCE_ERR_INVALID_ARG, "null q rejected");
    cce_qwen35_deltanet_free(&st2);
}

/* =========================================================================
 * Gated causal attention core
 * ======================================================================= */
static void test_attention(void) {
    printf("[attention]\n");

    float q[NT*NH*HD], gate[NT*NH*HD], k[NT*NKV*HD], v[NT*NKV*HD];
    float qn_w[HD], kn_w[HD];
    for (int i = 0; i < NT*NH*HD; i++) { q[i] = rndf(); gate[i] = rndf(); }
    for (int i = 0; i < NT*NKV*HD; i++) { k[i] = rndf(); v[i] = rndf(); }
    for (int d = 0; d < HD; d++) { qn_w[d] = 1.0f + 0.1f*rndf(); kn_w[d] = 1.0f + 0.1f*rndf(); }

    cce_qwen35_attn_config cfg = { NH, NKV, HD, 1e-6f };

    /* reference */
    double dq[NT*NH*HD], dg[NT*NH*HD], dk[NT*NKV*HD], dv[NT*NKV*HD];
    double dqn[HD], dkn[HD];
    for (int i=0;i<NT*NH*HD;i++){ dq[i]=q[i]; dg[i]=gate[i]; }
    for (int i=0;i<NT*NKV*HD;i++){ dk[i]=k[i]; dv[i]=v[i]; }
    for (int d=0;d<HD;d++){ dqn[d]=qn_w[d]; dkn[d]=kn_w[d]; }
    double rout[NT*NH*HD];
    ref_gated_attention(NT,NH,NKV,HD,1e-6, dq,dg,dk,dv,dqn,dkn, rout);

    /* snapshot inputs to prove no mutation */
    float qs[NT*NH*HD], gs[NT*NH*HD], ks[NT*NKV*HD], vs[NT*NKV*HD];
    memcpy(qs,q,sizeof q); memcpy(gs,gate,sizeof gate);
    memcpy(ks,k,sizeof k); memcpy(vs,v,sizeof v);

    float out[NT*NH*HD];
    CHECK(cce_qwen35_gated_attention(&cfg, NT, q,gate,k,v, qn_w,kn_w, out) == CCE_OK,
          "gated attention ok");
    int aok = 1;
    for (int i = 0; i < NT*NH*HD; i++) if (!close_fa(out[i], rout[i])) aok = 0;
    CHECK(aok, "gated attention matches reference");
    CHECK(memcmp(qs,q,sizeof q)==0 && memcmp(gs,gate,sizeof gate)==0 &&
          memcmp(ks,k,sizeof k)==0 && memcmp(vs,v,sizeof v)==0,
          "gated attention does not mutate inputs");

    /* gate materially affects the result: driving the gate strongly negative
     * (sigmoid -> ~0) must collapse the output relative to the baseline. */
    float gate0[NT*NH*HD], out0[NT*NH*HD];
    for (int i = 0; i < NT*NH*HD; i++) gate0[i] = -20.0f;
    CHECK(cce_qwen35_gated_attention(&cfg, NT, q,gate0,k,v, qn_w,kn_w, out0) == CCE_OK,
          "gated attention (gate0) ok");
    double gdiff = 0.0, mag0 = 0.0;
    for (int i = 0; i < NT*NH*HD; i++) {
        gdiff += fabs((double)out[i]-out0[i]);
        mag0 += fabs((double)out0[i]);
    }
    CHECK(gdiff > 1e-3, "attention gate materially affects result");
    CHECK(mag0 < 1e-3, "gate ~0 collapses attention output");

    /* invalid contracts */
    cce_qwen35_attn_config bad = cfg; bad.n_kv_head = 3; /* 3 does not divide NH=4 */
    CHECK(cce_qwen35_gated_attention(&bad, NT, q,gate,k,v, qn_w,kn_w, out)
          == CCE_ERR_INVALID_ARG, "n_kv_head must divide n_head");
    bad = cfg; bad.head_dim = 0;
    CHECK(cce_qwen35_gated_attention(&bad, NT, q,gate,k,v, qn_w,kn_w, out)
          == CCE_ERR_INVALID_ARG, "head_dim<=0 rejected");
    CHECK(cce_qwen35_gated_attention(&cfg, 0, q,gate,k,v, qn_w,kn_w, out)
          == CCE_ERR_INVALID_ARG, "n_tokens<=0 rejected");
    CHECK(cce_qwen35_gated_attention(&cfg, NT, NULL,gate,k,v, qn_w,kn_w, out)
          == CCE_ERR_INVALID_ARG, "null q rejected");
    bad = cfg; bad.n_head = INT_MAX; bad.n_kv_head = 1; bad.head_dim = INT_MAX;
    CHECK(cce_qwen35_gated_attention(&bad, INT_MAX, q,gate,k,v, qn_w,kn_w, out)
          == CCE_ERR_INVALID_ARG, "overflowing attention geometry rejected");
}

/* =========================================================================
 * Honest capability boundary
 * ======================================================================= */
static void test_caps(void) {
    printf("[caps]\n");
    cce_qwen35_caps c;
    memset(&c, 0xAB, sizeof c);
    cce_qwen35_get_caps(&c);
    CHECK(c.layer_schedule_dispatch == 1, "advertises schedule dispatch");
    CHECK(c.deltanet_recurrent_core == 1, "advertises deltanet core");
    CHECK(c.gated_attention_core == 1, "advertises gated attention core");
    CHECK(c.gguf_loader == 0, "does NOT claim a GGUF loader");
    CHECK(c.end_to_end_runner == 0, "does NOT claim an e2e runner");
    CHECK(c.applies_rope == 0, "honest: RoPE is caller's job");
    CHECK(c.applies_short_conv == 0, "honest: conv is caller's job");
    CHECK(c.summary != NULL && c.summary[0] != '\0', "has a summary string");
}

int main(void) {
    printf("=== cce_qwen35 native Gated-DeltaNet + gated-attention core ===\n");
    test_schedule();
    test_deltanet();
    test_attention();
    test_caps();
    printf("checks: %d, fails: %d -> %s\n", checks, fails,
           fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
