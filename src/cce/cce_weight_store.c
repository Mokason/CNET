/* Content-addressed weight store. See include/cce/cce_weight_store.h.
 *
 * Layout: <dir>/<16-hex-digest>.spec, one payload per digest (git-objects
 * style; the filesystem is the index). Payload:
 *   magic "CSPC" | version u32 | kind u32 (0 cascade, 1 tensor)
 *   cascade: n_blocks i32, then per block: type i32, in i32, out i32,
 *            has_bias i32, weights f32[in*out], bias f32[out] if has_bias
 *   tensor:  ndim i32, shape i32[ndim], data f32[numel]
 */

#include "../../include/cce/cce_weight_store.h"
#include "../../include/cce/cce_specgraph.h"
#include "../../include/cce/cce_forest.h"
#include "../../include/cce/cce_block.h"
#include "../../include/cce/cce_ssm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define WS_MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <sys/stat.h>
#define WS_MKDIR(p) mkdir(p, 0777)
#endif

#define WS_MAGIC "CSPC"
#define WS_VERSION 1

struct cce_weight_store {
    char dir[512];
    int count;
    size_t bytes;
};

/* ---- payload serialization (to memory, so put can byte-verify) ---- */

typedef struct { unsigned char* buf; size_t len, cap; } ws_blob;

static int blob_put(ws_blob* b, const void* p, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = (b->cap ? b->cap * 2 : 4096);
        while (nc < b->len + n) nc *= 2;
        unsigned char* nb = (unsigned char*)realloc(b->buf, nc);
        if (!nb) return 0;
        b->buf = nb; b->cap = nc;
    }
    memcpy(b->buf + b->len, p, n);
    b->len += n;
    return 1;
}

static int blob_i32(ws_blob* b, int v) { return blob_put(b, &v, 4); }

static cce_result serialize_cascade(const cce_cascade* cas, ws_blob* b) {
    uint32_t ver = WS_VERSION, kind = 0;
    if (!blob_put(b, WS_MAGIC, 4) || !blob_put(b, &ver, 4) || !blob_put(b, &kind, 4)) return CCE_ERR_OOM;
    if (!blob_i32(b, cas->num_blocks)) return CCE_ERR_OOM;
    for (int i = 0; i < cas->num_blocks; i++) {
        const cce_block* blk = &cas->blocks[i];
        int in  = blk->weights.ndim >= 2 ? blk->weights.shape[0] : 0;
        int out = blk->weights.ndim >= 2 ? blk->weights.shape[1] : 0;
        int has_bias = (blk->bias.data && blk->bias.numel > 0) ? 1 : 0;
        if (in <= 0 || out <= 0 || !blk->weights.data) return CCE_ERR_UNSUPPORTED;
        if (!blob_i32(b, (int)blk->type) || !blob_i32(b, in) || !blob_i32(b, out) ||
            !blob_i32(b, has_bias)) return CCE_ERR_OOM;
        if (!blob_put(b, blk->weights.data, (size_t)in * out * sizeof(float))) return CCE_ERR_OOM;
        if (has_bias && !blob_put(b, blk->bias.data, blk->bias.numel * sizeof(float))) return CCE_ERR_OOM;
    }
    return CCE_OK;
}

/* Tensor payloads are SHAPE-FREE (numel + bytes): gguf and safetensors record
 * the same bytes under reversed dim orders, and content identity must be
 * bytes-only for cross-container dedup. The shape is model metadata and
 * lives in the manifest row instead. */
static cce_result serialize_tensor(const cce_tensor* t, ws_blob* b) {
    uint32_t ver = WS_VERSION, kind = 1;
    if (!t || !t->data || t->numel == 0) return CCE_ERR_INVALID_ARG;
    if (!blob_put(b, WS_MAGIC, 4) || !blob_put(b, &ver, 4) || !blob_put(b, &kind, 4)) return CCE_ERR_OOM;
    uint64_t numel = (uint64_t)t->numel;
    if (!blob_put(b, &numel, 8)) return CCE_ERR_OOM;
    if (!blob_put(b, t->data, t->numel * sizeof(float))) return CCE_ERR_OOM;
    return CCE_OK;
}

/* bytes-only content digest (numel + data) */
static uint64_t tensor_digest(const cce_tensor* t) {
    uint64_t h = 1469598103934665603ULL;
    const uint64_t P = 1099511628211ULL;
    uint64_t numel = (uint64_t)t->numel;
    const unsigned char* p = (const unsigned char*)&numel;
    for (size_t i = 0; i < sizeof(numel); i++) { h ^= p[i]; h *= P; }
    p = (const unsigned char*)t->data;
    for (size_t i = 0; i < t->numel * sizeof(float); i++) { h ^= p[i]; h *= P; }
    return h;
}

/* ---- store primitives ---- */

static void digest_path(const cce_weight_store* s, uint64_t digest, char* out, size_t cap) {
    snprintf(out, cap, "%s/%016llx.spec", s->dir, (unsigned long long)digest);
}

static long file_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

cce_result cce_weight_store_open(cce_weight_store** out, const char* dir) {
    if (!out || !dir) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    WS_MKDIR(dir); /* idempotent */
    cce_weight_store* s = (cce_weight_store*)calloc(1, sizeof(*s));
    if (!s) return CCE_ERR_OOM;
    strncpy(s->dir, dir, sizeof(s->dir) - 1);

    /* count existing payloads (the filesystem is the index) */
    char probe[600];
    snprintf(probe, sizeof(probe), "%s", dir);
    /* portable-enough directory scan: try opendir on posix, Find on win */
#ifdef _WIN32
    {
        struct _finddata_t fd;
        char pat[600];
        snprintf(pat, sizeof(pat), "%s/*.spec", dir);
        intptr_t h = _findfirst(pat, &fd);
        if (h != -1) {
            do { s->count++; s->bytes += (size_t)fd.size; } while (_findnext(h, &fd) == 0);
            _findclose(h);
        }
    }
#else
    {
        DIR* d = opendir(probe);
        if (d) {
            struct dirent* ent;
            while ((ent = readdir(d)) != NULL) {
                size_t n = strlen(ent->d_name);
                if (n > 5 && strcmp(ent->d_name + n - 5, ".spec") == 0) {
                    char path[700];
                    long bytes;
                    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
                    bytes = file_size(path);
                    if (bytes >= 0) {
                        s->count++;
                        s->bytes += (size_t)bytes;
                    }
                }
            }
            closedir(d);
        }
    }
#endif
    *out = s;
    return CCE_OK;
}

void cce_weight_store_close(cce_weight_store* s) {
    free(s);
}

int cce_weight_store_contains(const cce_weight_store* s, uint64_t digest) {
    if (!s) return 0;
    char path[600];
    digest_path(s, digest, path, sizeof(path));
    return file_size(path) >= 0;
}

int cce_weight_store_count(const cce_weight_store* s) { return s ? s->count : 0; }
size_t cce_weight_store_bytes(const cce_weight_store* s) { return s ? s->bytes : 0; }

/* write-or-verify: the honesty core. */
static cce_result put_blob(cce_weight_store* s, uint64_t digest, const ws_blob* b, int* reused_out) {
    char path[600];
    digest_path(s, digest, path, sizeof(path));
    long existing = file_size(path);
    if (existing >= 0) {
        /* byte-verify the reuse claim; refuse on collision */
        if ((size_t)existing != b->len) return CCE_ERR_UNSUPPORTED;
        FILE* f = fopen(path, "rb");
        if (!f) return CCE_ERR_IO;
        unsigned char* have = (unsigned char*)malloc(b->len);
        if (!have) { fclose(f); return CCE_ERR_OOM; }
        size_t got = fread(have, 1, b->len, f);
        fclose(f);
        int same = (got == b->len) && (memcmp(have, b->buf, b->len) == 0);
        free(have);
        if (!same) return CCE_ERR_UNSUPPORTED; /* digest collision: different bytes */
        if (reused_out) *reused_out = 1;
        return CCE_OK;
    }
    FILE* f = fopen(path, "wb");
    if (!f) return CCE_ERR_IO;
    size_t wrote = fwrite(b->buf, 1, b->len, f);
    fclose(f);
    if (wrote != b->len) { remove(path); return CCE_ERR_IO; }
    s->count++;
    s->bytes += b->len;
    if (reused_out) *reused_out = 0;
    return CCE_OK;
}

cce_result cce_weight_store_put(cce_weight_store* s, const cce_cascade* cas,
                                uint64_t* digest_out, int* reused_out) {
    if (!s || !cas) return CCE_ERR_INVALID_ARG;
    uint64_t d = cce_spec_digest(cas);
    if (digest_out) *digest_out = d;
    ws_blob b = {0};
    cce_result rc = serialize_cascade(cas, &b);
    if (rc == CCE_OK) rc = put_blob(s, d, &b, reused_out);
    free(b.buf);
    return rc;
}

cce_result cce_weight_store_put_tensor(cce_weight_store* s, const cce_tensor* t,
                                       uint64_t* digest_out, int* reused_out) {
    if (!s || !t || !t->data) return CCE_ERR_INVALID_ARG;
    uint64_t d = tensor_digest(t);
    if (digest_out) *digest_out = d;
    ws_blob b = {0};
    cce_result rc = serialize_tensor(t, &b);
    if (rc == CCE_OK) rc = put_blob(s, d, &b, reused_out);
    free(b.buf);
    return rc;
}

/* ---- get: rehydrate ---- */

static cce_result read_payload(const cce_weight_store* s, uint64_t digest,
                               unsigned char** buf_out, size_t* len_out, uint32_t* kind_out) {
    char path[600];
    digest_path(s, digest, path, sizeof(path));
    long n = file_size(path);
    if (n < 12) return CCE_ERR_NOT_FOUND;
    FILE* f = fopen(path, "rb");
    if (!f) return CCE_ERR_NOT_FOUND;
    unsigned char* buf = (unsigned char*)malloc((size_t)n);
    if (!buf) { fclose(f); return CCE_ERR_OOM; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return CCE_ERR_IO; }
    fclose(f);
    if (memcmp(buf, WS_MAGIC, 4) != 0) { free(buf); return CCE_ERR_UNSUPPORTED; }
    uint32_t ver, kind;
    memcpy(&ver, buf + 4, 4);
    memcpy(&kind, buf + 8, 4);
    if (ver != WS_VERSION) { free(buf); return CCE_ERR_UNSUPPORTED; }
    *buf_out = buf; *len_out = (size_t)n; *kind_out = kind;
    return CCE_OK;
}

cce_result cce_weight_store_get(cce_weight_store* s, uint64_t digest, cce_cascade** out) {
    if (!s || !out) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    unsigned char* buf = NULL; size_t len = 0; uint32_t kind = 2;
    cce_result rc = read_payload(s, digest, &buf, &len, &kind);
    if (rc != CCE_OK) return rc;
    if (kind != 0) { free(buf); return CCE_ERR_UNSUPPORTED; }

    size_t off = 12;
    int nb = 0;
    if (off + 4 > len) { free(buf); return CCE_ERR_IO; }
    memcpy(&nb, buf + off, 4); off += 4;
    if (nb < 1 || nb > 64) { free(buf); return CCE_ERR_UNSUPPORTED; }

    cce_cascade* cas = NULL;
    if (cce_cascade_create(&cas, nb) != CCE_OK) { free(buf); return CCE_ERR_OOM; }
    for (int i = 0; i < nb; i++) {
        int type, in, out_d, has_bias;
        if (off + 16 > len) { cce_cascade_destroy(cas); free(buf); return CCE_ERR_IO; }
        memcpy(&type, buf + off, 4); memcpy(&in, buf + off + 4, 4);
        memcpy(&out_d, buf + off + 8, 4); memcpy(&has_bias, buf + off + 12, 4);
        off += 16;
        size_t wbytes = (size_t)in * out_d * sizeof(float);
        size_t bbytes = has_bias ? (size_t)out_d * sizeof(float) : 0;
        if (in <= 0 || out_d <= 0 || off + wbytes + bbytes > len) {
            cce_cascade_destroy(cas); free(buf); return CCE_ERR_IO;
        }
        cce_result arc = (type == (int)CCE_BLOCK_LINEAR_HEAD)
                       ? cce_cascade_add_linear_head(cas, in, out_d, 0.0f)
                       : cce_cascade_add_linear(cas, in, out_d, 0.0f);
        if (arc != CCE_OK) { cce_cascade_destroy(cas); free(buf); return arc; }
        cce_block* blk = &cas->blocks[cas->num_blocks - 1];
        blk->type = (cce_block_type_t)type;
        memcpy(blk->weights.data, buf + off, wbytes); off += wbytes;
        if (has_bias) {
            if (blk->bias.numel == 0) {
                int bsh[1] = { out_d };
                if (cce_tensor_alloc(&blk->bias, bsh, 1) != CCE_OK) {
                    cce_cascade_destroy(cas); free(buf); return CCE_ERR_OOM;
                }
            }
            memcpy(blk->bias.data, buf + off, bbytes); off += bbytes;
        }
    }
    free(buf);
    *out = cas;
    return CCE_OK;
}

cce_result cce_weight_store_get_tensor(cce_weight_store* s, uint64_t digest,
                                       const int* shape, int ndim, cce_tensor* out) {
    if (!s || !out || !shape || ndim < 1 || ndim > CCE_MAX_DIMS) return CCE_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    unsigned char* buf = NULL; size_t len = 0; uint32_t kind = 2;
    cce_result rc = read_payload(s, digest, &buf, &len, &kind);
    if (rc != CCE_OK) return rc;
    if (kind != 1) { free(buf); return CCE_ERR_UNSUPPORTED; }

    size_t off = 12;
    uint64_t numel = 0;
    if (off + 8 > len) { free(buf); return CCE_ERR_IO; }
    memcpy(&numel, buf + off, 8); off += 8;

    if (cce_tensor_alloc(out, (int*)shape, ndim) != CCE_OK) { free(buf); return CCE_ERR_OOM; }
    if ((uint64_t)out->numel != numel || off + numel * sizeof(float) > len) {
        cce_tensor_free(out); free(buf); return CCE_ERR_UNSUPPORTED; /* shape/payload mismatch */
    }
    memcpy(out->data, buf + off, (size_t)numel * sizeof(float));
    free(buf);
    return CCE_OK;
}

/* ---- model ingest ---- */

typedef struct { FILE* f; cce_weight_store* s; int total, fresh, reused; cce_result rc; } ingest_ctx;

static void ingest_spec(ingest_ctx* c, const char* branch, const cce_cascade* cas) {
    if (c->rc != CCE_OK || !cas) return;
    uint64_t d = 0; int r = 0;
    c->rc = cce_weight_store_put(c->s, cas, &d, &r);
    if (c->rc != CCE_OK) return;
    fprintf(c->f, "spec %s %016llx\n", branch, (unsigned long long)d);
    c->total++; if (r) c->reused++; else c->fresh++;
}

static void ingest_tensor(ingest_ctx* c, const char* slot, const cce_tensor* t) {
    if (c->rc != CCE_OK || !t || !t->data || t->numel == 0) return;
    uint64_t d = 0; int r = 0;
    c->rc = cce_weight_store_put_tensor(c->s, t, &d, &r);
    if (c->rc != CCE_OK) return;
    /* shape is model metadata, recorded here (payloads are bytes-only) */
    fprintf(c->f, "tensor %s %016llx %d", slot, (unsigned long long)d, t->ndim);
    for (int i = 0; i < t->ndim; i++) fprintf(c->f, " %d", t->shape[i]);
    fprintf(c->f, "\n");
    c->total++; if (r) c->reused++; else c->fresh++;
}

cce_result cce_weight_store_ingest_model(cce_weight_store* s, const cce_anymodel* m,
                                         const char* model_name, const char* manifest_path,
                                         int* n_total, int* n_new, int* n_reused) {
    if (!s || !m || !manifest_path) return CCE_ERR_INVALID_ARG;
    if (n_total) *n_total = 0;
    if (n_new) *n_new = 0;
    if (n_reused) *n_reused = 0;

    FILE* f = fopen(manifest_path, "wb");
    if (!f) return CCE_ERR_IO;
    ingest_ctx c = { f, s, 0, 0, 0, CCE_OK };
    char slot[128];

    if (m->transformer) {
        const cce_gguf_qwen2* t = m->transformer;
        fprintf(f, "CNET_MANIFEST v1\nmodel %s\nfamily transformer\n", model_name ? model_name : "?");
        fprintf(f, "hparams %d %d %d %d %d %d %d %d %d\n",
                t->n_layer, t->n_embd, t->n_head, t->n_kv_head, t->head_dim,
                t->vocab_size, t->ctx_len, t->feed_forward_length, t->max_ctx);
        fprintf(f, "numerics %.9g %.9g\n", (double)t->rope_freq_base, (double)t->rms_eps);
        fprintf(f, "tok %d %d %s\n", t->bos_token_id, t->eos_token_id,
                t->tokenizer_model[0] ? t->tokenizer_model : "?");
        if (t->forest) {
            for (int b = 0; b < t->forest->num_branches; b++)
                ingest_spec(&c, t->forest->branches[b].name, t->forest->branches[b].cascade);
        }
        ingest_tensor(&c, "tok_emb", &t->tok_emb);
        ingest_tensor(&c, "output_norm", &t->output_norm);
        ingest_tensor(&c, "output", &t->output);
        ingest_tensor(&c, "mtp_pre", &t->mtp_pre);
        ingest_tensor(&c, "mtp_post", &t->mtp_post);
        ingest_tensor(&c, "rope_freqs", &t->rope_freqs);
        for (int l = 0; l < t->n_layer; l++) {
            snprintf(slot, sizeof(slot), "attn_norm.%d", l);
            ingest_tensor(&c, slot, &t->attn_norm[l]);
            snprintf(slot, sizeof(slot), "ffn_norm.%d", l);
            ingest_tensor(&c, slot, &t->ffn_norm[l]);
            snprintf(slot, sizeof(slot), "attn_q_norm.%d", l);
            ingest_tensor(&c, slot, &t->attn_q_norm[l]);
            snprintf(slot, sizeof(slot), "post_attention_norm.%d", l);
            ingest_tensor(&c, slot, &t->post_attention_norm[l]);
            snprintf(slot, sizeof(slot), "post_ffw_norm.%d", l);
            ingest_tensor(&c, slot, &t->post_ffw_norm[l]);
            snprintf(slot, sizeof(slot), "layer_output_scale.%d", l);
            ingest_tensor(&c, slot, &t->layer_output_scale[l]);
        }
    } else if (m->ssm) {
        const cce_ssm_model* t = m->ssm;
        fprintf(f, "CNET_MANIFEST v1\nmodel %s\nfamily ssm\n", model_name ? model_name : "?");
        fprintf(f, "hparams %d %d %d %d %d %d %d\n",
                t->n_layer, t->d_model, t->d_inner, t->d_state, t->d_conv, t->dt_rank, t->vocab_size);
        if (t->forest) {
            for (int b = 0; b < t->forest->num_branches; b++)
                ingest_spec(&c, t->forest->branches[b].name, t->forest->branches[b].cascade);
        }
        ingest_tensor(&c, "tok_emb", &t->tok_emb);
        ingest_tensor(&c, "norm_f", &t->norm_f);
        for (int l = 0; l < t->n_layer; l++) {
            snprintf(slot, sizeof(slot), "norm.%d", l);   ingest_tensor(&c, slot, &t->norm[l]);
            snprintf(slot, sizeof(slot), "conv_w.%d", l); ingest_tensor(&c, slot, &t->conv_w[l]);
            snprintf(slot, sizeof(slot), "conv_b.%d", l); ingest_tensor(&c, slot, &t->conv_b[l]);
            snprintf(slot, sizeof(slot), "A_log.%d", l);  ingest_tensor(&c, slot, &t->A_log[l]);
            snprintf(slot, sizeof(slot), "Dvec.%d", l);   ingest_tensor(&c, slot, &t->Dvec[l]);
        }
    } else {
        fclose(f);
        remove(manifest_path);
        return CCE_ERR_UNSUPPORTED;
    }

    fprintf(f, "end\n");
    fclose(f);
    if (c.rc != CCE_OK) { remove(manifest_path); return c.rc; }
    if (n_total) *n_total = c.total;
    if (n_new) *n_new = c.fresh;
    if (n_reused) *n_reused = c.reused;
    return CCE_OK;
}

/* ---- transformer restore ---- */

static int slot_layer(const char* slot, const char* prefix) {
    size_t n = strlen(prefix);
    if (strncmp(slot, prefix, n) != 0 || slot[n] != '.') return -1;
    return atoi(slot + n + 1);
}

cce_result cce_weight_store_restore_transformer(cce_weight_store* s, const char* manifest_path,
                                                const char* forest_archive_path,
                                                cce_gguf_qwen2** out) {
    if (!s || !manifest_path || !forest_archive_path || !out) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    FILE* f = fopen(manifest_path, "rb");
    if (!f) return CCE_ERR_IO;
    char line[512];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "CNET_MANIFEST v1", 16) != 0) {
        fclose(f); return CCE_ERR_UNSUPPORTED;
    }

    cce_gguf_qwen2* m = (cce_gguf_qwen2*)calloc(1, sizeof(*m));
    if (!m) { fclose(f); return CCE_ERR_OOM; }
    m->bos_token_id = -1; m->eos_token_id = -1;

    int is_transformer = 0;
    cce_result rc = CCE_OK;

    /* header pass first (hparams before branches, as written) */
    while (rc == CCE_OK && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "family ", 7) == 0) {
            is_transformer = (strncmp(line + 7, "transformer", 11) == 0);
            if (!is_transformer) rc = CCE_ERR_UNSUPPORTED;
        } else if (strncmp(line, "hparams ", 8) == 0) {
            if (sscanf(line + 8, "%d %d %d %d %d %d %d %d %d",
                       &m->n_layer, &m->n_embd, &m->n_head, &m->n_kv_head, &m->head_dim,
                       &m->vocab_size, &m->ctx_len, &m->feed_forward_length, &m->max_ctx) != 9)
                rc = CCE_ERR_UNSUPPORTED;
            else if (m->n_layer < 1 || m->n_layer > 4096) rc = CCE_ERR_UNSUPPORTED;
            else {
                m->attn_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
                m->ffn_norm  = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
                m->attn_q_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
                m->post_attention_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
                m->post_ffw_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
                m->layer_output_scale = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
                if (!m->attn_norm || !m->ffn_norm || !m->attn_q_norm ||
                    !m->post_attention_norm || !m->post_ffw_norm || !m->layer_output_scale)
                    rc = CCE_ERR_OOM;
                else {
                    remove(forest_archive_path);
                    if (cce_forest_open(&m->forest, forest_archive_path,
                                        8 * m->n_layer + 8) != CCE_OK) rc = CCE_ERR_IO;
                }
            }
        } else if (strncmp(line, "numerics ", 9) == 0) {
            double rb = 0, eps = 0;
            if (sscanf(line + 9, "%lg %lg", &rb, &eps) == 2) {
                m->rope_freq_base = (float)rb;
                m->rms_eps = (float)eps;
            }
        } else if (strncmp(line, "tok ", 4) == 0) {
            sscanf(line + 4, "%d %d %63s", &m->bos_token_id, &m->eos_token_id, m->tokenizer_model);
        } else if (strncmp(line, "spec ", 5) == 0) {
            char branch[128];
            unsigned long long d = 0;
            if (!m->forest || sscanf(line + 5, "%127s %llx", branch, &d) != 2) { rc = CCE_ERR_UNSUPPORTED; break; }
            cce_cascade* cas = NULL;
            rc = cce_weight_store_get(s, (uint64_t)d, &cas);
            if (rc != CCE_OK) break;
            int idx = -1;
            rc = cce_forest_add_cascade_branch(m->forest, cas, branch, &idx);
            if (rc != CCE_OK) { cce_cascade_destroy(cas); break; }
            free(cas); /* forest owns its shallow copy */
        } else if (strncmp(line, "tensor ", 7) == 0) {
            char slot[128];
            unsigned long long d = 0;
            int nd = 0, shp[CCE_MAX_DIMS] = {0};
            int got = sscanf(line + 7, "%127s %llx %d %d %d %d %d", slot, &d, &nd,
                             &shp[0], &shp[1], &shp[2], &shp[3]);
            if (got < 4 || nd < 1 || nd > 4 || got < 3 + nd) { rc = CCE_ERR_UNSUPPORTED; break; }
            cce_tensor* dst = NULL;
            int l;
            if      (strcmp(slot, "tok_emb") == 0)     dst = &m->tok_emb;
            else if (strcmp(slot, "output_norm") == 0) dst = &m->output_norm;
            else if (strcmp(slot, "output") == 0)      dst = &m->output;
            else if (strcmp(slot, "mtp_pre") == 0)     dst = &m->mtp_pre;
            else if (strcmp(slot, "mtp_post") == 0)    dst = &m->mtp_post;
            else if (strcmp(slot, "rope_freqs") == 0)  dst = &m->rope_freqs;
            else if ((l = slot_layer(slot, "attn_norm")) >= 0 && l < m->n_layer) dst = &m->attn_norm[l];
            else if ((l = slot_layer(slot, "ffn_norm")) >= 0 && l < m->n_layer)  dst = &m->ffn_norm[l];
            else if ((l = slot_layer(slot, "attn_q_norm")) >= 0 && l < m->n_layer) dst = &m->attn_q_norm[l];
            else if ((l = slot_layer(slot, "post_attention_norm")) >= 0 && l < m->n_layer) dst = &m->post_attention_norm[l];
            else if ((l = slot_layer(slot, "post_ffw_norm")) >= 0 && l < m->n_layer) dst = &m->post_ffw_norm[l];
            else if ((l = slot_layer(slot, "layer_output_scale")) >= 0 && l < m->n_layer) dst = &m->layer_output_scale[l];
            if (dst) rc = cce_weight_store_get_tensor(s, (uint64_t)d, shp, nd, dst);
        } else if (strncmp(line, "end", 3) == 0) {
            break;
        }
    }
    fclose(f);

    if (rc == CCE_OK && (!m->forest || !m->tok_emb.data)) rc = CCE_ERR_UNSUPPORTED;
    if (rc == CCE_OK) {
        if (m->max_ctx <= 0) m->max_ctx = 2048;
        /* Manifest restore bypasses the GGUF/safetensors loaders, so rebuild
           the same uniform per-layer geometry before sizing KV slots. */
        rc = cce_gguf_qwen2_geom_uniform(m);
    }
    if (rc == CCE_OK) {
        size_t k_size = (size_t)m->max_ctx * m->k_slot_floats;
        size_t v_size = (size_t)m->max_ctx * m->v_slot_floats;
        m->k_cache = (float*)calloc(k_size, sizeof(float));
        m->v_cache = (float*)calloc(v_size, sizeof(float));
        if (!m->k_cache || !m->v_cache) rc = CCE_ERR_OOM;
    }
    if (rc != CCE_OK) { cce_gguf_qwen2_free(m); return rc; }
    m->cur_pos = 0;
    *out = m;
    return CCE_OK;
}

/* ---- ssm restore (mirrors the transformer restore + cce_ssm_load wiring) ---- */

cce_result cce_weight_store_restore_ssm(cce_weight_store* s, const char* manifest_path,
                                        const char* forest_archive_path,
                                        cce_ssm_model** out) {
    if (!s || !manifest_path || !forest_archive_path || !out) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    FILE* f = fopen(manifest_path, "rb");
    if (!f) return CCE_ERR_IO;
    char line[512];
    if (!fgets(line, sizeof(line), f) || strncmp(line, "CNET_MANIFEST v1", 16) != 0) {
        fclose(f); return CCE_ERR_UNSUPPORTED;
    }

    cce_ssm_model* m = (cce_ssm_model*)calloc(1, sizeof(*m));
    if (!m) { fclose(f); return CCE_ERR_OOM; }
    m->norm_eps = 1e-5f;               /* matches cce_ssm_load's default */
    m->bos_token_id = -1; m->eos_token_id = -1;

    cce_result rc = CCE_OK;
    while (rc == CCE_OK && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "family ", 7) == 0) {
            if (strncmp(line + 7, "ssm", 3) != 0) rc = CCE_ERR_UNSUPPORTED;
        } else if (strncmp(line, "hparams ", 8) == 0) {
            if (sscanf(line + 8, "%d %d %d %d %d %d %d",
                       &m->n_layer, &m->d_model, &m->d_inner, &m->d_state,
                       &m->d_conv, &m->dt_rank, &m->vocab_size) != 7)
                rc = CCE_ERR_UNSUPPORTED;
            else if (m->n_layer < 1 || m->n_layer > 4096) rc = CCE_ERR_UNSUPPORTED;
            else {
                int L = m->n_layer;
                m->norm   = (cce_tensor*)calloc(L, sizeof(cce_tensor));
                m->conv_w = (cce_tensor*)calloc(L, sizeof(cce_tensor));
                m->conv_b = (cce_tensor*)calloc(L, sizeof(cce_tensor));
                m->A_log  = (cce_tensor*)calloc(L, sizeof(cce_tensor));
                m->Dvec   = (cce_tensor*)calloc(L, sizeof(cce_tensor));
                m->in_cas  = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
                m->x_cas   = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
                m->dt_cas  = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
                m->out_cas = (cce_cascade**)calloc(L, sizeof(cce_cascade*));
                if (!m->norm || !m->conv_w || !m->conv_b || !m->A_log || !m->Dvec ||
                    !m->in_cas || !m->x_cas || !m->dt_cas || !m->out_cas)
                    rc = CCE_ERR_OOM;
                else {
                    remove(forest_archive_path);
                    if (cce_forest_open(&m->forest, forest_archive_path,
                                        8 * L + 8) != CCE_OK) rc = CCE_ERR_IO;
                }
            }
        } else if (strncmp(line, "spec ", 5) == 0) {
            char branch[128];
            unsigned long long d = 0;
            if (!m->forest || sscanf(line + 5, "%127s %llx", branch, &d) != 2) { rc = CCE_ERR_UNSUPPORTED; break; }
            cce_cascade* cas = NULL;
            rc = cce_weight_store_get(s, (uint64_t)d, &cas);
            if (rc != CCE_OK) break;
            int idx = -1;
            rc = cce_forest_add_cascade_branch(m->forest, cas, branch, &idx);
            if (rc != CCE_OK) { cce_cascade_destroy(cas); break; }
            free(cas); /* forest owns its shallow copy */
        } else if (strncmp(line, "tensor ", 7) == 0) {
            char slot[128];
            unsigned long long d = 0;
            int nd = 0, shp[CCE_MAX_DIMS] = {0};
            int got = sscanf(line + 7, "%127s %llx %d %d %d %d %d", slot, &d, &nd,
                             &shp[0], &shp[1], &shp[2], &shp[3]);
            if (got < 4 || nd < 1 || nd > 4 || got < 3 + nd) { rc = CCE_ERR_UNSUPPORTED; break; }
            cce_tensor* dst = NULL;
            int l;
            if      (strcmp(slot, "tok_emb") == 0) dst = &m->tok_emb;
            else if (strcmp(slot, "norm_f") == 0)  dst = &m->norm_f;
            else if ((l = slot_layer(slot, "norm")) >= 0 && l < m->n_layer)   dst = &m->norm[l];
            else if ((l = slot_layer(slot, "conv_w")) >= 0 && l < m->n_layer) dst = &m->conv_w[l];
            else if ((l = slot_layer(slot, "conv_b")) >= 0 && l < m->n_layer) dst = &m->conv_b[l];
            else if ((l = slot_layer(slot, "A_log")) >= 0 && l < m->n_layer)  dst = &m->A_log[l];
            else if ((l = slot_layer(slot, "Dvec")) >= 0 && l < m->n_layer)   dst = &m->Dvec[l];
            if (dst) rc = cce_weight_store_get_tensor(s, (uint64_t)d, shp, nd, dst);
        } else if (strncmp(line, "end", 3) == 0) {
            break;
        }
    }
    fclose(f);

    if (rc == CCE_OK && (!m->forest || !m->tok_emb.data || !m->norm_f.data))
        rc = CCE_ERR_UNSUPPORTED;

    /* resolve the cached per-layer cascades exactly like cce_ssm_load */
    if (rc == CCE_OK) {
        char brname[64];
        for (int l = 0; l < m->n_layer && rc == CCE_OK; l++) {
            cce_forest* fr = m->forest;
            for (int b = 0; b < fr->num_branches; b++) {
                const char* nm = fr->branches[b].name;
                snprintf(brname, sizeof(brname), "mamba.blk.%d.in_proj", l);
                if (strcmp(nm, brname) == 0) m->in_cas[l] = fr->branches[b].cascade;
                snprintf(brname, sizeof(brname), "mamba.blk.%d.x_proj", l);
                if (strcmp(nm, brname) == 0) m->x_cas[l] = fr->branches[b].cascade;
                snprintf(brname, sizeof(brname), "mamba.blk.%d.dt_proj", l);
                if (strcmp(nm, brname) == 0) m->dt_cas[l] = fr->branches[b].cascade;
                snprintf(brname, sizeof(brname), "mamba.blk.%d.out_proj", l);
                if (strcmp(nm, brname) == 0) m->out_cas[l] = fr->branches[b].cascade;
                if (strcmp(nm, "mamba.lm_head") == 0) m->head_cas = fr->branches[b].cascade;
            }
            if (!m->in_cas[l] || !m->x_cas[l] || !m->dt_cas[l] || !m->out_cas[l])
                rc = CCE_ERR_UNSUPPORTED;
        }
        if (rc == CCE_OK && !m->head_cas) rc = CCE_ERR_UNSUPPORTED;
    }

    if (rc == CCE_OK) {
        m->conv_state = (float*)calloc((size_t)m->n_layer * m->d_inner * m->d_conv, sizeof(float));
        m->ssm_state  = (float*)calloc((size_t)m->n_layer * m->d_inner * m->d_state, sizeof(float));
        if (!m->conv_state || !m->ssm_state) rc = CCE_ERR_OOM;
    }
    if (rc != CCE_OK) { cce_ssm_free(m); return rc; }
    m->cur_pos = 0;
    *out = m;
    return CCE_OK;
}
