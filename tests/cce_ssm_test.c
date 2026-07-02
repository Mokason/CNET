/* cce_ssm: mamba-1 selective SSM runner, born exact.
 *
 * Verification strategy (non-circular):
 *  1. Generate deterministic weights as plain float arrays.
 *  2. Run an INDEPENDENT double-precision reference (plain loops, written
 *     directly from the recurrence definition, no CCE code) on those arrays.
 *  3. Write the same weights as an HF-style safetensors file, load through
 *     cce_ssm_load (forest decomposition + cascades), compare every token's
 *     logits against the reference.
 *  4. Write the same weights as a GGUF file (llama.cpp mamba naming) and
 *     assert the two container paths produce IDENTICAL logits (mapping
 *     equivalence; both stores are f32).
 *  5. State discipline: incremental == batch, reset reproduces exactly.
 *  6. Refusals: incomplete files fail cleanly; the gguf transformer
 *     entry point refuses ssm ggufs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "../include/cce/cce_ssm.h"
#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_detect.h"

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

/* ---- dims (small but not degenerate: E != D, N != K, R > 1) ---- */
#define L_  2
#define D_  6
#define E_  8
#define N_  4
#define K_  4
#define R_  2
#define V_  12
#define XDB_ (R_ + 2 * N_)

/* ---- deterministic weights ---- */

static uint64_t g_seed = 0x243F6A8885A308D3ULL;
static float rndf(void) {
    g_seed = g_seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((double)(g_seed >> 33) / 2147483648.0 - 1.0) * 0.5f;
}

typedef struct {
    float norm[L_][D_];
    float in_w[L_][2 * E_][D_];
    float conv_w[L_][E_][K_];
    float conv_b[L_][E_];
    float x_w[L_][XDB_][E_];
    float dt_w[L_][E_][R_];
    float dt_b[L_][E_];
    float a_log[L_][E_][N_];
    float dvec[L_][E_];
    float out_w[L_][D_][E_];
    float emb[V_][D_];
    float norm_f[D_];
} ssm_weights;

static void gen_weights(ssm_weights* w) {
    float* p = (float*)w;
    size_t n = sizeof(*w) / sizeof(float);
    for (size_t i = 0; i < n; i++) p[i] = rndf();
    /* norms near 1 like trained models */
    for (int l = 0; l < L_; l++)
        for (int i = 0; i < D_; i++) w->norm[l][i] = 1.0f + 0.1f * w->norm[l][i];
    for (int i = 0; i < D_; i++) w->norm_f[i] = 1.0f + 0.1f * w->norm_f[i];
}

/* ---- independent double-precision reference ---- */

static double ref_silu(double x)     { return x / (1.0 + exp(-x)); }
static double ref_softplus(double x) { return (x > 20.0) ? x : log1p(exp(x)); }

static void ref_forward(const ssm_weights* w, const int* tokens, int n_tokens,
                        double logits[][V_]) {
    double conv_state[L_][E_][K_];
    double ssm_state[L_][E_][N_];
    memset(conv_state, 0, sizeof(conv_state));
    memset(ssm_state, 0, sizeof(ssm_state));

    for (int t = 0; t < n_tokens; t++) {
        double x[D_];
        for (int i = 0; i < D_; i++) x[i] = w->emb[tokens[t]][i];

        for (int l = 0; l < L_; l++) {
            /* rms norm */
            double ss = 0;
            for (int i = 0; i < D_; i++) ss += x[i] * x[i];
            ss = 1.0 / sqrt(ss / D_ + 1e-5);
            double xn[D_];
            for (int i = 0; i < D_; i++) xn[i] = x[i] * ss * w->norm[l][i];

            /* in_proj */
            double xz[2 * E_];
            for (int o = 0; o < 2 * E_; o++) {
                double acc = 0;
                for (int i = 0; i < D_; i++) acc += (double)w->in_w[l][o][i] * xn[i];
                xz[o] = acc;
            }

            /* conv shift + depthwise conv + silu */
            double xc[E_];
            for (int e = 0; e < E_; e++) {
                for (int k = 0; k < K_ - 1; k++) conv_state[l][e][k] = conv_state[l][e][k + 1];
                conv_state[l][e][K_ - 1] = xz[e];
                double acc = w->conv_b[l][e];
                for (int k = 0; k < K_; k++) acc += (double)w->conv_w[l][e][k] * conv_state[l][e][k];
                xc[e] = ref_silu(acc);
            }

            /* x_proj -> dt_in, B, C */
            double xdb[XDB_];
            for (int o = 0; o < XDB_; o++) {
                double acc = 0;
                for (int e = 0; e < E_; e++) acc += (double)w->x_w[l][o][e] * xc[e];
                xdb[o] = acc;
            }
            const double* B = xdb + R_;
            const double* C = xdb + R_ + N_;

            /* dt_proj + softplus */
            double dt[E_];
            for (int e = 0; e < E_; e++) {
                double acc = w->dt_b[l][e];
                for (int r = 0; r < R_; r++) acc += (double)w->dt_w[l][e][r] * xdb[r];
                dt[e] = ref_softplus(acc);
            }

            /* selective scan step */
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

            /* out_proj + residual */
            for (int i = 0; i < D_; i++) {
                double acc = 0;
                for (int e = 0; e < E_; e++) acc += (double)w->out_w[l][i][e] * y[e];
                x[i] += acc;
            }
        }

        /* final norm + tied head */
        double ss = 0;
        for (int i = 0; i < D_; i++) ss += x[i] * x[i];
        ss = 1.0 / sqrt(ss / D_ + 1e-5);
        double xf[D_];
        for (int i = 0; i < D_; i++) xf[i] = x[i] * ss * w->norm_f[i];
        for (int v = 0; v < V_; v++) {
            double acc = 0;
            for (int i = 0; i < D_; i++) acc += (double)w->emb[v][i] * xf[i];
            logits[t][v] = acc;
        }
    }
}

/* ---- safetensors writer (F32, real data) ---- */

typedef struct { char name[128]; int shape[4]; int ndim; const float* data; size_t numel; } st_entry;

static void st_write(const char* path, const st_entry* ents, int n_ents) {
    char json[16384];
    size_t j = 0;
    uint64_t off = 0;
    j += snprintf(json + j, sizeof(json) - j, "{");
    for (int i = 0; i < n_ents; i++) {
        uint64_t sz = ents[i].numel * 4;
        j += snprintf(json + j, sizeof(json) - j, "%s\"%s\":{\"dtype\":\"F32\",\"shape\":[",
                      i ? "," : "", ents[i].name);
        for (int d = 0; d < ents[i].ndim; d++)
            j += snprintf(json + j, sizeof(json) - j, "%s%d", d ? "," : "", ents[i].shape[d]);
        j += snprintf(json + j, sizeof(json) - j, "],\"data_offsets\":[%llu,%llu]}",
                      (unsigned long long)off, (unsigned long long)(off + sz));
        off += sz;
    }
    j += snprintf(json + j, sizeof(json) - j, "}");

    FILE* f = fopen(path, "wb");
    uint64_t hlen = (uint64_t)j;
    fwrite(&hlen, 8, 1, f);
    fwrite(json, 1, j, f);
    for (int i = 0; i < n_ents; i++) fwrite(ents[i].data, 4, ents[i].numel, f);
    fclose(f);
}

static int build_st_entries(const ssm_weights* w, st_entry* e, int skip_a_log) {
    int n = 0;
#define ADD(nm, d0, d1, d2, nd, ptr, cnt) do { \
        snprintf(e[n].name, sizeof(e[n].name), "%s", nm); \
        e[n].shape[0] = d0; e[n].shape[1] = d1; e[n].shape[2] = d2; \
        e[n].ndim = nd; e[n].data = ptr; e[n].numel = cnt; n++; } while (0)
    ADD("backbone.embedding.weight", V_, D_, 0, 2, &w->emb[0][0], (size_t)V_ * D_);
    ADD("backbone.norm_f.weight", D_, 0, 0, 1, w->norm_f, D_);
    char nm[128];
    for (int l = 0; l < L_; l++) {
        snprintf(nm, sizeof(nm), "backbone.layers.%d.norm.weight", l);
        ADD(nm, D_, 0, 0, 1, w->norm[l], D_);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.in_proj.weight", l);
        ADD(nm, 2 * E_, D_, 0, 2, &w->in_w[l][0][0], (size_t)2 * E_ * D_);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.conv1d.weight", l);
        ADD(nm, E_, 1, K_, 3, &w->conv_w[l][0][0], (size_t)E_ * K_);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.conv1d.bias", l);
        ADD(nm, E_, 0, 0, 1, w->conv_b[l], E_);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.x_proj.weight", l);
        ADD(nm, XDB_, E_, 0, 2, &w->x_w[l][0][0], (size_t)XDB_ * E_);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.dt_proj.weight", l);
        ADD(nm, E_, R_, 0, 2, &w->dt_w[l][0][0], (size_t)E_ * R_);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.dt_proj.bias", l);
        ADD(nm, E_, 0, 0, 1, w->dt_b[l], E_);
        if (!skip_a_log) {
            snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.A_log", l);
            ADD(nm, E_, N_, 0, 2, &w->a_log[l][0][0], (size_t)E_ * N_);
        }
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.D", l);
        ADD(nm, E_, 0, 0, 1, w->dvec[l], E_);
        snprintf(nm, sizeof(nm), "backbone.layers.%d.mixer.out_proj.weight", l);
        ADD(nm, D_, E_, 0, 2, &w->out_w[l][0][0], (size_t)D_ * E_);
    }
#undef ADD
    return n;
}

/* ---- gguf writer (F32, real data, llama.cpp mamba naming) ---- */

static void gg_u32(FILE* f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void gg_u64(FILE* f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void gg_str(FILE* f, const char* s) { gg_u64(f, strlen(s)); fwrite(s, 1, strlen(s), f); }

static void gguf_write(const char* path, const st_entry* ents, int n_ents) {
    FILE* f = fopen(path, "wb");
    fwrite("GGUF", 1, 4, f);
    gg_u32(f, 3);
    gg_u64(f, (uint64_t)n_ents);
    gg_u64(f, 2);
    /* kvs */
    gg_str(f, "general.architecture"); gg_u32(f, 8); gg_str(f, "mamba");
    gg_str(f, "mamba.block_count");    gg_u32(f, 4); gg_u32(f, L_);
    /* tensor table. GGUF records dims in ggml ne order (innermost first) —
       the REVERSE of torch — with identical row-major bytes; verified against
       the real gguf in Models/. Emit reversed so the fixture matches reality. */
    uint64_t off = 0;
    for (int i = 0; i < n_ents; i++) {
        gg_str(f, ents[i].name);
        gg_u32(f, (uint32_t)ents[i].ndim);
        for (int d = ents[i].ndim - 1; d >= 0; d--) gg_u64(f, (uint64_t)ents[i].shape[d]);
        gg_u32(f, 0 /* F32 */);
        gg_u64(f, off);
        off += ents[i].numel * 4;
    }
    for (int i = 0; i < n_ents; i++) fwrite(ents[i].data, 4, ents[i].numel, f);
    fclose(f);
}

static int build_gguf_entries(const ssm_weights* w, st_entry* e) {
    int n = 0;
#define ADD(nm, d0, d1, d2, nd, ptr, cnt) do { \
        snprintf(e[n].name, sizeof(e[n].name), "%s", nm); \
        e[n].shape[0] = d0; e[n].shape[1] = d1; e[n].shape[2] = d2; \
        e[n].ndim = nd; e[n].data = ptr; e[n].numel = cnt; n++; } while (0)
    ADD("token_embd.weight", V_, D_, 0, 2, &w->emb[0][0], (size_t)V_ * D_);
    ADD("output_norm.weight", D_, 0, 0, 1, w->norm_f, D_);
    char nm[128];
    for (int l = 0; l < L_; l++) {
        snprintf(nm, sizeof(nm), "blk.%d.attn_norm.weight", l);   ADD(nm, D_, 0, 0, 1, w->norm[l], D_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_in.weight", l);      ADD(nm, 2 * E_, D_, 0, 2, &w->in_w[l][0][0], (size_t)2 * E_ * D_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_conv1d.weight", l);  ADD(nm, E_, K_, 0, 2, &w->conv_w[l][0][0], (size_t)E_ * K_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_conv1d.bias", l);    ADD(nm, E_, 0, 0, 1, w->conv_b[l], E_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_x.weight", l);       ADD(nm, XDB_, E_, 0, 2, &w->x_w[l][0][0], (size_t)XDB_ * E_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_dt.weight", l);      ADD(nm, E_, R_, 0, 2, &w->dt_w[l][0][0], (size_t)E_ * R_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_dt.bias", l);        ADD(nm, E_, 0, 0, 1, w->dt_b[l], E_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_a", l);              ADD(nm, E_, N_, 0, 2, &w->a_log[l][0][0], (size_t)E_ * N_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_d", l);              ADD(nm, E_, 0, 0, 1, w->dvec[l], E_);
        snprintf(nm, sizeof(nm), "blk.%d.ssm_out.weight", l);     ADD(nm, D_, E_, 0, 2, &w->out_w[l][0][0], (size_t)D_ * E_);
    }
#undef ADD
    return n;
}

/* ---- test ---- */

int main(void) {
    printf("=== cce_ssm: mamba-1 runner, born exact ===\n");

    ssm_weights* w = (ssm_weights*)malloc(sizeof(ssm_weights));
    gen_weights(w);

    static const int tokens[6] = { 3, 7, 1, 11, 0, 5 };
    const int NT = 6;

    /* reference */
    double ref[6][V_];
    ref_forward(w, tokens, NT, ref);

    /* write both containers */
    st_entry ents[64];
    int n_st = build_st_entries(w, ents, 0);
    st_write("ssm_test.safetensors", ents, n_st);
    int n_gg = build_gguf_entries(w, ents);
    gguf_write("ssm_test.gguf", ents, n_gg);

    /* 1. load from safetensors, check derived dims */
    cce_ssm_model* m = NULL;
    CHECK(cce_ssm_load(&m, "ssm_test.safetensors") == CCE_OK && m, "st mamba loads");
    if (!m) { printf("cannot continue\n"); return 1; }
    CHECK(m->n_layer == L_ && m->d_model == D_ && m->d_inner == E_ &&
          m->d_state == N_ && m->d_conv == K_ && m->dt_rank == R_ && m->vocab_size == V_,
          "all dims derived from tensor shapes (no config needed)");
    CHECK(m->forest && m->head_cas, "linear projections decomposed into forest specialists");

    /* 2. per-token logits vs the independent reference.
       Module runs float32, reference float64: gate with allclose-style mixed
       tolerance |diff| <= atol + rtol*|ref| (float32 accumulation noise). */
    float st_logits[6][V_];
    double worst = 0, worst_abs = 0;
    for (int t = 0; t < NT; t++) {
        CHECK(cce_ssm_forward(m, &tokens[t], 1, st_logits[t], V_) == CCE_OK, "forward ok");
        for (int v = 0; v < V_; v++) {
            double diff = fabs((double)st_logits[t][v] - ref[t][v]);
            double ratio = diff / (1e-4 + 5e-4 * fabs(ref[t][v]));
            if (ratio > worst) worst = ratio;
            if (diff > worst_abs) worst_abs = diff;
        }
    }
    printf("  vs double reference: max |diff| %.3g, worst tolerance ratio %.3g\n", worst_abs, worst);
    CHECK(worst < 1.0, "matches independent double-precision reference (atol 1e-4, rtol 5e-4)");

    /* 3. reset reproduces exactly */
    cce_ssm_reset(m);
    CHECK(m->cur_pos == 0, "reset clears position");
    float replay[V_];
    for (int t = 0; t < NT; t++) cce_ssm_forward(m, &tokens[t], 1, replay, V_);
    float d0 = 0;
    for (int v = 0; v < V_; v++) { float d = fabsf(replay[v] - st_logits[NT - 1][v]); if (d > d0) d0 = d; }
    CHECK(d0 == 0.0f, "reset + replay is bit-identical");

    /* 4. batch == incremental */
    cce_ssm_reset(m);
    float batch[V_];
    CHECK(cce_ssm_forward(m, tokens, NT, batch, V_) == CCE_OK, "batch forward ok");
    d0 = 0;
    for (int v = 0; v < V_; v++) { float d = fabsf(batch[v] - st_logits[NT - 1][v]); if (d > d0) d0 = d; }
    CHECK(d0 == 0.0f, "one batch call == token-by-token calls (state discipline)");
    cce_ssm_free(m); m = NULL;

    /* 5. gguf mapping equivalence: same weights, same logits, bit for bit */
    CHECK(cce_ssm_load(&m, "ssm_test.gguf") == CCE_OK && m, "gguf mamba loads");
    if (m) {
        float gg_logits[V_];
        for (int t = 0; t < NT; t++) cce_ssm_forward(m, &tokens[t], 1, gg_logits, V_);
        d0 = 0;
        for (int v = 0; v < V_; v++) { float d = fabsf(gg_logits[v] - st_logits[NT - 1][v]); if (d > d0) d0 = d; }
        CHECK(d0 == 0.0f, "gguf and safetensors mappings are bit-identical");
        cce_ssm_free(m); m = NULL;
    }

    /* 6. universal entry: detect + anymodel dispatch */
    {
        cce_model_info info;
        CHECK(cce_detect_file("ssm_test.safetensors", &info) == CCE_OK &&
              info.family == CCE_ARCH_FAMILY_MAMBA && info.runnable == 1 &&
              strcmp(info.runner, "cce_ssm_load") == 0,
              "detect: st mamba runnable via cce_ssm_load");
        CHECK(cce_detect_file("ssm_test.gguf", &info) == CCE_OK &&
              info.family == CCE_ARCH_FAMILY_MAMBA && info.runnable == 1 &&
              strcmp(info.runner, "cce_ssm_load") == 0,
              "detect: gguf mamba runnable via cce_ssm_load");

        cce_anymodel* am = NULL;
        CHECK(cce_anymodel_open(&am, "ssm_test.gguf") == CCE_OK && am && am->ssm,
              "anymodel autoloads a mamba gguf into the ssm runner");
        if (am) {
            float lg[V_];
            CHECK(cce_ssm_forward(am->ssm, tokens, NT, lg, V_) == CCE_OK, "anymodel ssm forward runs");
            cce_anymodel_free(am);
        }
    }

    /* 7. honest refusals */
    {
        cce_gguf_qwen2* q = NULL;
        CHECK(cce_gguf_load_model(&q, "ssm_test.gguf") == CCE_ERR_UNSUPPORTED && q == NULL,
              "transformer gguf entry refuses ssm gguf instead of mis-running it");

        int n_bad = build_st_entries(w, ents, 1 /* skip A_log */);
        st_write("ssm_bad.safetensors", ents, n_bad);
        cce_ssm_model* bad = NULL;
        CHECK(cce_ssm_load(&bad, "ssm_bad.safetensors") != CCE_OK && bad == NULL,
              "incomplete mamba file (no A_log) refuses cleanly");
        remove("ssm_bad.safetensors");
    }

    remove("ssm_test.safetensors");
    remove("ssm_test.gguf");
    free(w);

    printf("\n%d checks, %d failed -> %s\n", checks, fails, fails ? "FAIL" : "OK");
    return fails ? 1 : 0;
}
