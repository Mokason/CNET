/* dense_stream_a2: Arc A2 of the Dense expert-streaming arc.
 *
 * A1 (dense_stream_q) proved a NAIVE int8 model streams from the content-
 * addressed store under a bounded resident cap, BIT-IDENTICALLY, at a real
 * on-disk reduction. A2 adds the QUALITY story: quantize each streamed
 * specialist DATA-AWARE (our GPTQ/OBQ solver, calibrated on that specialist's
 * REAL input activations), prove it still streams bit-identically, and show the
 * data-aware win over naive — at int8 (near-lossless, marginal) and, where it
 * matters, at int4 (naive loses real quality; data-aware recovers it).
 *
 * NEW capability exercised: per-specialist activation capture. The streamable
 * forward (cce_gguf_qwen2_forward) grows an OPT-IN hook (cce_gguf_set_capture_hook)
 * that fires immediately before each linear specialist's matvec with that
 * specialist's branch name + the input rows. Default OFF => the forward is
 * byte-identical (make test stays green). A short calibration pass over several
 * token sequences accumulates each specialist's REAL activations -> Sigma=XᵀX ->
 * the OBQ reconstruction. Because we instrument the monolithic forward cleanly,
 * ALL specialists (every layer, not just layer 0) get REAL calibration.
 *
 * The four variants, each ingested / restored / tier-attached / streamed:
 *   naive-int8    : per-out absmax/127, no Sigma            (grid [-127,127])
 *   dataaware-int8: GPTQ-OBQ on real activations            (grid [-127,127])
 *   naive-int4    : per-out absmax/7,   no Sigma            (grid [-7,7])
 *   dataaware-int4: GPTQ-OBQ on real activations            (grid [-7,7])
 * int4 codes ride in the SAME int8 w_q array (values in [-7,7]) so the A1.5
 * store path round-trips them unchanged — no packed-payload path needed. (True
 * 4-bit STORAGE, 8x/20x packed, is the A2-follow-on; A2 proves the data-aware
 * QUALITY win that streams bit-identically.)
 *
 * GATES per variant:
 *   - streaming logits BIT-IDENTICAL to that variant's all-resident logits (dmax==0),
 *   - resident <= cap, one streaming pass (rehydrations == specialist count),
 *   - store bytes(int quant) < store bytes(FP).
 * QUALITY: held-out (disjoint from calibration) logit relerr of each streamed
 * variant vs the FP all-resident model. Firm gate: dataaware-int4 < naive-int4.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../include/cce/cce_weight_store.h"
#include "../include/cce/cce_tier_runtime.h"
#include "../include/cce/cce_forest.h"
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_block.h"
#include "../include/cce/cce_detect.h"
#include "tiny_model_fixture.h"

#ifdef _WIN32
#include <io.h>
#else
#include <dirent.h>
#endif

static int checks = 0, fails = 0;
static FILE* LOGF = NULL;
#define LOG(...) do { printf(__VA_ARGS__); if (LOGF) fprintf(LOGF, __VA_ARGS__); } while (0)
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; LOG("  FAIL: %s\n", msg); } \
                              else { LOG("  ok:   %s\n", msg); } } while (0)

static void wipe_store_dir(const char* dir) {
#ifdef _WIN32
    struct _finddata_t fd; char pat[600], path[700];
    snprintf(pat, sizeof(pat), "%s/*.spec", dir);
    intptr_t h = _findfirst(pat, &fd);
    if (h != -1) { do { snprintf(path, sizeof(path), "%s/%s", dir, fd.name); remove(path); }
                   while (_findnext(h, &fd) == 0); _findclose(h); }
#else
    DIR* d = opendir(dir);
    if (d) { struct dirent* ent; char path[700];
        while ((ent = readdir(d)) != NULL) {
            size_t n = strlen(ent->d_name);
            if (n > 5 && strcmp(ent->d_name + n - 5, ".spec") == 0) {
                snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name); remove(path);
            }
        }
        closedir(d);
    }
#endif
    remove(dir);
}

/* ======================= per-specialist activation capture ======================= */
typedef struct { char name[64]; float* rows; int in_dim; int n_rows; int cap_rows; } spec_cap;
typedef struct { spec_cap specs[64]; int n_specs; } cap_ctx;

static spec_cap* cap_find(cap_ctx* c, const char* name) {
    for (int i = 0; i < c->n_specs; i++) if (strcmp(c->specs[i].name, name) == 0) return &c->specs[i];
    return NULL;
}
static void capture_cb(const char* spec_name, const float* rows, int n_rows, int in_dim, void* uctx) {
    cap_ctx* c = (cap_ctx*)uctx;
    spec_cap* sc = cap_find(c, spec_name);
    if (!sc) {
        if (c->n_specs >= 64) return;
        sc = &c->specs[c->n_specs++];
        snprintf(sc->name, sizeof(sc->name), "%s", spec_name);
        sc->in_dim = in_dim; sc->n_rows = 0; sc->cap_rows = 0; sc->rows = NULL;
    }
    if (sc->in_dim != in_dim) return; /* shape guard */
    if (sc->n_rows + n_rows > sc->cap_rows) {
        int nc = sc->cap_rows ? sc->cap_rows * 2 : 256;
        while (nc < sc->n_rows + n_rows) nc *= 2;
        sc->rows = (float*)realloc(sc->rows, (size_t)nc * in_dim * sizeof(float));
        sc->cap_rows = nc;
    }
    memcpy(sc->rows + (size_t)sc->n_rows * in_dim, rows, (size_t)n_rows * in_dim * sizeof(float));
    sc->n_rows += n_rows;
}
static void cap_free(cap_ctx* c) { for (int i = 0; i < c->n_specs; i++) free(c->specs[i].rows); }

/* ======================= Sigma = XᵀX + 1% mean-diag damping ======================= */
static void build_sigma(const float* X, int n, int in, double* Sig) {
    for (int i = 0; i < in; i++) {
        double* row = Sig + (size_t)i * in;
        for (int j = 0; j < in; j++) row[j] = 0.0;
        for (int r = 0; r < n; r++) {
            double xi = (double)X[(size_t)r * in + i]; const float* xr = X + (size_t)r * in;
            for (int j = 0; j < in; j++) row[j] += xi * (double)xr[j];
        }
    }
    double md = 0; for (int i = 0; i < in; i++) md += Sig[(size_t)i * in + i]; md /= (in ? in : 1);
    for (int i = 0; i < in; i++) Sig[(size_t)i * in + i] += 0.01 * md;   /* GPTQ 1% mean-diag */
}

/* ======================= GPTQ-Cholesky OBQ solver (grid-generalized) =======================
 * Copied from tests/gptq_solver.c / tests/proj_qat_gemma_e2e.c and GENERALIZED to a
 * symmetric k-level grid: rounds to [-K,K] instead of the ternary [-1,1]. int8 => K=127,
 * int4 => K=7 (the error-feedback + optimal-scale refit are grid-agnostic). W laid out
 * [in][out]; Sig is [in][in] and already damped. Outputs int8 codes[in*out] + per-out scale.
 * Single OBQ error-feedback pass in act-order + optimal-scale refit (no coordinate polish,
 * matching the gemma e2e "fast+refit" config). */
static void gptq_build_U(const double* H, int in, double* U, int* dead) {
    double* L    = (double*)calloc((size_t)in * in, sizeof(double));
    double* Minv = (double*)calloc((size_t)in * in, sizeof(double));
    double* Hinv = (double*)calloc((size_t)in * in, sizeof(double));
    memset(U, 0, (size_t)in * in * sizeof(double));
    double md = 0; for (int i = 0; i < in; i++) md += H[(size_t)i * in + i]; md /= (in ? in : 1);
    double tiny = 1e-10 * (md > 0 ? md : 1.0);
    /* 1) lower Cholesky H = L Lᵀ (dead-column guarded) */
    for (int j = 0; j < in; j++) {
        double d = H[(size_t)j * in + j];
        for (int k = 0; k < j; k++) { double l = L[(size_t)j * in + k]; d -= l * l; }
        if (d <= tiny) { dead[j] = 1; L[(size_t)j * in + j] = 1.0; }
        else { dead[j] = 0; L[(size_t)j * in + j] = sqrt(d); }
        if (dead[j]) continue;
        double ljj = L[(size_t)j * in + j];
        for (int i = j + 1; i < in; i++) {
            double s = H[(size_t)i * in + j];
            const double* Li = L + (size_t)i * in; const double* Lj = L + (size_t)j * in;
            for (int k = 0; k < j; k++) s -= Li[k] * Lj[k];
            L[(size_t)i * in + j] = s / ljj;
        }
    }
    /* 2) invert lower-triangular L -> Minv */
    for (int i = 0; i < in; i++) {
        double lii = L[(size_t)i * in + i];
        Minv[(size_t)i * in + i] = 1.0 / lii;
        for (int j = 0; j < i; j++) {
            if (dead[j]) { Minv[(size_t)i * in + j] = 0.0; continue; }
            double s = 0; for (int k = j; k < i; k++) s += L[(size_t)i * in + k] * Minv[(size_t)k * in + j];
            Minv[(size_t)i * in + j] = -s / lii;
        }
    }
    /* 3) Hinv = Minvᵀ Minv */
    for (int a = 0; a < in; a++)
        for (int b = a; b < in; b++) {
            double s = 0; for (int k = b; k < in; k++) s += Minv[(size_t)k * in + a] * Minv[(size_t)k * in + b];
            Hinv[(size_t)a * in + b] = s; Hinv[(size_t)b * in + a] = s;
        }
    /* 4) upper Cholesky Hinv = Uᵀ U */
    for (int i = 0; i < in; i++) {
        double d = Hinv[(size_t)i * in + i];
        for (int k = 0; k < i; k++) { double u = U[(size_t)k * in + i]; d -= u * u; }
        if (d <= tiny || dead[i]) {
            dead[i] = 1; U[(size_t)i * in + i] = 1.0;
            for (int j = i + 1; j < in; j++) U[(size_t)i * in + j] = 0.0;
            continue;
        }
        U[(size_t)i * in + i] = sqrt(d);
        double uii = U[(size_t)i * in + i];
        for (int j = i + 1; j < in; j++) {
            double s = Hinv[(size_t)i * in + j];
            for (int k = 0; k < i; k++) s -= U[(size_t)k * in + i] * U[(size_t)k * in + j];
            U[(size_t)i * in + j] = s / uii;
        }
    }
    free(L); free(Minv); free(Hinv);
}
static const double* g_sortdiag;
static int cmp_desc(const void* a, const void* b) {
    int ia = *(const int*)a, ib = *(const int*)b;
    double da = g_sortdiag[ia], db = g_sortdiag[ib];
    return (da < db) - (da > db);
}
/* returns codes (int8, [in*out]) + per-out scale; grid level K (>=1). */
static void reconstruct_gptq_k(const float* W, int in, int out, const double* Sig, int K,
                               int8_t* codes, float* scale) {
    int* perm = (int*)malloc((size_t)in * sizeof(int));
    for (int i = 0; i < in; i++) perm[i] = i;
    double* diag = (double*)malloc((size_t)in * sizeof(double));
    for (int i = 0; i < in; i++) diag[i] = Sig[(size_t)i * in + i];
    g_sortdiag = diag; qsort(perm, in, sizeof(int), cmp_desc); g_sortdiag = NULL;

    double* Hp = (double*)malloc((size_t)in * in * sizeof(double));
    for (int a = 0; a < in; a++) { int pa = perm[a]; const double* Sa = Sig + (size_t)pa * in;
        double* Ha = Hp + (size_t)a * in; for (int b = 0; b < in; b++) Ha[b] = Sa[perm[b]]; }

    double* Wr  = (double*)malloc((size_t)out * in * sizeof(double)); /* OBQ working buffer */
    double* Wo  = (double*)malloc((size_t)out * in * sizeof(double)); /* original (for refit) */
    double* gam = (double*)malloc((size_t)out * sizeof(double));
    for (int o = 0; o < out; o++) {
        double* wr = Wr + (size_t)o * in; double* wo = Wo + (size_t)o * in; double mx = 0;
        for (int k = 0; k < in; k++) { double v = (double)W[(size_t)perm[k] * out + o];
            wr[k] = v; wo[k] = v; double a = fabs(v); if (a > mx) mx = a; }
        gam[o] = (mx > 0) ? mx / (double)K : 1.0;   /* absmax/K init (spreads across the grid) */
    }
    double* U = (double*)malloc((size_t)in * in * sizeof(double));
    int* dead = (int*)calloc((size_t)in, sizeof(int));
    gptq_build_U(Hp, in, U, dead);

    double* Q = (double*)malloc((size_t)out * in * sizeof(double)); /* integer pattern */
    for (int i = 0; i < in; i++) {
        double uii = U[(size_t)i * in + i]; int di = dead[i]; const double* Ui = U + (size_t)i * in;
        for (int o = 0; o < out; o++) {
            double* wr = Wr + (size_t)o * in; double e, r = 0.0;
            if (di) e = 0.0;
            else { double g = gam[o], wv = wr[i];
                if (g > 0) { r = round(wv / g); if (r > K) r = K; if (r < -K) r = -K; }
                e = (wv - g * r) / uii; }
            Q[(size_t)o * in + i] = r;
            if (e != 0.0) for (int j = i + 1; j < in; j++) wr[j] -= e * Ui[j];
        }
    }
    /* optimal-scale refit gamma* = (qᵀΣw)/(qᵀΣq) per output row */
    double* Sw = (double*)malloc((size_t)in * sizeof(double));
    for (int o = 0; o < out; o++) {
        double* q = Q + (size_t)o * in; const double* wo = Wo + (size_t)o * in;
        for (int i = 0; i < in; i++) {
            double s = 0; const double* Hi = Hp + (size_t)i * in;
            for (int j = 0; j < in; j++) s += Hi[j] * wo[j];
            Sw[i] = s;
        }
        double num = 0, den = 0;
        for (int i = 0; i < in; i++) {
            double sq = 0; const double* Hi = Hp + (size_t)i * in;
            for (int j = 0; j < in; j++) sq += Hi[j] * q[j];
            num += q[i] * Sw[i]; den += q[i] * sq;
        }
        if (den > 1e-12) { double gn = num / den; if (gn > 0) gam[o] = gn; }
        if (gam[o] <= 0) gam[o] = 1.0;
    }
    free(Sw);
    /* scatter codes (inverse perm) + scale */
    for (int o = 0; o < out; o++) {
        scale[o] = (float)gam[o]; const double* q = Q + (size_t)o * in;
        for (int k = 0; k < in; k++) {
            long ci = (long)q[k]; if (ci > K) ci = K; if (ci < -K) ci = -K;
            codes[(size_t)perm[k] * out + o] = (int8_t)ci;
        }
    }
    free(perm); free(diag); free(Hp); free(Wr); free(Wo); free(gam); free(U); free(dead); free(Q);
}

/* ======================= naive + data-aware block quantizers ======================= */
/* NAIVE: per-output absmax/K, round-clip to [-K,K]. K=127 == cce_block_quantize_int8. */
static void naive_quant_block(cce_block* blk, int K) {
    int in = blk->weights.shape[0], out = blk->weights.shape[1];
    int8_t* q = (int8_t*)malloc((size_t)in * out); float* sc = (float*)calloc((size_t)out, sizeof(float));
    for (int i = 0; i < in; i++) { const float* wr = &blk->weights.data[(size_t)i * out];
        for (int o = 0; o < out; o++) { float a = fabsf(wr[o]); if (a > sc[o]) sc[o] = a; } }
    for (int o = 0; o < out; o++) sc[o] = (sc[o] > 0.0f) ? sc[o] / (float)K : 1.0f;
    for (int i = 0; i < in; i++) {
        const float* wr = &blk->weights.data[(size_t)i * out]; int8_t* qr = &q[(size_t)i * out];
        for (int o = 0; o < out; o++) {
            long c = lroundf(wr[o] / sc[o]);
            if (c > K) c = K;
            if (c < -K) c = -K;
            qr[o] = (int8_t)c;
        }
    }
    blk->w_q = q; blk->w_scale = sc;
}
/* DATA-AWARE: build Sigma from captured activations, run the OBQ solver at grid K. */
static void dataaware_quant_block(cce_block* blk, const spec_cap* sc_act, int K) {
    int in = blk->weights.shape[0], out = blk->weights.shape[1];
    double* Sig = (double*)malloc((size_t)in * in * sizeof(double));
    build_sigma(sc_act->rows, sc_act->n_rows, in, Sig);
    int8_t* q = (int8_t*)malloc((size_t)in * out); float* scl = (float*)malloc((size_t)out * sizeof(float));
    reconstruct_gptq_k(blk->weights.data, in, out, Sig, K, q, scl);
    blk->w_q = q; blk->w_scale = scl;
    free(Sig);
}

/* Quantize every linear specialist (except lm_head/mtp — kept FP, mirroring
 * cce_gguf_qwen2_quantize_int8). Returns #blocks quantized; *n_real counts the
 * data-aware blocks that used REAL captured activations. */
static int quantize_model(cce_gguf_qwen2* t, cap_ctx* cap, int K, int dataaware, int* n_real) {
    int n = 0; if (n_real) *n_real = 0;
    for (int b = 0; b < t->forest->num_branches; b++) {
        const char* bn = t->forest->branches[b].name;
        if (strstr(bn, "lm_head") || strstr(bn, "mtp.")) continue;
        cce_cascade* cas = t->forest->branches[b].cascade; if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; j++) {
            cce_block* blk = &cas->blocks[j];
            if (!blk->weights.data || blk->weights.ndim != 2) continue;
            int in = blk->weights.shape[0];
            if (dataaware) {
                spec_cap* sa = cap_find(cap, bn);
                if (sa && sa->in_dim == in && sa->n_rows >= in) {
                    dataaware_quant_block(blk, sa, K); if (n_real) (*n_real)++;
                } else {
                    naive_quant_block(blk, K); /* fallback (should not trigger: all captured) */
                }
            } else {
                naive_quant_block(blk, K);
            }
            n++;
        }
    }
    return n;
}

/* Run n_seq eval sequences (each seq_len tokens), reset KV each, write each
 * sequence's last-token logits into out[seq*TL_V ..]. */
static int run_eval(cce_gguf_qwen2* m, int seqs[][8], int n_seq, int seq_len, float* out) {
    for (int s = 0; s < n_seq; s++) {
        m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, seqs[s], seq_len, out + (size_t)s * TL_V, TL_V) != CCE_OK) return -1;
    }
    return 0;
}

int main(void) {
    LOGF = fopen("logs/dense_stream_a2.log", "wb");
    LOG("=== dense_stream_a2: DATA-AWARE quantized dense streaming (Arc A2) ===\n");

    const int N_SPECS = 7 * TL_L + 1;   /* 15: q,k,v,o,gate,up,down per layer + lm_head */
    const int HOT_CAP = 8;              /* one layer's working set, << N_SPECS */

    /* calibration token sequences (source of the captured activations) */
    #define NCAL 16
    #define CAL_LEN 8
    int cal[NCAL][8];
    { uint64_t r = 0xD1B54A32D192ED03ULL;
      for (int s = 0; s < NCAL; s++) for (int t = 0; t < CAL_LEN; t++) {
          r = r * 6364136223846793005ULL + 1442695040888963407ULL;
          cal[s][t] = (int)((r >> 40) % (uint64_t)TL_V); } }
    /* held-out eval sequences, DISJOINT construction from calibration */
    #define NEVAL 8
    #define EVAL_LEN 6
    int eval[NEVAL][8];
    { uint64_t r = 0x9E3779B97F4A7C15ULL;
      for (int s = 0; s < NEVAL; s++) for (int t = 0; t < EVAL_LEN; t++) {
          r = r * 2862933555777941757ULL + 3037000493ULL;
          eval[s][t] = (int)((r >> 33) % (uint64_t)TL_V); } }

    tl_weights* w = (tl_weights*)malloc(sizeof(tl_weights));
    tl_gen(w, 0);
    tl_write_st_dir("dsa2_src", w);

    /* ---------- calibration: capture REAL per-specialist activations ---------- */
    cap_ctx cap; memset(&cap, 0, sizeof(cap));
    float refFP[NEVAL * TL_V];
    {
        cce_anymodel* mcal = NULL;
        CHECK(cce_anymodel_open(&mcal, "dsa2_src/model.safetensors") == CCE_OK && mcal, "calib model opens");
        if (!mcal) return 1;
        cce_gguf_set_capture_hook(capture_cb, &cap);
        for (int s = 0; s < NCAL; s++) {
            mcal->transformer->cur_pos = 0;
            float scratch[TL_V];
            cce_gguf_qwen2_forward(mcal->transformer, cal[s], CAL_LEN, scratch, TL_V);
        }
        cce_gguf_set_capture_hook(NULL, NULL);   /* detach: forward byte-identical again */
        /* FP reference eval logits (hook off) */
        CHECK(run_eval(mcal->transformer, eval, NEVAL, EVAL_LEN, refFP) == 0, "FP all-resident eval forward");
        cce_anymodel_free(mcal);
    }

    LOG("\n--- capture approach: OPT-IN forward hook (cce_gguf_set_capture_hook) ---\n");
    LOG("  captured %d specialists over %d calib seqs x %d tok = %d rows/spec target\n",
        cap.n_specs, NCAL, CAL_LEN, NCAL * CAL_LEN);
    int all_real = 1, min_over = 1 << 30;
    for (int i = 0; i < cap.n_specs; i++) {
        spec_cap* s = &cap.specs[i];
        int quant_spec = !(strstr(s->name, "lm_head") || strstr(s->name, "mtp."));
        double over = s->in_dim ? (double)s->n_rows / s->in_dim : 0;
        LOG("    %-22s in_dim=%2d  rows=%4d  (%.1fx over in_dim)%s\n",
            s->name, s->in_dim, s->n_rows, over, quant_spec ? "" : "  [FP: not quantized]");
        if (quant_spec) { if (s->n_rows < s->in_dim) all_real = 0;
                          if (s->n_rows / (s->in_dim ? s->in_dim : 1) < min_over) min_over = s->n_rows / (s->in_dim ? s->in_dim : 1); }
    }
    CHECK(all_real, "REAL activations captured for every quantized specialist (all layers, rows >= in_dim)");
    LOG("  => calibration is REAL for ALL specialists (monolithic forward instrumented; no synthetic fallback).\n");

    /* ---------- FP store bytes (compression baseline) ---------- */
    size_t bytes_fp = 0;
    {
        wipe_store_dir("dsa2_fp");
        cce_weight_store* sfp = NULL;
        CHECK(cce_weight_store_open(&sfp, "dsa2_fp") == CCE_OK && sfp, "FP store opens");
        cce_anymodel* mb = NULL;
        CHECK(cce_anymodel_open(&mb, "dsa2_src/model.safetensors") == CCE_OK && mb, "FP model reopens for ingest");
        int nt = 0, nn = 0, nr = 0;
        CHECK(cce_weight_store_ingest_model(sfp, mb, "modelFP", "dsa2_fp.manifest", &nt, &nn, &nr) == CCE_OK, "FP model ingests");
        bytes_fp = cce_weight_store_bytes(sfp);
        cce_anymodel_free(mb);
        cce_weight_store_close(sfp);
    }

    /* ---------- the four variants ---------- */
    struct { const char* tag; int K; int dataaware; } V[4] = {
        { "naive-int8",     127, 0 }, { "dataaware-int8", 127, 1 },
        { "naive-int4",       7, 0 }, { "dataaware-int4",   7, 1 },
    };
    double v_relerr[4]; size_t v_bytes[4]; int v_bitok[4];

    for (int vi = 0; vi < 4; vi++) {
        LOG("\n===================== variant: %s (grid [-%d,%d], %s) =====================\n",
            V[vi].tag, V[vi].K, V[vi].K, V[vi].dataaware ? "DATA-AWARE GPTQ/OBQ" : "naive absmax");

        char store[64], man[64], restore[64];
        snprintf(store, sizeof(store), "dsa2_store_%d", vi);
        snprintf(man, sizeof(man), "dsa2_%d.manifest", vi);
        snprintf(restore, sizeof(restore), "dsa2_restore_%d.cce", vi);

        /* quantize a fresh model */
        cce_anymodel* ma = NULL;
        CHECK(cce_anymodel_open(&ma, "dsa2_src/model.safetensors") == CCE_OK && ma, "variant model opens");
        if (!ma) return 1;
        int n_real = 0, nq = quantize_model(ma->transformer, &cap, V[vi].K, V[vi].dataaware, &n_real);
        LOG("  quantized %d linear specialists (%d via REAL data-aware GPTQ)\n", nq, n_real);
        CHECK(nq == N_SPECS - 1, "quantized every linear specialist except lm_head");
        if (V[vi].dataaware) CHECK(n_real == nq, "all data-aware specialists used REAL captured activations");

        /* all-resident quantized reference logits */
        float refV[NEVAL * TL_V];
        CHECK(run_eval(ma->transformer, eval, NEVAL, EVAL_LEN, refV) == 0, "all-resident quantized eval forward");

        /* ingest -> store bytes */
        wipe_store_dir(store);
        cce_weight_store* sq = NULL;
        CHECK(cce_weight_store_open(&sq, store) == CCE_OK && sq, "quant store opens");
        int nt = 0, nn = 0, nr = 0;
        CHECK(cce_weight_store_ingest_model(sq, ma, "modelQ", man, &nt, &nn, &nr) == CCE_OK, "quant model ingests");
        v_bytes[vi] = cce_weight_store_bytes(sq);
        cce_anymodel_free(ma);

        /* restore + tier-attach + stream (cold) */
        cce_gguf_qwen2* m = NULL;
        CHECK(cce_weight_store_restore_transformer(sq, man, restore, &m) == CCE_OK && m, "restores from quant store");
        if (!m) return 1;
        cce_tier_runtime* rt = NULL;
        CHECK(cce_tier_attach(&rt, m, sq, man, HOT_CAP) == CCE_OK && rt, "tier runtime attaches");
        if (!rt) return 1;
        CHECK(cce_tier_evict_all(rt) == CCE_OK, "evict all (start cold)");
        CHECK(cce_tier_resident(rt) == 0, "starts fully cold");

        /* first streamed forward: residency mechanics on ONE pass */
        float streamV[NEVAL * TL_V];
        m->cur_pos = 0;
        CHECK(cce_gguf_qwen2_forward(m, eval[0], EVAL_LEN, streamV, TL_V) == CCE_OK, "streaming forward runs");
        int rehydr1 = cce_tier_rehydrations(rt);
        LOG("  residency: hot_cap=%d N_SPECS=%d high_water=%d rehydrations(1 pass)=%d resident=%d\n",
            HOT_CAP, N_SPECS, cce_tier_high_water(rt), rehydr1, cce_tier_resident(rt));
        CHECK(cce_tier_high_water(rt) <= HOT_CAP, "resident specialists never exceeded the cap");
        CHECK(rehydr1 == N_SPECS, "one streaming pass: each specialist fetched exactly once");

        /* remaining eval sequences (residency stays bounded; math unchanged) */
        for (int s = 1; s < NEVAL; s++) {
            m->cur_pos = 0;
            CHECK(cce_gguf_qwen2_forward(m, eval[s], EVAL_LEN, streamV + (size_t)s * TL_V, TL_V) == CCE_OK,
                  s == 1 ? "further streamed eval forwards run" : "...");
        }
        CHECK(cce_tier_high_water(rt) <= HOT_CAP, "resident cap held across all eval forwards");

        /* GATE: bit-identity vs all-resident quantized ; QUALITY vs FP */
        float dmax_q = 0; double num = 0, den = 0, numFP = 0;
        for (int i = 0; i < NEVAL * TL_V; i++) {
            float a = fabsf(streamV[i] - refV[i]); if (a > dmax_q) dmax_q = a;
            double d = (double)streamV[i] - (double)refFP[i]; num += d * d; den += (double)refFP[i] * refFP[i];
            double df = (double)refV[i] - (double)refFP[i]; numFP += df * df; /* (all-resident vs FP, sanity) */
        }
        double relerr = den > 0 ? sqrt(num / den) : 0.0;
        v_relerr[vi] = relerr; v_bitok[vi] = (dmax_q == 0.0f);
        LOG("  dmax(stream, all-resident quant) = %.6g  %s\n", (double)dmax_q,
            dmax_q == 0.0f ? "-> streaming BIT-IDENTICAL" : "-> DIVERGENCE");
        LOG("  held-out logit relerr (stream vs FP all-resident) = %.6f\n", relerr);
        LOG("  store bytes: quant=%zu  FP=%zu  (%.3fx)\n", v_bytes[vi], bytes_fp,
            v_bytes[vi] ? (double)bytes_fp / v_bytes[vi] : 0.0);
        CHECK(dmax_q == 0.0f, "streaming logits BIT-IDENTICAL to the all-resident quantized model (dmax==0)");
        CHECK(v_bytes[vi] < bytes_fp, "store captures the quantization (quant payload < FP ingest)");

        cce_tier_detach(rt);
        cce_gguf_qwen2_free(m);
        remove(restore);
        cce_weight_store_close(sq);
        wipe_store_dir(store);
        remove(man);
    }

    /* ---------- the quality table: data-aware vs naive at int8 and int4 ---------- */
    LOG("\n===================== held-out quality: data-aware vs naive =====================\n");
    LOG("  (streamed logit relerr vs FP all-resident; all four stream BIT-IDENTICALLY)\n");
    LOG("    grid    naive-relerr   dataaware-relerr   data-aware win\n");
    LOG("    int8   %12.6f   %14.6f   %+.1f%%\n", v_relerr[0], v_relerr[1],
        v_relerr[0] > 0 ? 100.0 * (v_relerr[0] - v_relerr[1]) / v_relerr[0] : 0.0);
    LOG("    int4   %12.6f   %14.6f   %+.1f%%\n", v_relerr[2], v_relerr[3],
        v_relerr[2] > 0 ? 100.0 * (v_relerr[2] - v_relerr[3]) / v_relerr[2] : 0.0);
    LOG("  store bytes (all quant variants identical: int4 codes ride in int8 w_q): "
        "int8=%zu int4=%zu FP=%zu\n", v_bytes[1], v_bytes[3], bytes_fp);
    LOG("  NOTE: int4 packed 4-bit STORAGE (8x/20x) is the A2-follow-on (needs a packed\n");
    LOG("        payload path); A2 proves the data-aware int4 QUALITY win that streams bit-identically.\n");

    LOG("\n--- gates ---\n");
    CHECK(v_bitok[0] && v_bitok[1] && v_bitok[2] && v_bitok[3],
          "ALL four variants stream bit-identically to their all-resident twin");
    CHECK(v_relerr[3] < v_relerr[2],
          "THE WIN: data-aware int4 held-out relerr < naive int4 (data-aware recovers int4 quality)");
    CHECK(v_relerr[1] <= v_relerr[0] + 1e-4,
          "data-aware int8 not worse than naive int8 (near-lossless: marginal, as expected)");
    CHECK(v_relerr[0] < 0.05 && v_relerr[1] < 0.05,
          "int8 both near-lossless (relerr < 5%)");

    /* teardown */
    cap_free(&cap);
    wipe_store_dir("dsa2_fp");
    tl_cleanup_st_dir("dsa2_src");
    remove("dsa2_fp.manifest");
    free(w);

    LOG("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    if (LOGF) fclose(LOGF);
    return fails ? 1 : 0;
}
