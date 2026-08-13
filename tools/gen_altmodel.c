/* Generate DIFFERENT-config transformers for cce_transformer_qat load+parity
 * gates (altmodel_cache / gpt2 variants). Deterministic random-ish F32 weights
 * in safetensors format; gpt2 schema embeds golden.ids / golden.logits from an
 * independent float64 reference forward.
 *
 * Usage: gen_altmodel [out_dir] [supra|gpt2|gpt2_nohead|gpt2_gap]
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

/* ---- deterministic RNG (xorshift64* + Box-Muller) ---- */
typedef struct { uint64_t s; int has_spare; double spare; } Rng;

static void rng_seed(Rng *r, uint64_t seed) {
    r->s = seed ? seed : 0x9e3779b97f4a7c15ULL;
    r->has_spare = 0;
}

static uint64_t rng_u64(Rng *r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * 0x2545F4914F6CDD1DULL;
}

static double rng_uniform(Rng *r) {
    return (rng_u64(r) >> 11) * (1.0 / 9007199254740992.0);
}

static double rng_gauss(Rng *r) {
    double u, v, s;
    if (r->has_spare) {
        r->has_spare = 0;
        return r->spare;
    }
    do {
        u = rng_uniform(r) * 2.0 - 1.0;
        v = rng_uniform(r) * 2.0 - 1.0;
        s = u * u + v * v;
    } while (s >= 1.0 || s == 0.0);
    s = sqrt(-2.0 * log(s) / s);
    r->spare = v * s;
    r->has_spare = 1;
    return u * s;
}

static float *alloc_f(size_t n) {
    float *p = (float *)calloc(n, sizeof(float));
    if (!p) { fprintf(stderr, "oom\n"); exit(1); }
    return p;
}

static void fill_w(Rng *r, float *p, size_t n, float scale) {
    size_t i;
    for (i = 0; i < n; i++) p[i] = (float)(rng_gauss(r) * scale);
}

static void fill_z(float *p, size_t n) {
    memset(p, 0, n * sizeof(float));
}

static void fill_one(float *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) p[i] = 1.0f;
}

static void fill_ln(Rng *r, float *p, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) p[i] = (float)(1.0 + rng_gauss(r) * 0.02);
}

/* ---- safetensors writer ---- */
typedef struct {
    char name[128];
    int shape[4];
    int ndim;
    const float *data;
    size_t numel;
} StEnt;

typedef struct {
    char key[64];
    char val[64];
} MetaKV;

static int ensure_dir(const char *path) {
    return MKDIR(path);
}

static int st_write(const char *path, StEnt *ents, int n_ents,
                    MetaKV *meta, int n_meta) {
    char *json;
    size_t jcap = 256 * 1024, jlen = 0;
    uint64_t off = 0;
    int i, d;
    FILE *f;

    json = (char *)malloc(jcap);
    if (!json) return -1;
    jlen += (size_t)snprintf(json + jlen, jcap - jlen, "{");
    if (n_meta > 0) {
        jlen += (size_t)snprintf(json + jlen, jcap - jlen, "\"__metadata__\":{");
        for (i = 0; i < n_meta; i++) {
            jlen += (size_t)snprintf(json + jlen, jcap - jlen, "%s\"%s\":\"%s\"",
                                     i ? "," : "", meta[i].key, meta[i].val);
        }
        jlen += (size_t)snprintf(json + jlen, jcap - jlen, "},");
    }
    for (i = 0; i < n_ents; i++) {
        uint64_t sz = (uint64_t)ents[i].numel * 4ull;
        jlen += (size_t)snprintf(json + jlen, jcap - jlen,
                                 "%s\"%s\":{\"dtype\":\"F32\",\"shape\":[",
                                 i ? "," : "", ents[i].name);
        for (d = 0; d < ents[i].ndim; d++)
            jlen += (size_t)snprintf(json + jlen, jcap - jlen, "%s%d",
                                     d ? "," : "", ents[i].shape[d]);
        jlen += (size_t)snprintf(json + jlen, jcap - jlen,
                                 "],\"data_offsets\":[%llu,%llu]}",
                                 (unsigned long long)off,
                                 (unsigned long long)(off + sz));
        off += sz;
        if (jlen + 256 > jcap) {
            jcap *= 2;
            json = (char *)realloc(json, jcap);
            if (!json) return -1;
        }
    }
    jlen += (size_t)snprintf(json + jlen, jcap - jlen, "}");

    f = fopen(path, "wb");
    if (!f) { free(json); return -1; }
    {
        uint64_t hlen = (uint64_t)jlen;
        fwrite(&hlen, 8, 1, f);
        fwrite(json, 1, jlen, f);
    }
    for (i = 0; i < n_ents; i++)
        fwrite(ents[i].data, 4, ents[i].numel, f);
    fclose(f);
    free(json);
    return 0;
}

static void add_ent(StEnt *e, int *n, const char *name,
                    int d0, int d1, int ndim, const float *data, size_t numel) {
    StEnt *x = &e[(*n)++];
    snprintf(x->name, sizeof x->name, "%s", name);
    x->shape[0] = d0; x->shape[1] = d1; x->shape[2] = 0; x->shape[3] = 0;
    x->ndim = ndim; x->data = data; x->numel = numel;
}

/* ---- gpt2 reference forward (float64) ---- */
static void layer_norm(const double *x, int T, int D,
                       const float *g, const float *bb, double *y) {
    int t, d;
    for (t = 0; t < T; t++) {
        double mu = 0, var = 0;
        for (d = 0; d < D; d++) mu += x[t * D + d];
        mu /= D;
        for (d = 0; d < D; d++) {
            double v = x[t * D + d] - mu;
            var += v * v;
        }
        var /= D;
        {
            double inv = 1.0 / sqrt(var + 1e-5);
            for (d = 0; d < D; d++)
                y[t * D + d] = (x[t * D + d] - mu) * inv * g[d] + bb[d];
        }
    }
}

static void gelu_inplace(double *x, int n) {
    int i;
    for (i = 0; i < n; i++) {
        double v = x[i];
        x[i] = 0.5 * v * (1.0 + tanh(0.79788456 * (v + 0.044715 * v * v * v)));
    }
}

/* Matmul: A[T,K] @ B[K,N] -> C[T,N]  (Conv1D / GPT-2 layout) */
static void matmul_tk_kn(const double *A, const float *B, double *C,
                         int T, int K, int N) {
    int t, n, k;
    for (t = 0; t < T; t++) {
        for (n = 0; n < N; n++) {
            double s = 0;
            for (k = 0; k < K; k++)
                s += A[t * K + k] * (double)B[k * N + n];
            C[t * N + n] = s;
        }
    }
}

static void add_bias(double *X, const float *b, int T, int N) {
    int t, n;
    for (t = 0; t < T; t++)
        for (n = 0; n < N; n++)
            X[t * N + n] += b[n];
}

static void forward_last_logits(
    int L, int D, int H, int V, int M,
    float *wte, float *wpe, float *ln_f_w, float *ln_f_b,
    float **ln1_w, float **ln1_b, float **ln2_w, float **ln2_b,
    float **c_attn_w, float **c_attn_b,
    float **c_proj_w, float **c_proj_b,
    float **c_fc_w, float **c_fc_b,
    float **c_proj_mlp_w, float **c_proj_mlp_b,
    const int *ids, int T, double *logits_out)
{
    int t, l, h, i, j;
    int hd = D / H;
    double *x, *tmp, *qkv, *attn_cat, *mid, *scores, *probs;

    x = (double *)calloc((size_t)T * D, sizeof(double));
    tmp = (double *)calloc((size_t)T * D, sizeof(double));
    qkv = (double *)calloc((size_t)T * 3 * D, sizeof(double));
    attn_cat = (double *)calloc((size_t)T * D, sizeof(double));
    mid = (double *)calloc((size_t)T * M, sizeof(double));
    scores = (double *)calloc((size_t)T * T, sizeof(double));
    probs = (double *)calloc((size_t)T * T, sizeof(double));
    if (!x || !tmp || !qkv || !attn_cat || !mid || !scores || !probs) {
        fprintf(stderr, "oom forward\n");
        exit(1);
    }

    for (t = 0; t < T; t++) {
        int id = ids[t];
        for (i = 0; i < D; i++)
            x[t * D + i] = (double)wte[id * D + i] + (double)wpe[t * D + i];
    }

    for (l = 0; l < L; l++) {
        layer_norm(x, T, D, ln1_w[l], ln1_b[l], tmp);
        matmul_tk_kn(tmp, c_attn_w[l], qkv, T, D, 3 * D);
        add_bias(qkv, c_attn_b[l], T, 3 * D);

        memset(attn_cat, 0, (size_t)T * D * sizeof(double));
        for (h = 0; h < H; h++) {
            for (t = 0; t < T; t++) {
                for (j = 0; j < T; j++) {
                    double s = 0;
                    for (i = 0; i < hd; i++) {
                        double q = qkv[t * 3 * D + h * hd + i];
                        double k = qkv[j * 3 * D + D + h * hd + i];
                        s += q * k;
                    }
                    s /= sqrt((double)hd);
                    if (j > t) s = -1e9;
                    scores[t * T + j] = s;
                }
            }
            for (t = 0; t < T; t++) {
                double mx = scores[t * T], den = 0;
                for (j = 1; j < T; j++)
                    if (scores[t * T + j] > mx) mx = scores[t * T + j];
                for (j = 0; j < T; j++) {
                    double e = exp(scores[t * T + j] - mx);
                    probs[t * T + j] = e;
                    den += e;
                }
                for (j = 0; j < T; j++) probs[t * T + j] /= den;
            }
            for (t = 0; t < T; t++) {
                for (i = 0; i < hd; i++) {
                    double s = 0;
                    for (j = 0; j < T; j++) {
                        double v = qkv[j * 3 * D + 2 * D + h * hd + i];
                        s += probs[t * T + j] * v;
                    }
                    attn_cat[t * D + h * hd + i] = s;
                }
            }
        }
        matmul_tk_kn(attn_cat, c_proj_w[l], tmp, T, D, D);
        add_bias(tmp, c_proj_b[l], T, D);
        for (t = 0; t < T * D; t++) x[t] += tmp[t];

        layer_norm(x, T, D, ln2_w[l], ln2_b[l], tmp);
        matmul_tk_kn(tmp, c_fc_w[l], mid, T, D, M);
        add_bias(mid, c_fc_b[l], T, M);
        gelu_inplace(mid, T * M);
        matmul_tk_kn(mid, c_proj_mlp_w[l], tmp, T, M, D);
        add_bias(tmp, c_proj_mlp_b[l], T, D);
        for (t = 0; t < T * D; t++) x[t] += tmp[t];
    }

    layer_norm(x, T, D, ln_f_w, ln_f_b, tmp);
    {
        const double *hvec = tmp + (T - 1) * D;
        for (i = 0; i < V; i++) {
            double s = 0;
            for (j = 0; j < D; j++)
                s += hvec[j] * (double)wte[i * D + j];
            logits_out[i] = s;
        }
    }

    free(x); free(tmp); free(qkv); free(attn_cat); free(mid); free(scores); free(probs);
}

static int write_supra(const char *path) {
    const int L = 4, D = 128, H = 4, V = 2000, B = 96, M = 512;
    Rng rng;
    StEnt *ents;
    int n = 0, l;
    float *tok, *pos, *lnf_w, *lnf_b, *head;
    float *ln1_w[4], *ln1_b[4], *ln2_w[4], *ln2_b[4];
    float *qkv_w[4], *qkv_b[4], *proj_w[4], *proj_b[4];
    float *mlp0_w[4], *mlp0_b[4], *mlp2_w[4], *mlp2_b[4];
    char name[128];

    rng_seed(&rng, 1234);
    ents = (StEnt *)calloc(256, sizeof(StEnt));
    tok = alloc_f((size_t)V * D); fill_w(&rng, tok, (size_t)V * D, 0.02f);
    pos = alloc_f((size_t)B * D); fill_w(&rng, pos, (size_t)B * D, 0.02f);
    lnf_w = alloc_f(D); fill_one(lnf_w, D);
    lnf_b = alloc_f(D); fill_z(lnf_b, D);
    head = alloc_f((size_t)V * D); fill_w(&rng, head, (size_t)V * D, 0.02f);

    add_ent(ents, &n, "tok_emb.weight", V, D, 2, tok, (size_t)V * D);
    add_ent(ents, &n, "pos_emb.weight", B, D, 2, pos, (size_t)B * D);
    add_ent(ents, &n, "ln_f.weight", D, 0, 1, lnf_w, D);
    add_ent(ents, &n, "ln_f.bias", D, 0, 1, lnf_b, D);
    add_ent(ents, &n, "head.weight", V, D, 2, head, (size_t)V * D);

    for (l = 0; l < L; l++) {
        ln1_w[l] = alloc_f(D); fill_one(ln1_w[l], D);
        ln1_b[l] = alloc_f(D); fill_z(ln1_b[l], D);
        ln2_w[l] = alloc_f(D); fill_one(ln2_w[l], D);
        ln2_b[l] = alloc_f(D); fill_z(ln2_b[l], D);
        qkv_w[l] = alloc_f((size_t)3 * D * D); fill_w(&rng, qkv_w[l], (size_t)3 * D * D, 0.02f);
        qkv_b[l] = alloc_f(3 * D); fill_z(qkv_b[l], 3 * D);
        proj_w[l] = alloc_f((size_t)D * D); fill_w(&rng, proj_w[l], (size_t)D * D, 0.02f);
        proj_b[l] = alloc_f(D); fill_z(proj_b[l], D);
        mlp0_w[l] = alloc_f((size_t)M * D); fill_w(&rng, mlp0_w[l], (size_t)M * D, 0.02f);
        mlp0_b[l] = alloc_f(M); fill_z(mlp0_b[l], M);
        mlp2_w[l] = alloc_f((size_t)D * M); fill_w(&rng, mlp2_w[l], (size_t)D * M, 0.02f);
        mlp2_b[l] = alloc_f(D); fill_z(mlp2_b[l], D);

        snprintf(name, sizeof name, "blocks.%d.ln1.weight", l);
        add_ent(ents, &n, name, D, 0, 1, ln1_w[l], D);
        snprintf(name, sizeof name, "blocks.%d.ln1.bias", l);
        add_ent(ents, &n, name, D, 0, 1, ln1_b[l], D);
        snprintf(name, sizeof name, "blocks.%d.ln2.weight", l);
        add_ent(ents, &n, name, D, 0, 1, ln2_w[l], D);
        snprintf(name, sizeof name, "blocks.%d.ln2.bias", l);
        add_ent(ents, &n, name, D, 0, 1, ln2_b[l], D);
        snprintf(name, sizeof name, "blocks.%d.attn.qkv.weight", l);
        add_ent(ents, &n, name, 3 * D, D, 2, qkv_w[l], (size_t)3 * D * D);
        snprintf(name, sizeof name, "blocks.%d.attn.qkv.bias", l);
        add_ent(ents, &n, name, 3 * D, 0, 1, qkv_b[l], 3 * D);
        snprintf(name, sizeof name, "blocks.%d.attn.proj.weight", l);
        add_ent(ents, &n, name, D, D, 2, proj_w[l], (size_t)D * D);
        snprintf(name, sizeof name, "blocks.%d.attn.proj.bias", l);
        add_ent(ents, &n, name, D, 0, 1, proj_b[l], D);
        snprintf(name, sizeof name, "blocks.%d.mlp.0.weight", l);
        add_ent(ents, &n, name, M, D, 2, mlp0_w[l], (size_t)M * D);
        snprintf(name, sizeof name, "blocks.%d.mlp.0.bias", l);
        add_ent(ents, &n, name, M, 0, 1, mlp0_b[l], M);
        snprintf(name, sizeof name, "blocks.%d.mlp.2.weight", l);
        add_ent(ents, &n, name, D, M, 2, mlp2_w[l], (size_t)D * M);
        snprintf(name, sizeof name, "blocks.%d.mlp.2.bias", l);
        add_ent(ents, &n, name, D, 0, 1, mlp2_b[l], D);
    }

    if (st_write(path, ents, n, NULL, 0) != 0) {
        fprintf(stderr, "write failed %s\n", path);
        return 1;
    }
    printf("wrote %s  config L%d D%d H%d V%d B%d M%d  (%d tensors)\n",
           path, L, D, H, V, B, M, n);
    printf("GEN_ALTMODEL_OK schema=supra\n");
    free(ents);
    (void)H;
    return 0;
}

static int write_corrupt(const char *path, const char *schema) {
    const int D = 32, V = 100, B = 16, M = 64;
    Rng rng;
    StEnt *ents;
    int n = 0, li, nlayers;
    int layers[4];
    MetaKV meta[1];
    int n_meta = 0;
    float *wte, *wpe, *lnf_w, *lnf_b;
    char name[128];

    rng_seed(&rng, 99);
    ents = (StEnt *)calloc(128, sizeof(StEnt));
    if (strcmp(schema, "gpt2_nohead") == 0) {
        layers[0] = 0; layers[1] = 1; nlayers = 2;
        n_meta = 0;
    } else {
        layers[0] = 0; layers[1] = 1; layers[2] = 3; nlayers = 3;
        snprintf(meta[0].key, sizeof meta[0].key, "n_head");
        snprintf(meta[0].val, sizeof meta[0].val, "2");
        n_meta = 1;
    }

    wte = alloc_f((size_t)V * D); fill_w(&rng, wte, (size_t)V * D, 0.02f);
    wpe = alloc_f((size_t)B * D); fill_w(&rng, wpe, (size_t)B * D, 0.02f);
    lnf_w = alloc_f(D); fill_w(&rng, lnf_w, D, 0.02f);
    lnf_b = alloc_f(D); fill_w(&rng, lnf_b, D, 0.02f);
    add_ent(ents, &n, "wte.weight", V, D, 2, wte, (size_t)V * D);
    add_ent(ents, &n, "wpe.weight", B, D, 2, wpe, (size_t)B * D);
    add_ent(ents, &n, "ln_f.weight", D, 0, 1, lnf_w, D);
    add_ent(ents, &n, "ln_f.bias", D, 0, 1, lnf_b, D);

    for (li = 0; li < nlayers; li++) {
        int l = layers[li];
        float *a, *b;
        a = alloc_f(D); fill_w(&rng, a, D, 0.02f);
        b = alloc_f(D); fill_w(&rng, b, D, 0.02f);
        snprintf(name, sizeof name, "h.%d.ln_1.weight", l);
        add_ent(ents, &n, name, D, 0, 1, a, D);
        snprintf(name, sizeof name, "h.%d.ln_1.bias", l);
        add_ent(ents, &n, name, D, 0, 1, b, D);
        a = alloc_f(D); fill_w(&rng, a, D, 0.02f);
        b = alloc_f(D); fill_w(&rng, b, D, 0.02f);
        snprintf(name, sizeof name, "h.%d.ln_2.weight", l);
        add_ent(ents, &n, name, D, 0, 1, a, D);
        snprintf(name, sizeof name, "h.%d.ln_2.bias", l);
        add_ent(ents, &n, name, D, 0, 1, b, D);

        a = alloc_f((size_t)D * 3 * D); fill_w(&rng, a, (size_t)D * 3 * D, 0.02f);
        b = alloc_f(3 * D); fill_w(&rng, b, 3 * D, 0.02f);
        snprintf(name, sizeof name, "h.%d.attn.c_attn.weight", l);
        add_ent(ents, &n, name, D, 3 * D, 2, a, (size_t)D * 3 * D);
        snprintf(name, sizeof name, "h.%d.attn.c_attn.bias", l);
        add_ent(ents, &n, name, 3 * D, 0, 1, b, 3 * D);

        a = alloc_f((size_t)D * D); fill_w(&rng, a, (size_t)D * D, 0.02f);
        b = alloc_f(D); fill_w(&rng, b, D, 0.02f);
        snprintf(name, sizeof name, "h.%d.attn.c_proj.weight", l);
        add_ent(ents, &n, name, D, D, 2, a, (size_t)D * D);
        snprintf(name, sizeof name, "h.%d.attn.c_proj.bias", l);
        add_ent(ents, &n, name, D, 0, 1, b, D);

        a = alloc_f((size_t)D * M); fill_w(&rng, a, (size_t)D * M, 0.02f);
        b = alloc_f(M); fill_w(&rng, b, M, 0.02f);
        snprintf(name, sizeof name, "h.%d.mlp.c_fc.weight", l);
        add_ent(ents, &n, name, D, M, 2, a, (size_t)D * M);
        snprintf(name, sizeof name, "h.%d.mlp.c_fc.bias", l);
        add_ent(ents, &n, name, M, 0, 1, b, M);

        a = alloc_f((size_t)M * D); fill_w(&rng, a, (size_t)M * D, 0.02f);
        b = alloc_f(D); fill_w(&rng, b, D, 0.02f);
        snprintf(name, sizeof name, "h.%d.mlp.c_proj.weight", l);
        add_ent(ents, &n, name, M, D, 2, a, (size_t)M * D);
        snprintf(name, sizeof name, "h.%d.mlp.c_proj.bias", l);
        add_ent(ents, &n, name, D, 0, 1, b, D);
    }

    if (st_write(path, ents, n, meta, n_meta) != 0) {
        fprintf(stderr, "write failed %s\n", path);
        return 1;
    }
    printf("wrote %s  CORRUPT variant '%s' (%d tensors)\n", path, schema, n);
    printf("GEN_ALTMODEL_OK schema=%s\n", schema);
    free(ents);
    return 0;
}

static int write_gpt2(const char *path) {
    const int L = 6, D = 128, H = 2, V = 2000, B = 96, M = 512;
    const int NSEQ = 8, TSEQ = 8;
    Rng rng;
    StEnt *ents;
    int n = 0, l, s, t, i;
    MetaKV meta[3];
    float *wte, *wpe, *lnf_w, *lnf_b;
    float *ln1_w[6], *ln1_b[6], *ln2_w[6], *ln2_b[6];
    float *c_attn_w[6], *c_attn_b[6], *c_proj_w[6], *c_proj_b[6];
    float *c_fc_w[6], *c_fc_b[6], *c_proj_mlp_w[6], *c_proj_mlp_b[6];
    float *golden_ids, *golden_logits;
    int ids[8][8];
    char name[128];

    rng_seed(&rng, 4321);
    ents = (StEnt *)calloc(256, sizeof(StEnt));
    wte = alloc_f((size_t)V * D); fill_w(&rng, wte, (size_t)V * D, 0.02f);
    wpe = alloc_f((size_t)B * D); fill_w(&rng, wpe, (size_t)B * D, 0.02f);
    lnf_w = alloc_f(D); fill_ln(&rng, lnf_w, D);
    lnf_b = alloc_f(D); fill_w(&rng, lnf_b, D, 0.01f);
    add_ent(ents, &n, "wte.weight", V, D, 2, wte, (size_t)V * D);
    add_ent(ents, &n, "wpe.weight", B, D, 2, wpe, (size_t)B * D);
    add_ent(ents, &n, "ln_f.weight", D, 0, 1, lnf_w, D);
    add_ent(ents, &n, "ln_f.bias", D, 0, 1, lnf_b, D);

    for (l = 0; l < L; l++) {
        ln1_w[l] = alloc_f(D); fill_ln(&rng, ln1_w[l], D);
        ln1_b[l] = alloc_f(D); fill_w(&rng, ln1_b[l], D, 0.01f);
        ln2_w[l] = alloc_f(D); fill_ln(&rng, ln2_w[l], D);
        ln2_b[l] = alloc_f(D); fill_w(&rng, ln2_b[l], D, 0.01f);
        c_attn_w[l] = alloc_f((size_t)D * 3 * D); fill_w(&rng, c_attn_w[l], (size_t)D * 3 * D, 0.02f);
        c_attn_b[l] = alloc_f(3 * D); fill_w(&rng, c_attn_b[l], 3 * D, 0.01f);
        c_proj_w[l] = alloc_f((size_t)D * D); fill_w(&rng, c_proj_w[l], (size_t)D * D, 0.02f);
        c_proj_b[l] = alloc_f(D); fill_w(&rng, c_proj_b[l], D, 0.01f);
        c_fc_w[l] = alloc_f((size_t)D * M); fill_w(&rng, c_fc_w[l], (size_t)D * M, 0.02f);
        c_fc_b[l] = alloc_f(M); fill_w(&rng, c_fc_b[l], M, 0.01f);
        c_proj_mlp_w[l] = alloc_f((size_t)M * D); fill_w(&rng, c_proj_mlp_w[l], (size_t)M * D, 0.02f);
        c_proj_mlp_b[l] = alloc_f(D); fill_w(&rng, c_proj_mlp_b[l], D, 0.01f);

        snprintf(name, sizeof name, "h.%d.ln_1.weight", l);
        add_ent(ents, &n, name, D, 0, 1, ln1_w[l], D);
        snprintf(name, sizeof name, "h.%d.ln_1.bias", l);
        add_ent(ents, &n, name, D, 0, 1, ln1_b[l], D);
        snprintf(name, sizeof name, "h.%d.ln_2.weight", l);
        add_ent(ents, &n, name, D, 0, 1, ln2_w[l], D);
        snprintf(name, sizeof name, "h.%d.ln_2.bias", l);
        add_ent(ents, &n, name, D, 0, 1, ln2_b[l], D);
        snprintf(name, sizeof name, "h.%d.attn.c_attn.weight", l);
        add_ent(ents, &n, name, D, 3 * D, 2, c_attn_w[l], (size_t)D * 3 * D);
        snprintf(name, sizeof name, "h.%d.attn.c_attn.bias", l);
        add_ent(ents, &n, name, 3 * D, 0, 1, c_attn_b[l], 3 * D);
        snprintf(name, sizeof name, "h.%d.attn.c_proj.weight", l);
        add_ent(ents, &n, name, D, D, 2, c_proj_w[l], (size_t)D * D);
        snprintf(name, sizeof name, "h.%d.attn.c_proj.bias", l);
        add_ent(ents, &n, name, D, 0, 1, c_proj_b[l], D);
        snprintf(name, sizeof name, "h.%d.mlp.c_fc.weight", l);
        add_ent(ents, &n, name, D, M, 2, c_fc_w[l], (size_t)D * M);
        snprintf(name, sizeof name, "h.%d.mlp.c_fc.bias", l);
        add_ent(ents, &n, name, M, 0, 1, c_fc_b[l], M);
        snprintf(name, sizeof name, "h.%d.mlp.c_proj.weight", l);
        add_ent(ents, &n, name, M, D, 2, c_proj_mlp_w[l], (size_t)M * D);
        snprintf(name, sizeof name, "h.%d.mlp.c_proj.bias", l);
        add_ent(ents, &n, name, D, 0, 1, c_proj_mlp_b[l], D);
    }

    golden_ids = alloc_f((size_t)NSEQ * TSEQ);
    golden_logits = alloc_f((size_t)NSEQ * V);
    for (s = 0; s < NSEQ; s++)
        for (t = 0; t < TSEQ; t++) {
            ids[s][t] = (int)(rng_u64(&rng) % (uint64_t)V);
            golden_ids[s * TSEQ + t] = (float)ids[s][t];
        }

    for (s = 0; s < NSEQ; s++) {
        double *logits = (double *)calloc(V, sizeof(double));
        if (!logits) return 1;
        forward_last_logits(L, D, H, V, M,
                            wte, wpe, lnf_w, lnf_b,
                            ln1_w, ln1_b, ln2_w, ln2_b,
                            c_attn_w, c_attn_b, c_proj_w, c_proj_b,
                            c_fc_w, c_fc_b, c_proj_mlp_w, c_proj_mlp_b,
                            ids[s], TSEQ, logits);
        for (i = 0; i < V; i++)
            golden_logits[s * V + i] = (float)logits[i];
        free(logits);
    }
    add_ent(ents, &n, "golden.ids", NSEQ, TSEQ, 2, golden_ids, (size_t)NSEQ * TSEQ);
    add_ent(ents, &n, "golden.logits", NSEQ, V, 2, golden_logits, (size_t)NSEQ * V);

    snprintf(meta[0].key, sizeof meta[0].key, "n_head");
    snprintf(meta[0].val, sizeof meta[0].val, "%d", H);
    snprintf(meta[1].key, sizeof meta[1].key, "n_layer");
    snprintf(meta[1].val, sizeof meta[1].val, "%d", L);
    snprintf(meta[2].key, sizeof meta[2].key, "naming");
    snprintf(meta[2].val, sizeof meta[2].val, "gpt2");

    if (st_write(path, ents, n, meta, 3) != 0) {
        fprintf(stderr, "write failed %s\n", path);
        return 1;
    }
    printf("wrote %s  config L%d D%d H%d V%d B%d M%d  (%d tensors, "
           "gpt2 naming, Conv1D [in,out], tied head, %d golden seqs)\n",
           path, L, D, H, V, B, M, n, NSEQ);
    printf("GEN_ALTMODEL_OK schema=gpt2\n");
    free(ents);
    return 0;
}

int main(int argc, char **argv) {
    const char *out_dir = (argc > 1) ? argv[1] : "altmodel_cache";
    const char *schema = (argc > 2) ? argv[2] : "supra";
    char path[1024];

    ensure_dir(out_dir);
    snprintf(path, sizeof path, "%s/model.safetensors", out_dir);

    if (strcmp(schema, "supra") == 0)
        return write_supra(path);
    if (strcmp(schema, "gpt2_nohead") == 0 || strcmp(schema, "gpt2_gap") == 0)
        return write_corrupt(path, schema);
    if (strcmp(schema, "gpt2") == 0)
        return write_gpt2(path);

    fprintf(stderr, "unknown schema '%s' (want: supra | gpt2 | gpt2_nohead | gpt2_gap)\n",
            schema);
    return 1;
}
