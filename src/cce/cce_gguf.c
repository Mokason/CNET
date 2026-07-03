#include "../../include/cce/cce_gguf.h"
#include "../../include/cce/cce_safetensors.h" /* for some helpers if needed, but we'll be self-contained */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <math.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

/* ---- Internal GGUF state ---- */

typedef struct {
    char     key[256];
    uint32_t type;
    uint64_t len;     /* for strings/arrays */
    union {
        uint8_t  u8;
        int8_t   i8;
        uint16_t u16;
        int16_t  i16;
        uint32_t u32;
        int32_t  i32;
        float    f32;
        uint64_t u64;
        int64_t  i64;
        double   f64;
        char*    str; /* owned */
    } val;
    /* array support is minimal for Phase 1 */
} gguf_kv;

struct cce_gguf {
    FILE* f;
    char path[512];
    uint32_t version;
    uint64_t n_tensors_hdr;
    uint64_t n_kv_hdr;

    cce_gguf_tensor_meta* tensors;
    int n_tensors;

    gguf_kv* kvs;
    int n_kvs;

    /* cached useful hparams */
    char arch[64];
    int n_layer;
    int hidden_size;
    int n_heads;
    int n_kv_heads;
    int vocab_size;
    int context_length;
    int feed_forward_length;
    float rope_freq_base;   /* 0 = absent */
    float rms_eps;          /* 0 = absent */

    /* tokenizer metadata embedded from GGUF (Phase 4) */
    char tokenizer_model[64];
    int bos_token_id;
    int eos_token_id;

    uint64_t data_offset; /* absolute file offset where tensor data begins */
};

/* cce_gguf_qwen2 struct defined in header */
    cce_forest* forest;  /* holds all the linear specialists */

    /* per-layer norms (RMS, only weight) */
/* struct cce_gguf_qwen2 defined only in header to avoid redefinition */

/* ---- Low level readers (little endian assumed) ---- */

static bool gguf_read_u8(FILE* f, uint8_t* v)  { return fread(v, 1, 1, f) == 1; }
static bool gguf_read_u32(FILE* f, uint32_t* v) { return fread(v, 4, 1, f) == 1; }
static bool gguf_read_u64(FILE* f, uint64_t* v) { return fread(v, 8, 1, f) == 1; }
static bool gguf_read_f32(FILE* f, float* v)    { return fread(v, 4, 1, f) == 1; }

static float gguf_f16_to_f32(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t exp  = (h & 0x7C00) >> 10;
    uint32_t mant = h & 0x03FF;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) f = sign;
        else {
            exp = 1;
            while ((mant & 0x200) == 0) { mant <<= 1; exp--; }
            mant &= 0x1FF;
            f = sign | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        f = sign | 0x7F800000 | (mant << 13);
    } else {
        f = sign | ((exp + 112) << 23) | (mant << 13);
    }
    float r;
    memcpy(&r, &f, 4);
    return r;
}

static bool gguf_read_string(FILE* f, char* buf, size_t cap, uint64_t* out_len) {
    uint64_t len = 0;
    if (!gguf_read_u64(f, &len)) return false;
    if (out_len) *out_len = len;
    if (len >= cap) {
        /* skip large string */
        fseek(f, (long)len, SEEK_CUR);
        if (buf && cap > 0) buf[0] = 0;
        return true;
    }
    if (fread(buf, 1, (size_t)len, f) != len) return false;
    buf[len] = 0;
    return true;
}

/* Read a single KV. Consume bytes correctly for Phase 1. */
static bool gguf_read_kv(FILE* f, gguf_kv* kv) {
    uint64_t klen = 0;
    if (!gguf_read_string(f, kv->key, sizeof(kv->key), &klen)) return false;

    uint32_t typ = 0;
    if (!gguf_read_u32(f, &typ)) return false;
    kv->type = typ;

    switch (typ) {
        case GGUF_TYPE_UINT8:  return gguf_read_u8(f, &kv->val.u8);
        case GGUF_TYPE_INT8:   return fread(&kv->val.i8, 1, 1, f) == 1;
        case GGUF_TYPE_UINT16: return fread(&kv->val.u16, 2, 1, f) == 1;
        case GGUF_TYPE_INT16:  return fread(&kv->val.i16, 2, 1, f) == 1;
        case GGUF_TYPE_UINT32: return gguf_read_u32(f, &kv->val.u32);
        case GGUF_TYPE_INT32:  return fread(&kv->val.i32, 4, 1, f) == 1;
        case GGUF_TYPE_FLOAT32: return gguf_read_f32(f, &kv->val.f32);
        case GGUF_TYPE_BOOL:   return fread(&kv->val.u8, 1, 1, f) == 1;
        case GGUF_TYPE_STRING: {
            uint64_t slen = 0;
            if (!gguf_read_u64(f, &slen)) return false;
            char* s = (char*)malloc((size_t)slen + 1);
            if (!s) { fseek(f, (long)slen, SEEK_CUR); return false; }
            if (slen > 0 && fread(s, 1, (size_t)slen, f) != slen) { free(s); return false; }
            s[slen] = 0;
            kv->val.str = s;
            return true;
        }
        case GGUF_TYPE_UINT64: return gguf_read_u64(f, &kv->val.u64);
        case GGUF_TYPE_INT64:  return fread(&kv->val.i64, 8, 1, f) == 1;
        case GGUF_TYPE_FLOAT64: return fread(&kv->val.f64, 8, 1, f) == 1;
        case GGUF_TYPE_ARRAY: {
            uint32_t elem_type; uint64_t n;
            if (!gguf_read_u32(f, &elem_type) || !gguf_read_u64(f, &n)) return false;
            for (uint64_t k=0; k < n; k++) {
                if (elem_type == GGUF_TYPE_STRING) {
                    uint64_t sl = 0; gguf_read_u64(f, &sl); fseek(f, (long)sl, SEEK_CUR);
                } else {
                    size_t esz = 4;
                    if (elem_type==0 || elem_type==1 || elem_type==7) esz=1;
                    else if (elem_type==2||elem_type==3) esz=2;
                    else if (elem_type==10||elem_type==11||elem_type==12) esz=8;
                    fseek(f, (long)esz, SEEK_CUR);
                }
            }
            return true;
        }
        default:
            return false;
    }
}

static void gguf_free_kv(gguf_kv* kv) {
    if (kv->type == GGUF_TYPE_STRING && kv->val.str) {
        free(kv->val.str);
        kv->val.str = NULL;
    }
}

/* ---- Public API ---- */

cce_result cce_gguf_load(const char* path, cce_gguf** out) {
    if (!path || !out) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    cce_gguf* g = (cce_gguf*)calloc(1, sizeof(cce_gguf));
    if (!g) return CCE_ERR_OOM;
    strncpy(g->path, path, sizeof(g->path)-1);

    g->f = fopen(path, "rb");
    if (!g->f) { free(g); return CCE_ERR_IO; }

    /* Header */
    char magic[4];
    if (fread(magic, 1, 4, g->f) != 4 || memcmp(magic, "GGUF", 4) != 0) {
        fclose(g->f); free(g); return CCE_ERR_UNSUPPORTED;
    }
    uint32_t ver = 0;
    if (!gguf_read_u32(g->f, &ver)) { fclose(g->f); free(g); return CCE_ERR_IO; }
    g->version = ver;

    uint64_t nt = 0, nk = 0;
    if (!gguf_read_u64(g->f, &nt) || !gguf_read_u64(g->f, &nk)) {
        fclose(g->f); free(g); return CCE_ERR_IO;
    }
    g->n_tensors_hdr = nt;
    g->n_kv_hdr = nk;

    /* Metadata */
    g->kvs = (gguf_kv*)calloc((size_t)nk, sizeof(gguf_kv));
    g->n_kvs = (int)nk;
    for (uint64_t i = 0; i < nk; i++) {
        if (!gguf_read_kv(g->f, &g->kvs[i])) {
            /* best effort continue */
        }
    }

    /* Extract useful hparams (Qwen2 / Llama style) */
    g->arch[0] = 0;
    g->tokenizer_model[0] = 0;
    g->bos_token_id = -1;
    g->eos_token_id = -1;
    for (int i = 0; i < g->n_kvs; i++) {
        const char* k = g->kvs[i].key;
        if (strcmp(k, "general.architecture") == 0 && g->kvs[i].type == GGUF_TYPE_STRING && g->kvs[i].val.str) {
            strncpy(g->arch, g->kvs[i].val.str, sizeof(g->arch)-1);
        }
        if (strcmp(k, "tokenizer.ggml.model") == 0 && g->kvs[i].type == GGUF_TYPE_STRING && g->kvs[i].val.str) {
            strncpy(g->tokenizer_model, g->kvs[i].val.str, sizeof(g->tokenizer_model)-1);
        }
        if (strcmp(k, "tokenizer.ggml.bos_token_id") == 0) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->bos_token_id = g->kvs[i].val.u32;
            else if (g->kvs[i].type == GGUF_TYPE_INT32) g->bos_token_id = g->kvs[i].val.i32;
            else if (g->kvs[i].type == GGUF_TYPE_UINT64) g->bos_token_id = (int)g->kvs[i].val.u64;
        }
        if (strcmp(k, "tokenizer.ggml.eos_token_id") == 0) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->eos_token_id = g->kvs[i].val.u32;
            else if (g->kvs[i].type == GGUF_TYPE_INT32) g->eos_token_id = g->kvs[i].val.i32;
            else if (g->kvs[i].type == GGUF_TYPE_UINT64) g->eos_token_id = (int)g->kvs[i].val.u64;
        }
        /* Flexible key matching to support qwen2/llama/gemma/gemma2/gemma4-assistant etc. */
        if (strstr(k, "block_count")) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->n_layer = g->kvs[i].val.u32;
            else if (g->kvs[i].type == GGUF_TYPE_UINT64) g->n_layer = (int)g->kvs[i].val.u64;
        }
        if (strstr(k, "embedding_length") && !strstr(k, "out") && !strstr(k, "per_layer")) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->hidden_size = g->kvs[i].val.u32;
        }
        if (strstr(k, "attention.head_count") && !strstr(k, "kv") && !strstr(k, "_kv")) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->n_heads = g->kvs[i].val.u32;
        }
        if (strstr(k, "head_count_kv") || strstr(k, ".attention.head_count_kv")) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->n_kv_heads = g->kvs[i].val.u32;
        }
        if (strstr(k, "vocab_size")) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->vocab_size = g->kvs[i].val.u32;
            else if (g->kvs[i].type == GGUF_TYPE_UINT64) g->vocab_size = (int)g->kvs[i].val.u64;
        }
        if (strstr(k, "context_length")) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->context_length = g->kvs[i].val.u32;
        }
        if (strstr(k, "feed_forward_length") || strstr(k, "ffn_length")) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32) g->feed_forward_length = g->kvs[i].val.u32;
            else if (g->kvs[i].type == GGUF_TYPE_UINT64) g->feed_forward_length = (int)g->kvs[i].val.u64;
        }
        if (strstr(k, "rope.freq_base")) {
            if (g->kvs[i].type == GGUF_TYPE_FLOAT32) g->rope_freq_base = g->kvs[i].val.f32;
            else if (g->kvs[i].type == GGUF_TYPE_FLOAT64) g->rope_freq_base = (float)g->kvs[i].val.f64;
        }
        if (strstr(k, "layer_norm_rms_epsilon") || strstr(k, "rms_norm_eps")) {
            if (g->kvs[i].type == GGUF_TYPE_FLOAT32) g->rms_eps = g->kvs[i].val.f32;
            else if (g->kvs[i].type == GGUF_TYPE_FLOAT64) g->rms_eps = (float)g->kvs[i].val.f64;
        }
    }
    if (g->n_kv_heads == 0) g->n_kv_heads = g->n_heads;

    /* Tensor table */
    g->tensors = (cce_gguf_tensor_meta*)calloc((size_t)nt, sizeof(cce_gguf_tensor_meta));
    g->n_tensors = (int)nt;

    uint64_t max_offset = 0;
    for (uint64_t i = 0; i < nt; i++) {
        cce_gguf_tensor_meta* t = &g->tensors[i];
        uint64_t nlen = 0;
        if (!gguf_read_string(g->f, t->name, sizeof(t->name), &nlen)) {
            /* error recovery */
        }
        uint32_t nd = 0;
        if (!gguf_read_u32(g->f, &nd)) nd = 0;
        t->ndim = nd;
        for (uint32_t d = 0; d < nd && d < (uint32_t)CCE_MAX_DIMS; d++) {
            uint64_t dim = 0;
            gguf_read_u64(g->f, &dim);
            t->shape[d] = (int)dim;
        }
        gguf_read_u32(g->f, &t->ggml_type);
        gguf_read_u64(g->f, &t->data_offset);

        /* Compute nbytes from shape + type (for f32 only in Phase 1) */
        size_t elems = 1;
        for (int d=0; d<t->ndim; d++) elems *= (size_t)t->shape[d];
        if (t->ggml_type == 0 /* F32 */) {
            t->nbytes = elems * 4;
        } else if (t->ggml_type == 1 /* F16 */) {
            t->nbytes = elems * 2;
        } else {
            t->nbytes = elems * 4; /* conservative */
        }
        if (t->data_offset + t->nbytes > max_offset) max_offset = t->data_offset + t->nbytes;
    }

    /* Data section starts right after the tensor info table.
       Use the actual current file position (writers align the data). */
    long data_base = ftell(g->f);
    g->data_offset = (uint64_t)data_base;

    /* For safety, if offsets in file are relative to data_start, we adjust when loading */
    /* Many writers put absolute-ish offsets; we will seek to data_offset + tensor.data_offset when loading */

    /* We don't close the file; keep it open for on-demand loads */
    *out = g;
    return CCE_OK;
}

void cce_gguf_free(cce_gguf* g) {
    if (!g) return;
    if (g->f) fclose(g->f);
    if (g->tensors) free(g->tensors);
    for (int i = 0; i < g->n_kvs; i++) gguf_free_kv(&g->kvs[i]);
    if (g->kvs) free(g->kvs);
    free(g);
}

int cce_gguf_tensor_count(const cce_gguf* g) {
    return g ? g->n_tensors : 0;
}

int cce_gguf_find_tensor(const cce_gguf* g, const char* name) {
    if (!g || !name) return -1;
    for (int i = 0; i < g->n_tensors; i++) {
        if (strcmp(g->tensors[i].name, name) == 0) return i;
    }
    return -1;
}

cce_result cce_gguf_get_tensor_meta(const cce_gguf* g, int idx, cce_gguf_tensor_meta* out) {
    if (!g || idx < 0 || idx >= g->n_tensors || !out) return CCE_ERR_INVALID_ARG;
    *out = g->tensors[idx];
    return CCE_OK;
}

cce_result cce_gguf_load_f32(const cce_gguf* g, int idx, float* buf, size_t cap_elems) {
    if (!g || idx < 0 || idx >= g->n_tensors || !buf) return CCE_ERR_INVALID_ARG;
    const cce_gguf_tensor_meta* m = &g->tensors[idx];

    size_t elems = 1;
    for (int d = 0; d < m->ndim; d++) elems *= (size_t)m->shape[d];
    if (cap_elems < elems) return CCE_ERR_INVALID_ARG;

    /* Seek to the tensor data.
       In practice for most GGUF writers the data_offset in the tensor info is the offset
       from the *start of the tensor data section*. We use the one we recorded. */
    uint64_t abs_off = g->data_offset + m->data_offset;
    if (_fseeki64(g->f, (int64_t)abs_off, SEEK_SET) != 0) return CCE_ERR_IO;

    if (m->ggml_type == 0 /* F32 */) {
        if (fread(buf, 4, elems, g->f) != elems) return CCE_ERR_IO;
    } else if (m->ggml_type == 1 /* F16 */) {
        /* proper F16->F32 */
        uint16_t* tmp = (uint16_t*)malloc(elems * 2);
        if (!tmp) return CCE_ERR_OOM;
        if (fread(tmp, 2, elems, g->f) != elems) { free(tmp); return CCE_ERR_IO; }
        for (size_t i = 0; i < elems; i++) {
            buf[i] = gguf_f16_to_f32(tmp[i]);
        }
        free(tmp);
    } else if (m->ggml_type == 8 /* Q8_0 */) {
        /* Standard GGUF Q8_0: f16 scale + 32 int8 per block (32 weights) */
        const size_t block_size = 32;
        size_t nblocks = (elems + block_size - 1) / block_size;
        for (size_t b = 0; b < nblocks; b++) {
            uint16_t d16;
            if (fread(&d16, 2, 1, g->f) != 1) return CCE_ERR_IO;
            float d = gguf_f16_to_f32(d16);
            int8_t qs[32];
            size_t this_block = (b == nblocks-1) ? (elems - b*block_size) : block_size;
            if (fread(qs, 1, this_block, g->f) != this_block) return CCE_ERR_IO;
            for (size_t i = 0; i < this_block; i++) {
                buf[b*block_size + i] = qs[i] * d;
            }
            if (this_block < block_size) fseek(g->f, (long)(block_size - this_block), SEEK_CUR);
        }
    } else if (m->ggml_type == 2 /* Q4_0 */) {
        /* Q4_0: 32 values, f16 d, 16 bytes qs (nibbles) */
        const size_t QK4_0 = 32;
        size_t nblocks = (elems + QK4_0 - 1) / QK4_0;
        for (size_t b = 0; b < nblocks; b++) {
            uint16_t d16;
            if (fread(&d16, 2, 1, g->f) != 1) return CCE_ERR_IO;
            float d = gguf_f16_to_f32(d16);
            uint8_t qs[16];
            size_t this_b = ((b+1)*QK4_0 > elems) ? (elems - b*QK4_0) : QK4_0;
            if (fread(qs, 1, 16, g->f) != 16) return CCE_ERR_IO;
            for (size_t i = 0; i < this_b; i++) {
                int x = (qs[i>>1] >> ((i&1)*4)) & 0xF;
                buf[b*QK4_0 + i] = (x - 8) * d;
            }
        }
    } else if (m->ggml_type == 3 /* Q4_1 */) {
        /* Q4_1: f16 d, f16 m, 16 bytes qs */
        const size_t QK4_1 = 32;
        size_t nblocks = (elems + QK4_1 - 1) / QK4_1;
        for (size_t b = 0; b < nblocks; b++) {
            uint16_t d16, m16;
            if (fread(&d16, 2, 1, g->f) !=1 || fread(&m16,2,1,g->f)!=1) return CCE_ERR_IO;
            float d = gguf_f16_to_f32(d16);
            float m = gguf_f16_to_f32(m16);
            uint8_t qs[16];
            if (fread(qs, 1, 16, g->f) != 16) return CCE_ERR_IO;
            size_t this_b = ((size_t)QK4_1 < (elems - b*QK4_1)) ? (size_t)QK4_1 : (elems - b*QK4_1);
            for (size_t i=0; i<this_b; i++) {
                int x = (qs[i>>1] >> ((i&1)*4)) & 0xF;
                buf[b*QK4_1 + i] = x * d + m;
            }
        }
    } else if (m->ggml_type == 6 /* Q5_0 */) {
        /* Q5_0: 32 vals, f16 d, 20 bytes (16 qs + 4 high bits) */
        const size_t QK5_0 = 32;
        size_t nblocks = (elems + QK5_0 - 1) / QK5_0;
        for (size_t b = 0; b < nblocks; b++) {
            uint16_t d16;
            if (fread(&d16, 2, 1, g->f) != 1) return CCE_ERR_IO;
            float d = gguf_f16_to_f32(d16);
            uint8_t qs[20];
            size_t this_b = ((b+1)*QK5_0 > elems) ? (elems - b*QK5_0) : QK5_0;
            if (fread(qs, 1, 20, g->f) != 20) return CCE_ERR_IO;
            for (size_t i=0; i<this_b; i++) {
                int lo = (qs[i>>1] >> ((i&1)*4)) & 0xF;
                int hi = (qs[16 + (i>>3)] >> (i&7)) & 1;  /* rough high bit */
                int x = lo | (hi << 4);
                buf[b*QK5_0 + i] = (x - 16) * d;  /* 5bit signed-ish */
            }
        }
    } else if (m->ggml_type == 7 /* Q5_1 */) {
        /* basic support */
        const size_t QK5_1 = 32;
        size_t nblocks = (elems + QK5_1 - 1) / QK5_1;
        for (size_t b = 0; b < nblocks; b++) {
            uint16_t d16, m16;
            if (fread(&d16,2,1,g->f)!=1 || fread(&m16,2,1,g->f)!=1) return CCE_ERR_IO;
            float d = gguf_f16_to_f32(d16), m = gguf_f16_to_f32(m16);
            uint8_t qs[20];
            if (fread(qs,1,20,g->f)!=20) return CCE_ERR_IO;
            size_t this_b = ((b+1)*QK5_1 > elems)?(elems-b*QK5_1):QK5_1;
            for(size_t i=0; i<this_b; i++) {
                int lo = (qs[i>>1] >> ((i&1)*4)) & 0xF;
                int hi = (qs[16 + (i>>3)] >> (i & 7)) & 1;
                buf[b*QK5_1 + i] = (lo | (hi<<4)) * d + m;
            }
        }
    } else if (m->ggml_type == 12 /* GGML_TYPE_Q4_K */) {
        /* Q4_K: 256 vals/block. Layout: f16 d, f16 dmin, u8 scales[12], u8 qs[128] */
        const int QK_K = 256;
        size_t nblocks = (elems + QK_K - 1) / QK_K;
        for (size_t b = 0; b < nblocks; b++) {
            uint16_t d16, dmin16;
            if (fread(&d16,2,1,g->f)!=1 || fread(&dmin16,2,1,g->f)!=1) return CCE_ERR_IO;
            float d = gguf_f16_to_f32(d16);
            float dmin = gguf_f16_to_f32(dmin16);
            uint8_t scales[12];
            if (fread(scales,1,12,g->f)!=12) return CCE_ERR_IO;
            uint8_t qs[128];
            if (fread(qs,1,128,g->f)!=128) return CCE_ERR_IO;
            /* Simplified: use super scale, ignore per-group 6bit scales for polish */
            for (int i = 0; i < QK_K; i++) {
                int nib = (qs[i>>1] >> ((i&1)*4)) & 0xF;
                buf[b*QK_K + i] = nib * d + dmin;  /* placeholder, real uses is/sc */
            }
        }
    } else if (m->ggml_type == 13 /* Q5_K */) {
        /* Q5_K: f16 d, f16 dmin, scales[12], qs[160?] for 5bit */
        const int QK_K = 256;
        size_t nblocks = (elems + QK_K - 1) / QK_K;
        for (size_t b = 0; b < nblocks; b++) {
            uint16_t d16, dmin16;
            if (fread(&d16,2,1,g->f)!=1 || fread(&dmin16,2,1,g->f)!=1) return CCE_ERR_IO;
            float d = gguf_f16_to_f32(d16);
            float dmin = gguf_f16_to_f32(dmin16);
            uint8_t scales[12];
            fread(scales,1,12,g->f);
            uint8_t qs[160];
            size_t qbytes = (QK_K * 5 + 7) / 8; /* ~160 */
            if (fread(qs,1,qbytes,g->f) != qbytes) return CCE_ERR_IO;
            for (int i=0; i<QK_K; i++) {
                /* rough 5bit from layout */
                int lo = (qs[i/2] >> ((i%2)*4)) & 0xF;
                int hi = (qs[128 + (i/8)] >> (i%8)) & 1;
                int x = lo | (hi<<4);
                buf[b*QK_K + i] = x * d + dmin;
            }
        }
    } else {
        return CCE_ERR_UNSUPPORTED;
    }
    return CCE_OK;
}

cce_result cce_gguf_load_as_tensor(const cce_gguf* g, int idx, cce_tensor* out) {
    if (!g || idx < 0 || idx >= g->n_tensors || !out) return CCE_ERR_INVALID_ARG;

    cce_gguf_tensor_meta meta;
    cce_gguf_get_tensor_meta(g, idx, &meta);

    /* Build shape (GGUF shape[0] is usually the last logical dim for some, but for linear we transpose later) */
    int shape[CCE_MAX_DIMS] = {0};
    int nd = meta.ndim;
    /* GGUF stores dims in the order written; most HF converters write [out, in] for linears.
       We'll keep as-is and let populate decide transpose. */
    for (int d = 0; d < nd && d < CCE_MAX_DIMS; d++) {
        shape[d] = meta.shape[d];
    }

    if (cce_tensor_alloc(out, shape, nd) != CCE_OK) return CCE_ERR_OOM;

    cce_result rc = cce_gguf_load_f32(g, idx, out->data, out->numel);
    if (rc != CCE_OK) {
        cce_tensor_free(out);
        return rc;
    }
    return CCE_OK;
}

cce_result cce_gguf_load_tensor_by_name(const cce_gguf* g, const char* name, cce_tensor* out) {
    int idx = cce_gguf_find_tensor(g, name);
    if (idx < 0) return CCE_ERR_NOT_FOUND;
    return cce_gguf_load_as_tensor(g, idx, out);
}

/* Metadata accessors */
const char* cce_gguf_get_arch(const cce_gguf* g)     { return (g && g->arch[0]) ? g->arch : "unknown"; }
int cce_gguf_get_n_layer(const cce_gguf* g)          { return g ? g->n_layer : 0; }
int cce_gguf_get_hidden_size(const cce_gguf* g)      { return g ? g->hidden_size : 0; }
int cce_gguf_get_n_heads(const cce_gguf* g)          { return g ? g->n_heads : 0; }
int cce_gguf_get_n_kv_heads(const cce_gguf* g)       { return g ? g->n_kv_heads : 0; }
int cce_gguf_get_vocab_size(const cce_gguf* g)       { return g ? g->vocab_size : 0; }
int cce_gguf_get_context_length(const cce_gguf* g)   { return g ? g->context_length : 0; }
int cce_gguf_get_feed_forward_length(const cce_gguf* g) { return g ? g->feed_forward_length : 0; }
float cce_gguf_get_rope_freq_base(const cce_gguf* g) { return g ? g->rope_freq_base : 0.0f; }
float cce_gguf_get_rms_eps(const cce_gguf* g)        { return g ? g->rms_eps : 0.0f; }

const char* cce_gguf_get_tokenizer_model(const cce_gguf* g) { return (g && g->tokenizer_model[0]) ? g->tokenizer_model : "unknown"; }
int cce_gguf_get_bos_token_id(const cce_gguf* g) { return g ? g->bos_token_id : -1; }
int cce_gguf_get_eos_token_id(const cce_gguf* g) { return g ? g->eos_token_id : -1; }

/* Simple RMSNorm (no mean subtraction) */
static cce_result gguf_rms_norm(const cce_tensor* in, const cce_tensor* weight, float eps, cce_tensor* out) {
    if (!in || !out || !weight || in->numel != out->numel) return CCE_ERR_INVALID_ARG;
    int last = in->shape[in->ndim-1];
    if ((size_t)last > weight->numel) return CCE_ERR_INVALID_ARG; /* undersized norm tensor: refuse, don't overread */
    size_t rows = in->numel / last;
    for (size_t r = 0; r < rows; r++) {
        const float* row = in->data + r * last;
        float* orow = out->data + r * last;
        float ss = 0;
        for (int i=0; i<last; i++) ss += row[i]*row[i];
        ss = 1.0f / sqrtf(ss / last + eps);
        for (int i=0; i<last; i++) {
            orow[i] = (row[i] * ss) * weight->data[i];
        }
    }
    return CCE_OK;
}

/* Helper copied from Supra for applying cascade to rows */
/* Optional OpenCL fast path for the linear seam (see cce_clgemm.h). NULL =
   the CPU path below, byte-for-byte as always. Set via cce_gguf_set_clgemm. */
#include "../../include/cce/cce_clgemm.h"
static cce_clgemm *g_gguf_clgemm = NULL;
void cce_gguf_set_clgemm(cce_clgemm *h) { g_gguf_clgemm = h; }

static cce_result apply_linear_rows(cce_cascade* cas, const cce_tensor* in, cce_tensor* out) {
    if (!cas || !in || !out || in->ndim != 2 || out->ndim != 2) return CCE_ERR_INVALID_ARG;
    int T    = in->shape[0];
    int din  = in->shape[1];
    int dout = out->shape[1];
    if (out->shape[0] != T) return CCE_ERR_INVALID_ARG;

    /* GPU fast path: exactly the shape cce_gguf_add_linear_branch builds —
       ONE plain-float LINEAR_HEAD block (pure affine, no quantization).
       Anything else falls through to the CPU path unchanged. */
    if (g_gguf_clgemm && cas->num_blocks == 1 && T >= 1 && T <= 8) {
        const cce_block *blk = &cas->blocks[0];
        if (blk->type == CCE_BLOCK_LINEAR_HEAD && !blk->w_q && !blk->w_trit &&
            blk->weights.ndim == 2 && blk->weights.shape[0] == din &&
            blk->weights.shape[1] == dout) {
            const float *bias =
                (blk->bias.numel == (size_t)dout) ? blk->bias.data : NULL;
            if (cce_clgemm_matmul(g_gguf_clgemm, in->data, (size_t)T,
                                  (size_t)din, blk->weights.data, bias,
                                  (size_t)dout, out->data) == 0) {
                return CCE_OK;
            }
        }
    }

    cce_tensor row_in = {0};
    int ish[1] = { din };
    if (cce_tensor_alloc(&row_in, ish, 1) != CCE_OK) return CCE_ERR_OOM;

    for (int t = 0; t < T; t++) {
        memcpy(row_in.data, in->data + (size_t)t * din, (size_t)din * sizeof(float));
        cce_tensor row_out = {0};                  /* cascade_forward allocates this */
        cce_result r = cce_cascade_forward(cas, &row_in, &row_out);
        if (r != CCE_OK) { cce_tensor_free(&row_out); cce_tensor_free(&row_in); return r; }
        if (row_out.numel != (size_t)dout) { cce_tensor_free(&row_out); cce_tensor_free(&row_in); return CCE_ERR_UNSUPPORTED; }
        memcpy(out->data + (size_t)t * dout, row_out.data, (size_t)dout * sizeof(float));
        cce_tensor_free(&row_out);
    }
    cce_tensor_free(&row_in);
    return CCE_OK;
}

/* ---- Population (very similar to Supra / safetensors) ---- */

cce_result cce_gguf_populate_block(cce_block* blk, const cce_gguf* gguf,
                                   const char* weight_name, const char* bias_name,
                                   int transpose_weight) {
    if (!blk || !gguf || !weight_name) return CCE_ERR_INVALID_ARG;

    cce_tensor w = {0};
    if (cce_gguf_load_tensor_by_name(gguf, weight_name, &w) != CCE_OK || w.ndim != 2) {
        cce_tensor_free(&w);
        return CCE_ERR_IO;
    }

    /* GGUF records dims in ggml ne order (innermost/input first) — the
       REVERSE of torch — while the raw bytes stay row-major [out][in]
       (verified against the real gguf in Models/: ffn_up dims [n_embd, ffn]). */
    int file_in  = w.shape[0];
    int file_out = w.shape[1];
    int tgt_in   = file_in;
    int tgt_out  = file_out;
    (void)transpose_weight;

    if (blk->weights.numel == 0) {
        int wsh[2] = {tgt_in, tgt_out};
        if (cce_tensor_alloc(&blk->weights, wsh, 2) != CCE_OK) {
            cce_tensor_free(&w);
            return CCE_ERR_OOM;
        }
    }

    /* file [out, in] -> blk [in, out]  data[ii * out + oo] = w[oo * in + ii] */
    for (int ii = 0; ii < file_in; ii++) {
        for (int oo = 0; oo < file_out; oo++) {
            blk->weights.data[(size_t)ii * file_out + oo] = w.data[(size_t)oo * file_in + ii];
        }
    }

    cce_tensor_free(&w);

    if (bias_name && bias_name[0]) {
        cce_tensor b = {0};
        if (cce_gguf_load_tensor_by_name(gguf, bias_name, &b) == CCE_OK) {
            if (blk->bias.numel == 0) {
                int bsh[1] = { (int)(blk->weights.shape[1]) };
                cce_tensor_alloc(&blk->bias, bsh, 1);
            }
            memcpy(blk->bias.data, b.data, b.numel * sizeof(float));
            cce_tensor_free(&b);
        }
    }
    return CCE_OK;
}

int cce_gguf_add_linear_branch(cce_forest* forest, const cce_gguf* gguf,
                               const char* weight_name, const char* bias_name,
                               const char* branch_name, float init_scale_fallback) {
    if (!forest || !gguf || !weight_name || !branch_name) return -1;

    cce_tensor w = {0};
    if (cce_gguf_load_tensor_by_name(gguf, weight_name, &w) != CCE_OK || w.ndim != 2) {
        cce_tensor_free(&w);
        return -1;
    }
    /* ggml ne order: dims recorded innermost (input) first */
    int in_d  = w.shape[0];
    int out_d = w.shape[1];
    cce_tensor_free(&w);

    int is_head = (strstr(branch_name, "lm_head") != NULL) ? 1 : 1; /* always pure linear (no internal sigmoid) for projs/head */
    (void)is_head;

    cce_cascade* cas = NULL;
    if (cce_cascade_create(&cas, 2) != CCE_OK) return -1;

    /* Force HEAD for pure affine (transformer projs + final head must not sigmoid inside) */
    cce_result rc = cce_cascade_add_linear_head(cas, in_d, out_d, init_scale_fallback);
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
        return -1;
    }

    cce_block* blk = &cas->blocks[cas->num_blocks - 1];
    rc = cce_gguf_populate_block(blk, gguf, weight_name, (bias_name && bias_name[0]) ? bias_name : NULL, 1);
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
        return -1;
    }

    int idx = -1;
    rc = cce_forest_add_cascade_branch(forest, cas, branch_name, &idx);
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
        return -1;
    }
    return idx;
}

cce_result cce_gguf_build_qwen2_forest(cce_forest** out_forest, const cce_gguf* gguf, float init_scale) {
    if (!out_forest || !gguf) return CCE_ERR_INVALID_ARG;

    int n_layer = cce_gguf_get_n_layer(gguf);
    if (n_layer <= 0) n_layer = 24;

    cce_forest* forest = NULL;
    const char *tmpf = "gguf_qwen2_forest.cce";
    remove(tmpf);
    if (cce_forest_open(&forest, tmpf, 512) != CCE_OK) {
        return CCE_ERR_IO;
    }

    char wname[256], bname[256], brname[128];

    for (int l = 0; l < n_layer; l++) {
        // Attention projections (separate for GQA)
        snprintf(wname, sizeof(wname), "blk.%d.attn_q.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.attn_q.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.q_proj", l);
        cce_gguf_add_linear_branch(forest, gguf, wname, bname[0]?bname:NULL, brname, init_scale);

        snprintf(wname, sizeof(wname), "blk.%d.attn_k.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.attn_k.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.k_proj", l);
        cce_gguf_add_linear_branch(forest, gguf, wname, bname[0]?bname:NULL, brname, init_scale);

        snprintf(wname, sizeof(wname), "blk.%d.attn_v.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.attn_v.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.v_proj", l);
        cce_gguf_add_linear_branch(forest, gguf, wname, bname[0]?bname:NULL, brname, init_scale);

        snprintf(wname, sizeof(wname), "blk.%d.attn_output.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.attn_output.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.o_proj", l);
        cce_gguf_add_linear_branch(forest, gguf, wname, bname[0]?bname:NULL, brname, init_scale);

        // MLP SwiGLU
        snprintf(wname, sizeof(wname), "blk.%d.ffn_gate.weight", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.gate_proj", l);
        cce_gguf_add_linear_branch(forest, gguf, wname, NULL, brname, init_scale);

        snprintf(wname, sizeof(wname), "blk.%d.ffn_up.weight", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.up_proj", l);
        cce_gguf_add_linear_branch(forest, gguf, wname, NULL, brname, init_scale);

        snprintf(wname, sizeof(wname), "blk.%d.ffn_down.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.ffn_down.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.down_proj", l);
        cce_gguf_add_linear_branch(forest, gguf, wname, bname[0]?bname:NULL, brname, init_scale);
    }

    // Head only (tok_emb lookup stays in dedicated tensor; we do not need a linear branch for it)
    const char* head_name = (cce_gguf_find_tensor(gguf, "output.weight") >= 0) ? "output.weight" : "token_embd.weight";
    cce_gguf_add_linear_branch(forest, gguf, head_name, NULL, "qwen2.lm_head", 0.0f);

    /* MTP (nextn) specialists for gemma4-assistant style models */
    if (cce_gguf_find_tensor(gguf, "nextn.pre_projection.weight") >= 0) {
        cce_gguf_add_linear_branch(forest, gguf, "nextn.pre_projection.weight", NULL, "gemma4.mtp.pre_proj", 0.0f);
    }
    if (cce_gguf_find_tensor(gguf, "nextn.post_projection.weight") >= 0) {
        cce_gguf_add_linear_branch(forest, gguf, "nextn.post_projection.weight", NULL, "gemma4.mtp.post_proj", 0.0f);
    }

    *out_forest = forest;
    return CCE_OK;
}

/* Helper to get cascade by name (from supra style) */
static cce_cascade* get_cascade_by_name(cce_forest* f, const char* name) {
    for (int i = 0; i < f->num_branches; i++) {
        if (strcmp(f->branches[i].name, name) == 0) return f->branches[i].cascade;
    }
    return NULL;
}

/* Forward declaration for helper used in forward */
static cce_result apply_linear_rows(cce_cascade* cas, const cce_tensor* in, cce_tensor* out);

/* Basic Qwen2 forward glue (RMS + GQA + SwiGLU) - full per-layer with KV cache */
static void gguf_silu(const cce_tensor* in, cce_tensor* out) {
    for (size_t i = 0; i < in->numel; i++) {
        float x = in->data[i];
        out->data[i] = x / (1.0f + expf(-x));  // silu(x) = x * sigmoid(x)
    }
}

static void gguf_apply_rope(float* q, float* k, int t, int head_dim, int pos, float base, int n_heads, int n_kv_heads) {
    (void)n_heads; (void)n_kv_heads;
    for (int i = 0; i < head_dim; i += 2) {
        float freq = 1.0f / powf(base, (float)i / head_dim);
        float val = (float)pos * freq;
        float cosv = cosf(val);
        float sinv = sinf(val);
        if (q) {
            float q0 = q[t * head_dim + i];
            float q1 = q[t * head_dim + i + 1];
            q[t * head_dim + i] = q0 * cosv - q1 * sinv;
            q[t * head_dim + i + 1] = q0 * sinv + q1 * cosv;
        }
        if (k) {
            float k0 = k[t * head_dim + i];
            float k1 = k[t * head_dim + i + 1];
            k[t * head_dim + i] = k0 * cosv - k1 * sinv;
            k[t * head_dim + i + 1] = k0 * sinv + k1 * cosv;
        }
    }
}

cce_result cce_gguf_qwen2_forward(cce_gguf_qwen2* m, const int* tokens, int n_tokens, float* logits_out, int logits_cap) {
    if (!m || !tokens || n_tokens < 1 || !logits_out) return CCE_ERR_INVALID_ARG;
    if (!m->forest) return CCE_ERR_INVALID_ARG;

    int D = m->n_embd;
    int H = m->n_head;
    int KV = m->n_kv_head;
    int HD = m->head_dim;
    int V = m->vocab_size ? m->vocab_size : 151936;

    int start_pos = m->cur_pos;

    /* numerics: config/metadata overrides with safe defaults for calloc'd structs */
    float eps = (m->rms_eps > 0.0f) ? m->rms_eps : 1e-6f;
    float rope_base = (m->rope_freq_base > 0.0f) ? m->rope_freq_base : 10000.0f;

    cce_tensor x = {0};
    int xsh[2] = {n_tokens, D};  // process the new tokens
    if (cce_tensor_alloc(&x, xsh, 2) != CCE_OK) return CCE_ERR_OOM;

    /* Embed the new tokens */
    for (int t = 0; t < n_tokens; t++) {
        int tok = tokens[t];
        if (tok < 0 || tok >= V) tok = 0;
        memcpy(x.data + (size_t)t * D, m->tok_emb.data + (size_t)tok * D, D * sizeof(float));
    }

    char name[128];
    for (int l = 0; l < m->n_layer; l++) {
        snprintf(name, sizeof(name), "qwen2.blk.%d.q_proj", l);
        cce_cascade* q_cas = cce_forest_get_resident(m->forest, name);
        snprintf(name, sizeof(name), "qwen2.blk.%d.k_proj", l);
        cce_cascade* k_cas = cce_forest_get_resident(m->forest, name);
        snprintf(name, sizeof(name), "qwen2.blk.%d.v_proj", l);
        cce_cascade* v_cas = cce_forest_get_resident(m->forest, name);
        snprintf(name, sizeof(name), "qwen2.blk.%d.o_proj", l);
        cce_cascade* o_cas = cce_forest_get_resident(m->forest, name);
        snprintf(name, sizeof(name), "qwen2.blk.%d.gate_proj", l);
        cce_cascade* gate_cas = cce_forest_get_resident(m->forest, name);
        snprintf(name, sizeof(name), "qwen2.blk.%d.up_proj", l);
        cce_cascade* up_cas = cce_forest_get_resident(m->forest, name);
        snprintf(name, sizeof(name), "qwen2.blk.%d.down_proj", l);
        cce_cascade* down_cas = cce_forest_get_resident(m->forest, name);

        if (!q_cas || !o_cas || !gate_cas || !up_cas || !down_cas) {
            cce_tensor_free(&x);
            return CCE_ERR_NOT_FOUND;
        }
        bool has_kv = (k_cas && v_cas);

        cce_tensor ln1 = {0}, q = {0};
        int lnsh[2] = {n_tokens, D};
        cce_tensor_alloc(&ln1, lnsh, 2);
        gguf_rms_norm(&x, &m->attn_norm[l], eps, &ln1);

        int qsh[2] = {n_tokens, D};
        cce_tensor_alloc(&q, qsh, 2);
        apply_linear_rows(q_cas, &ln1, &q);

        cce_tensor k = {0}, v = {0};
        if (has_kv) {
            int kvsh[2] = {n_tokens, KV * HD};
            cce_tensor_alloc(&k, kvsh, 2);
            apply_linear_rows(k_cas, &ln1, &k);
            cce_tensor_alloc(&v, kvsh, 2);
            apply_linear_rows(v_cas, &ln1, &v);
        }

        /* RoPE on new tokens - per head for correct positional encoding (quality fix) */
        for (int t = 0; t < n_tokens; t++) {
            int pos = start_pos + t;
            // q heads always (even in fallback)
            for (int hh = 0; hh < H; hh++) {
                float* qh = q.data + (size_t)t * D + (size_t)hh * HD;
                gguf_apply_rope(qh, NULL, 0, HD, pos, rope_base, H, KV);
            }
            if (has_kv) {
                for (int hh = 0; hh < KV; hh++) {
                    float* kh = k.data + (size_t)t * (KV * HD) + (size_t)hh * HD;
                    gguf_apply_rope(NULL, kh, 0, HD, pos, rope_base, H, KV);
                }
            }
        }

        cce_tensor after_attn = {0};
        cce_tensor_alloc(&after_attn, lnsh, 2);

        if (has_kv) {
            /* Full GQA attention */
            cce_tensor attn_out = {0};
            cce_tensor_alloc(&attn_out, lnsh, 2);
            memset(attn_out.data, 0, (size_t)n_tokens * D * sizeof(float));

            int max_abs = start_pos + n_tokens - 1;
            if (max_abs < 0) max_abs = 0;
            float *scores = (float*)malloc( (size_t)(max_abs + 1) * sizeof(float) );
            if (!scores) scores = (float*)calloc(4096, sizeof(float));

            for (int h = 0; h < H; h++) {
                int kv_h = h / (H / KV);
                for (int t = 0; t < n_tokens; t++) {
                    int abs_t = start_pos + t;
                    float* qh = q.data + (size_t)t * D + h * HD;
                    int max_t = abs_t;
                    for (int j = 0; j <= max_t; j++) {
                        float s = 0.0f;
                        float* kh;
                        if (j < start_pos) {
                            kh = m->k_cache + (size_t)l * m->max_ctx * KV * HD + (size_t)j * KV * HD + kv_h * HD;
                        } else {
                            int jj = j - start_pos;
                            kh = k.data + (size_t)jj * (KV*HD) + kv_h * HD;
                        }
                        for (int d = 0; d < HD; d++) s += qh[d] * kh[d];
                        scores[j] = s / sqrtf((float)HD);
                    }
                    float maxs = -1e30f;
                    for (int j=0; j<=abs_t; j++) if (scores[j] > maxs) maxs = scores[j];
                    float sum = 0.0f;
                    for (int j=0; j<=abs_t; j++) { scores[j] = expf(scores[j] - maxs); sum += scores[j]; }
                    for (int j=0; j<=abs_t; j++) scores[j] /= sum;
                    float* oh = attn_out.data + (size_t)t * D + h * HD;
                    memset(oh, 0, HD * sizeof(float));
                    for (int j = 0; j <= abs_t; j++) {
                        float* vh = (j < start_pos) ?
                            m->v_cache + (size_t)l * m->max_ctx * KV * HD + (size_t)j * KV * HD + kv_h * HD :
                            v.data + (size_t)(j - start_pos) * (KV*HD) + kv_h * HD;
                        for (int d = 0; d < HD; d++) oh[d] += scores[j] * vh[d];
                    }
                }
            }
            free(scores);
            apply_linear_rows(o_cas, &attn_out, &after_attn);
            cce_tensor_free(&attn_out);
        } else {
            /* Fallback for Gemma4-style (q + o only): use q_proj output through o_proj.
               This lets the model run using its actual weights even without explicit k/v.
               Also apply extra norms/scales when present. */
            cce_tensor qn = {0};
            cce_tensor_alloc(&qn, qsh, 2);
            if (m->attn_q_norm && m->attn_q_norm[l].data) {
                /* per-head or simple norm on q */
                gguf_rms_norm(&q, &m->attn_q_norm[l], eps, &qn);
            } else {
                memcpy(qn.data, q.data, (size_t)n_tokens * D * sizeof(float));
            }
            apply_linear_rows(o_cas, &qn, &after_attn);
            cce_tensor_free(&qn);

            /* Apply post-attention norm and layer scale if present */
            if (m->post_attention_norm && m->post_attention_norm[l].data) {
                cce_tensor tmp = {0};
                cce_tensor_alloc(&tmp, lnsh, 2);
                gguf_rms_norm(&after_attn, &m->post_attention_norm[l], eps, &tmp);
                memcpy(after_attn.data, tmp.data, (size_t)n_tokens * D * sizeof(float));
                cce_tensor_free(&tmp);
            }
            if (m->layer_output_scale && m->layer_output_scale[l].data && m->layer_output_scale[l].numel > 0) {
                float sc = m->layer_output_scale[l].data[0];
                for (size_t i = 0; i < (size_t)n_tokens * D; i++) after_attn.data[i] *= sc;
            }
        }

        /* residual */
        for (size_t i = 0; i < (size_t)n_tokens * D; i++) {
            after_attn.data[i] += x.data[i];
        }

        /* Write current k/v to cache (only if present) */
        if (has_kv) {
            for (int t = 0; t < n_tokens; t++) {
                int abs_t = start_pos + t;
                for (int h = 0; h < KV; h++) {
                    float* kc = m->k_cache + (size_t)l * m->max_ctx * KV * HD + (size_t)abs_t * KV * HD + (size_t)h * HD;
                    float* vc = m->v_cache + (size_t)l * m->max_ctx * KV * HD + (size_t)abs_t * KV * HD + (size_t)h * HD;
                    memcpy(kc, k.data + (size_t)t * (KV*HD) + (size_t)h * HD, HD * sizeof(float));
                    memcpy(vc, v.data + (size_t)t * (KV*HD) + (size_t)h * HD, HD * sizeof(float));
                }
            }
        }

        /* MLP */
        cce_tensor ln2 = {0}, gate = {0}, upv = {0}, mid = {0}, down = {0};
        int mlp_hidden = m->feed_forward_length > 0 ? m->feed_forward_length : 4864;
        cce_tensor_alloc(&ln2, lnsh, 2);
        gguf_rms_norm(&after_attn, &m->ffn_norm[l], eps, &ln2);

        cce_tensor_alloc(&gate, (int[]){n_tokens, mlp_hidden}, 2);
        apply_linear_rows(gate_cas, &ln2, &gate);

        cce_tensor_alloc(&upv, (int[]){n_tokens, mlp_hidden}, 2);
        apply_linear_rows(up_cas, &ln2, &upv);

        cce_tensor_alloc(&mid, (int[]){n_tokens, mlp_hidden}, 2);
        gguf_silu(&gate, &gate);
        for (size_t i = 0; i < (size_t)n_tokens * mlp_hidden; i++) {
            mid.data[i] = gate.data[i] * upv.data[i];  // silu(gate) * up
        }

        cce_tensor_alloc(&down, lnsh, 2);
        apply_linear_rows(down_cas, &mid, &down);

        for (size_t i = 0; i < (size_t)n_tokens * D; i++) {
            x.data[i] = after_attn.data[i] + down.data[i];
        }

        // free layer temps
        cce_tensor_free(&ln1); cce_tensor_free(&q);
        if (has_kv) { cce_tensor_free(&k); cce_tensor_free(&v); }
        cce_tensor_free(&after_attn);
        cce_tensor_free(&ln2); cce_tensor_free(&gate); cce_tensor_free(&upv); cce_tensor_free(&mid); cce_tensor_free(&down);
    }

    /* final norm + head */
    cce_tensor fn = {0};
    cce_tensor_alloc(&fn, xsh, 2);
    gguf_rms_norm(&x, &m->output_norm, eps, &fn);

    cce_tensor logits_t = {0};
    int lsh[2] = {n_tokens, V};
    cce_tensor_alloc(&logits_t, lsh, 2);
    cce_cascade* head_cas = cce_forest_get_resident(m->forest, "qwen2.lm_head");
    bool head_ok = false;
    if (head_cas) {
        if (apply_linear_rows(head_cas, &fn, &logits_t) == CCE_OK) head_ok = true;
    }
    if (!head_ok && m->output.data && m->output.ndim == 2) {
        /* Manual head using stored output (tied or not) - transpose safe guess */
        int od0 = m->output.shape[0];
        int od1 = m->output.shape[1];
        float* last = fn.data + (size_t)(n_tokens-1) * D;
        for (int vi = 0; vi < V && vi < logits_cap; vi++) {
            float s = 0.0f;
            if (od1 == D && od0 >= V) {
                // output [V-ish, D]
                for (int d=0; d<D; d++) s += last[d] * m->output.data[(size_t)vi * D + d];
            } else if (od0 == D && od1 >= V) {
                for (int d=0; d<D; d++) s += last[d] * m->output.data[(size_t)d * od1 + vi];
            } else if (od0 >= V && od1 == D) {
                for (int d=0; d<D; d++) s += last[d] * m->output.data[(size_t)vi * D + d];
            }
            logits_t.data[vi] = s;
        }
    }

    /* output last token logits */
    int out_len = (V < logits_cap) ? V : logits_cap;
    memcpy(logits_out, logits_t.data + (size_t)(n_tokens-1) * V, out_len * sizeof(float));

    m->cur_pos += n_tokens;

    cce_tensor_free(&x); cce_tensor_free(&fn); cce_tensor_free(&logits_t);
    return CCE_OK;
}

/* Phase 4 dispatcher. The cross-format arch->builder registry lives in
 * cce_detect.c (cce_anymodel_open); this entry point builds attention
 * transformers (qwen2/llama/gemma-style keys) and structurally REFUSES
 * anything else (e.g. SSM/mamba ggufs route to cce_ssm_load) instead of
 * mis-running it through the wrong forward pass. */
cce_result cce_gguf_load_model(cce_gguf_qwen2** out, const char* path) {
    cce_gguf* g = NULL;
    cce_result rc = cce_gguf_load(path, &g);
    if (rc != CCE_OK) return rc;
    int has_attn = 0, has_ssm = 0;
    int nt = cce_gguf_tensor_count(g);
    for (int i = 0; i < nt; i++) {
        cce_gguf_tensor_meta m;
        if (cce_gguf_get_tensor_meta(g, i, &m) != CCE_OK) continue;
        if (strstr(m.name, "attn_q.weight")) has_attn = 1;
        if (strstr(m.name, ".ssm_") || strstr(m.name, "ssm_in")) has_ssm = 1;
    }
    cce_gguf_free(g);
    if (!has_attn || has_ssm) return CCE_ERR_UNSUPPORTED;
    return cce_gguf_load_qwen2(out, path);
}

/* Load full Qwen2 decomposed model */
cce_result cce_gguf_load_qwen2(cce_gguf_qwen2** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    cce_gguf* g = NULL;
    cce_result rc = cce_gguf_load(path, &g);
    if (rc != CCE_OK) return rc;

    cce_gguf_qwen2* m = (cce_gguf_qwen2*)calloc(1, sizeof(cce_gguf_qwen2));
    if (!m) { cce_gguf_free(g); return CCE_ERR_OOM; }

    m->n_layer = cce_gguf_get_n_layer(g);
    m->n_embd = cce_gguf_get_hidden_size(g);
    m->n_head = cce_gguf_get_n_heads(g);
    m->n_kv_head = cce_gguf_get_n_kv_heads(g);
    m->vocab_size = cce_gguf_get_vocab_size(g);
    if (m->n_kv_head == 0) m->n_kv_head = m->n_head;
    m->head_dim = (m->n_head > 0) ? (m->n_embd / m->n_head) : 64; /* guard against 0 from unknown arch key */

    /* Embed tokenizer data from GGUF (Phase 4) */
    m->bos_token_id = cce_gguf_get_bos_token_id(g);
    m->eos_token_id = cce_gguf_get_eos_token_id(g);
    strncpy(m->tokenizer_model, cce_gguf_get_tokenizer_model(g), sizeof(m->tokenizer_model)-1);

    rc = cce_gguf_build_qwen2_forest(&m->forest, g, 0.02f);
    if (rc != CCE_OK) {
        free(m);
        cce_gguf_free(g);
        return rc;
    }

    /* Load norms */
    m->attn_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    m->ffn_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    m->attn_q_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    m->post_attention_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    m->post_ffw_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    m->layer_output_scale = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    for (int l = 0; l < m->n_layer; l++) {
        char name[128];
        snprintf(name, sizeof(name), "blk.%d.attn_norm.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->attn_norm[l]);
        snprintf(name, sizeof(name), "blk.%d.ffn_norm.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->ffn_norm[l]);
        snprintf(name, sizeof(name), "blk.%d.attn_q_norm.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->attn_q_norm[l]);
        snprintf(name, sizeof(name), "blk.%d.post_attention_norm.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->post_attention_norm[l]);
        snprintf(name, sizeof(name), "blk.%d.post_ffw_norm.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->post_ffw_norm[l]);
        snprintf(name, sizeof(name), "blk.%d.layer_output_scale.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->layer_output_scale[l]);
    }

    cce_gguf_load_tensor_by_name(g, "rope_freqs.weight", &m->rope_freqs);

    cce_gguf_load_tensor_by_name(g, "token_embd.weight", &m->tok_emb);
    cce_gguf_load_tensor_by_name(g, "output_norm.weight", &m->output_norm);
    if (cce_gguf_find_tensor(g, "output.weight") >= 0) {
        cce_gguf_load_tensor_by_name(g, "output.weight", &m->output);
    } else {
        /* tied */
        cce_gguf_load_tensor_by_name(g, "token_embd.weight", &m->output);
    }

    cce_gguf_load_tensor_by_name(g, "nextn.pre_projection.weight", &m->mtp_pre);
    cce_gguf_load_tensor_by_name(g, "nextn.post_projection.weight", &m->mtp_post);

    /* Derive vocab if missing from KV (common for some GGUF) */
    if (m->vocab_size <= 0 && m->tok_emb.ndim == 2) {
        int s0 = m->tok_emb.shape[0], s1 = m->tok_emb.shape[1];
        if (s0 == m->n_embd) m->vocab_size = s1;
        else if (s1 == m->n_embd) m->vocab_size = s0;
        else m->vocab_size = (s0 > s1 ? s0 : s1);
    }

    m->ctx_len = cce_gguf_get_context_length(g);
    m->feed_forward_length = cce_gguf_get_feed_forward_length(g);
    m->rope_freq_base = cce_gguf_get_rope_freq_base(g); /* 0 -> forward default 10000 */
    m->rms_eps = cce_gguf_get_rms_eps(g);               /* 0 -> forward default 1e-6 */
    m->max_ctx = (m->ctx_len > 0) ? m->ctx_len : 2048;
    if (m->max_ctx > 8192) m->max_ctx = 8192; /* cap KV cache alloc for loader demo; real use would be dynamic / paged */
    m->cur_pos = 0;

    size_t kv_size = (size_t)m->n_layer * m->max_ctx * m->n_kv_head * m->head_dim;
    m->k_cache = (float*)calloc(kv_size, sizeof(float));
    m->v_cache = (float*)calloc(kv_size, sizeof(float));

    cce_gguf_free(g);  /* we don't need the raw loader anymore */
    *out = m;
    return CCE_OK;
}

int cce_gguf_qwen2_quantize_int8(cce_gguf_qwen2* m) {
    if (!m || !m->forest) return -1;
    int n = 0;
    for (int b = 0; b < m->forest->num_branches; ++b) {
        const char* bn = m->forest->branches[b].name;
        if (strstr(bn, "lm_head") || strstr(bn, "mtp.")) continue; // keep lm_head + MTP in FP for quality
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; ++j)
            if (cce_block_quantize_int8(&cas->blocks[j]) == CCE_OK) n++;
    }
    return n;
}

/* BitNet b1.58 style ternary PTQ over the linear specialists in the forest.
 * Post-hoc (no STE). Mirrors cce_supra_quantize_ternary exactly for the Qwen2 case.
 */
int cce_gguf_qwen2_quantize_ternary(cce_gguf_qwen2* m) {
    if (!m || !m->forest) return -1;
    int n = 0;
    for (int b = 0; b < m->forest->num_branches; ++b) {
        const char* bn = m->forest->branches[b].name;
        if (strstr(bn, "lm_head") || strstr(bn, "mtp.")) continue; // keep lm_head + MTP in FP for quality
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; ++j) {
            if (cce_block_quantize_ternary(&cas->blocks[j]) == CCE_OK) n++;
        }
    }
    return n;
}

/* Pack to 1.6-bit (ternary trits) like Supra.
 * If the blocks are not yet quantized to ternary, it will do so (for convenience).
 * Call quantize_ternary explicitly first if you want to run the int8/ternary
 * in-memory version before packing.
 */
cce_result cce_gguf_qwen2_pack_trits(cce_gguf_qwen2* m) {
    if (!m || !m->forest) return CCE_ERR_INVALID_ARG;
    for (int b = 0; b < m->forest->num_branches; ++b) {
        const char* bn = m->forest->branches[b].name;
        if (strstr(bn, "lm_head") || strstr(bn, "mtp.")) continue; // keep lm_head + MTP in FP for quality
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; ++j) {
            cce_block* blk = &cas->blocks[j];
            if (!blk->w_q && !blk->w_trit) cce_block_quantize_ternary(blk);
            cce_block_pack_trits(blk);
            /* Reclaim original FP32 weights after packing to 1.6-bit trits.
               This prevents huge resident memory for the dequantized specialists
               (embed + other FP parts are kept separately as needed). */
            if (blk->w_trit) {
                cce_tensor_free(&blk->weights);
                blk->weights.data = NULL;
                blk->weights.numel = 0;
            }
        }
    }
    return CCE_OK;
}

/* ---- Packed artifact I/O (mirrors supra packed flow, qwen2 hparams + FP norms/emb + trit specialists) ---- */
#define GGUF_QWEN2_PACK_MAGIC 0x504b4751u /* 'Q','G','K','P' */

static void gguf_write_tensor(FILE* f, const cce_tensor* t) {
    if (!t || !t->data || t->numel == 0) { int z = 0; fwrite(&z, sizeof(int), 1, f); return; }
    int nd = t->ndim; fwrite(&nd, sizeof(int), 1, f);
    fwrite(t->shape, sizeof(int), (size_t)nd, f);
    fwrite(t->data, sizeof(float), t->numel, f);
}

static cce_result gguf_read_tensor(FILE* f, cce_tensor* t) {
    int nd = 0;
    if (fread(&nd, sizeof(int), 1, f) != 1) return CCE_ERR_IO;
    if (nd == 0) {
        memset(t, 0, sizeof(*t));
        return CCE_OK;
    }
    if (nd < 1 || nd > CCE_MAX_DIMS) return CCE_ERR_IO;
    int sh[CCE_MAX_DIMS]; if (fread(sh, sizeof(int), (size_t)nd, f) != (size_t)nd) return CCE_ERR_IO;
    if (cce_tensor_alloc(t, sh, nd) != CCE_OK) return CCE_ERR_OOM;
    if (fread(t->data, sizeof(float), t->numel, f) != t->numel) return CCE_ERR_IO;
    return CCE_OK;
}

static cce_result add_gguf_packed_branch(cce_forest* fr, const char* name, int in, int out, int bpr,
                                         const uint8_t* trit, const float* scale, const float* bias) {
    if (!fr || fr->num_branches >= fr->max_branches) return CCE_ERR_INVALID_ARG;
    cce_cascade* cas = NULL;
    if (cce_cascade_create(&cas, 1) != CCE_OK) return CCE_ERR_OOM;
    cas->num_blocks = 1;
    cce_block* blk = &cas->blocks[0];
    memset(blk, 0, sizeof(*blk));
    /* Always pure affine for reloaded specialists (matches fresh load path) */
    blk->type = CCE_BLOCK_LINEAR_HEAD;
    blk->weights.ndim = 2; blk->weights.shape[0] = in; blk->weights.shape[1] = out;
    blk->weights.numel = (size_t)in * out; blk->weights.data = NULL; blk->weights.owns_memory = 0;
    int bsh[1] = { out };
    cce_tensor_alloc(&blk->bias, bsh, 1);
    if (bias) memcpy(blk->bias.data, bias, (size_t)out * sizeof(float));
    blk->w_trit = (uint8_t*)malloc((size_t)in * bpr);
    if (trit) memcpy(blk->w_trit, trit, (size_t)in * bpr);
    blk->w_trit_bpr = bpr;
    blk->w_scale = (float*)malloc((size_t)out * sizeof(float));
    if (scale) memcpy(blk->w_scale, scale, (size_t)out * sizeof(float));
    int idx = fr->num_branches++;
    fr->branches[idx].cascade = cas;
    snprintf(fr->branches[idx].name, sizeof(fr->branches[idx].name), "%s", name);
    fr->branches[idx].tier = CCE_TIER_HOT;
    fr->branches[idx].is_view = 0;
    fr->branches[idx].persisted = 0;
    return CCE_OK;
}

cce_result cce_gguf_qwen2_export_packed(cce_gguf_qwen2* m, const char* path) {
    if (!m || !path || !m->forest) return CCE_ERR_INVALID_ARG;
    cce_gguf_qwen2_pack_trits(m);
    FILE* f = fopen(path, "wb");
    if (!f) return CCE_ERR_IO;
    uint32_t magic = GGUF_QWEN2_PACK_MAGIC, ver = 2; /* v2 adds MTP pre/post slots */
    fwrite(&magic, 4, 1, f); fwrite(&ver, 4, 1, f);
    int hp[8] = { m->n_layer, m->n_embd, m->n_head, m->n_kv_head, m->head_dim, m->vocab_size, m->ctx_len, m->max_ctx };
    fwrite(hp, sizeof(int), 8, f);
    /* tokenizer (Phase 4) */
    fwrite(&m->bos_token_id, sizeof(int), 1, f);
    fwrite(&m->eos_token_id, sizeof(int), 1, f);
    int tlen = (int)strlen(m->tokenizer_model);
    fwrite(&tlen, sizeof(int), 1, f);
    if (tlen > 0) fwrite(m->tokenizer_model, 1, tlen, f);
    int nlay = m->n_layer; fwrite(&nlay, sizeof(int), 1, f);
    for (int l = 0; l < nlay; l++) gguf_write_tensor(f, &m->attn_norm[l]);
    for (int l = 0; l < nlay; l++) gguf_write_tensor(f, &m->ffn_norm[l]);
    for (int l = 0; l < nlay; l++) gguf_write_tensor(f, &m->attn_q_norm[l]);
    for (int l = 0; l < nlay; l++) gguf_write_tensor(f, &m->post_attention_norm[l]);
    for (int l = 0; l < nlay; l++) gguf_write_tensor(f, &m->post_ffw_norm[l]);
    for (int l = 0; l < nlay; l++) gguf_write_tensor(f, &m->layer_output_scale[l]);
    gguf_write_tensor(f, &m->tok_emb);
    gguf_write_tensor(f, &m->output_norm);
    gguf_write_tensor(f, &m->output);
    gguf_write_tensor(f, &m->mtp_pre);
    gguf_write_tensor(f, &m->mtp_post);
    gguf_write_tensor(f, &m->rope_freqs);
    int ns = m->forest->num_branches; fwrite(&ns, sizeof(int), 1, f);
    for (int b = 0; b < ns; b++) {
        cce_cascade* cas = m->forest->branches[b].cascade;
        cce_block* blk = (cas && cas->num_blocks > 0) ? &cas->blocks[0] : NULL;
        if (!blk || !blk->w_trit) {
            int z = 0; fwrite(&z, sizeof(int), 1, f);
            continue;
        }
        int valid = 1; fwrite(&valid, sizeof(int), 1, f);
        char nm[64] = {0}; snprintf(nm, sizeof(nm), "%s", m->forest->branches[b].name);
        fwrite(nm, 1, 64, f);
        int in = blk->weights.shape[0], out = blk->weights.shape[1], bpr = blk->w_trit_bpr;
        int dims[3] = {in, out, bpr}; fwrite(dims, sizeof(int), 3, f);
        fwrite(blk->w_trit, 1, (size_t)in * bpr, f);
        fwrite(blk->w_scale, sizeof(float), (size_t)out, f);
        if (blk->bias.data && blk->bias.numel == (size_t)out) {
            fwrite(blk->bias.data, sizeof(float), (size_t)out, f);
        } else {
            float* z = (float*)calloc((size_t)out, sizeof(float));
            fwrite(z, sizeof(float), (size_t)out, f); free(z);
        }
    }
    fclose(f);
    return CCE_OK;
}

cce_result cce_gguf_qwen2_load_packed(cce_gguf_qwen2** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    FILE* f = fopen(path, "rb"); if (!f) return CCE_ERR_IO;
    uint32_t magic = 0, ver = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != GGUF_QWEN2_PACK_MAGIC) { fclose(f); return CCE_ERR_UNSUPPORTED; }
    if (fread(&ver, 4, 1, f) != 1) { fclose(f); return CCE_ERR_IO; }
    cce_gguf_qwen2* m = (cce_gguf_qwen2*)calloc(1, sizeof(*m));
    if (!m) { fclose(f); return CCE_ERR_OOM; }
    int hp[8];
    if (fread(hp, sizeof(int), 8, f) != 8) { free(m); fclose(f); return CCE_ERR_IO; }
    m->n_layer = hp[0]; m->n_embd = hp[1]; m->n_head = hp[2]; m->n_kv_head = hp[3];
    m->head_dim = hp[4]; m->vocab_size = hp[5]; m->ctx_len = hp[6]; m->max_ctx = hp[7];
    if (m->n_kv_head == 0) m->n_kv_head = m->n_head;
    if (m->head_dim == 0 && m->n_head > 0) m->head_dim = m->n_embd / m->n_head;
    if (m->max_ctx <= 0) m->max_ctx = 2048;
    m->cur_pos = 0;
    /* tokenizer from packed (Phase 4) */
    fread(&m->bos_token_id, sizeof(int), 1, f);
    fread(&m->eos_token_id, sizeof(int), 1, f);
    int tlen = 0; fread(&tlen, sizeof(int), 1, f);
    if (tlen > 0 && tlen < (int)sizeof(m->tokenizer_model)) {
        fread(m->tokenizer_model, 1, tlen, f);
        m->tokenizer_model[tlen] = 0;
    }

    int nlay = 0;
    if (fread(&nlay, sizeof(int), 1, f) != 1) { free(m); fclose(f); return CCE_ERR_IO; }
    m->attn_norm = (cce_tensor*)calloc((size_t)nlay, sizeof(cce_tensor));
    m->ffn_norm  = (cce_tensor*)calloc((size_t)nlay, sizeof(cce_tensor));
    m->attn_q_norm = (cce_tensor*)calloc((size_t)nlay, sizeof(cce_tensor));
    m->post_attention_norm = (cce_tensor*)calloc((size_t)nlay, sizeof(cce_tensor));
    m->post_ffw_norm = (cce_tensor*)calloc((size_t)nlay, sizeof(cce_tensor));
    m->layer_output_scale = (cce_tensor*)calloc((size_t)nlay, sizeof(cce_tensor));
    for (int l = 0; l < nlay; l++) if (gguf_read_tensor(f, &m->attn_norm[l]) != CCE_OK) { cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO; }
    for (int l = 0; l < nlay; l++) if (gguf_read_tensor(f, &m->ffn_norm[l]) != CCE_OK) { cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO; }
    for (int l = 0; l < nlay; l++) (void)gguf_read_tensor(f, &m->attn_q_norm[l]);
    for (int l = 0; l < nlay; l++) (void)gguf_read_tensor(f, &m->post_attention_norm[l]);
    for (int l = 0; l < nlay; l++) (void)gguf_read_tensor(f, &m->post_ffw_norm[l]);
    for (int l = 0; l < nlay; l++) (void)gguf_read_tensor(f, &m->layer_output_scale[l]);
    if (gguf_read_tensor(f, &m->tok_emb) != CCE_OK ||
        gguf_read_tensor(f, &m->output_norm) != CCE_OK ||
        gguf_read_tensor(f, &m->output) != CCE_OK) {
        cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO;
    }

    if (ver >= 2) {
        (void)gguf_read_tensor(f, &m->mtp_pre);
        (void)gguf_read_tensor(f, &m->mtp_post);
    }
    (void)gguf_read_tensor(f, &m->rope_freqs);

    int ns = 0;
    if (fread(&ns, sizeof(int), 1, f) != 1) { cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO; }
    const char *tmp = "gguf_qwen2_packed.cce";
    remove(tmp);
    if (cce_forest_open(&m->forest, tmp, ns + 8) != CCE_OK) { cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO; }
    for (int b = 0; b < ns; b++) {
        int valid = 0;
        if (fread(&valid, sizeof(int), 1, f) != 1) { cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO; }
        if (!valid) continue;
        char nm[64] = {0};
        if (fread(nm, 1, 64, f) != 64) { cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO; }
        int dims[3] = {0};
        if (fread(dims, sizeof(int), 3, f) != 3) { cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO; }
        int inn = dims[0], outt = dims[1], bpr = dims[2];
        size_t trb = (size_t)inn * bpr;
        uint8_t* tr = (uint8_t*)malloc(trb);
        float*   sc = (float*)malloc((size_t)outt * sizeof(float));
        float*   bs = (float*)calloc((size_t)outt, sizeof(float));
        if (!tr || !sc || !bs || fread(tr, 1, trb, f) != trb ||
            fread(sc, sizeof(float), (size_t)outt, f) != (size_t)outt ||
            fread(bs, sizeof(float), (size_t)outt, f) != (size_t)outt) {
            free(tr); free(sc); free(bs);
            cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO;
        }
        if (add_gguf_packed_branch(m->forest, nm, inn, outt, bpr, tr, sc, bs) != CCE_OK) {
            free(tr); free(sc); free(bs);
            cce_gguf_qwen2_free(m); fclose(f); return CCE_ERR_IO;
        }
        free(tr); free(sc); free(bs);
    }

    // Phase 4 quality: ensure lm_head branch exists as FP (from m->output) since we skip packing it
    if (get_cascade_by_name(m->forest, "qwen2.lm_head") == NULL && m->output.data && m->output.ndim == 2) {
        int out_d = m->output.shape[0]; // V
        int in_d = m->output.shape[1];  // D
        cce_cascade* hcas = NULL;
        if (cce_cascade_create(&hcas, 1) == CCE_OK) {
            cce_cascade_add_linear_head(hcas, in_d, out_d, 0.0f);
            cce_block* hb = &hcas->blocks[0];
            // transpose copy: m->output [V, D] -> blk [D, V]
            for (int i = 0; i < in_d; i++) {
                for (int o = 0; o < out_d; o++) {
                    hb->weights.data[(size_t)i * out_d + o] = m->output.data[(size_t)o * in_d + i];
                }
            }
            cce_forest_add_cascade_branch(m->forest, hcas, "qwen2.lm_head", NULL);
        }
    }

    /* Ensure MTP branches exist as FP (from the stored mtp_* tensors) */
    const char* mtp_bnames[2] = {"gemma4.mtp.pre_proj", "gemma4.mtp.post_proj"};
    cce_tensor* mtp_ts[2] = {&m->mtp_pre, &m->mtp_post};
    for (int mi = 0; mi < 2; mi++) {
        if (get_cascade_by_name(m->forest, mtp_bnames[mi]) == NULL &&
            mtp_ts[mi]->data && mtp_ts[mi]->ndim == 2) {
            int out_d = mtp_ts[mi]->shape[0];
            int in_d  = mtp_ts[mi]->shape[1];
            cce_cascade* cas = NULL;
            if (cce_cascade_create(&cas, 1) == CCE_OK &&
                cce_cascade_add_linear_head(cas, in_d, out_d, 0.0f) == CCE_OK) {
                cce_block* b = &cas->blocks[0];
                for (int i = 0; i < in_d; i++) {
                    for (int o = 0; o < out_d; o++) {
                        b->weights.data[(size_t)i * out_d + o] = mtp_ts[mi]->data[(size_t)o * in_d + i];
                    }
                }
                cce_forest_add_cascade_branch(m->forest, cas, mtp_bnames[mi], NULL);
            }
        }
    }

    size_t kvsz = (size_t)m->n_layer * m->max_ctx * m->n_kv_head * m->head_dim;
    m->k_cache = (float*)calloc(kvsz, sizeof(float));
    m->v_cache = (float*)calloc(kvsz, sizeof(float));

    fclose(f);
    *out = m;
    return CCE_OK;
}

void cce_gguf_qwen2_free(cce_gguf_qwen2* m) {
    if (!m) return;
    if (m->forest) cce_forest_close(m->forest);
    // cleanup internal temp backing files to prevent junk accumulation
    remove("gguf_qwen2_forest.cce");
    remove("gguf_qwen2_packed.cce");
    remove("st_llama_forest.cce"); /* same struct built by cce_st_llama_load */
    if (m->attn_norm) {
        for (int l=0; l<m->n_layer; l++) cce_tensor_free(&m->attn_norm[l]);
        free(m->attn_norm);
    }
    if (m->ffn_norm) {
        for (int l=0; l<m->n_layer; l++) cce_tensor_free(&m->ffn_norm[l]);
        free(m->ffn_norm);
    }
    if (m->attn_q_norm) {
        for (int l=0; l<m->n_layer; l++) cce_tensor_free(&m->attn_q_norm[l]);
        free(m->attn_q_norm);
    }
    if (m->post_attention_norm) {
        for (int l=0; l<m->n_layer; l++) cce_tensor_free(&m->post_attention_norm[l]);
        free(m->post_attention_norm);
    }
    if (m->post_ffw_norm) {
        for (int l=0; l<m->n_layer; l++) cce_tensor_free(&m->post_ffw_norm[l]);
        free(m->post_ffw_norm);
    }
    if (m->layer_output_scale) {
        for (int l=0; l<m->n_layer; l++) cce_tensor_free(&m->layer_output_scale[l]);
        free(m->layer_output_scale);
    }
    cce_tensor_free(&m->tok_emb);
    cce_tensor_free(&m->output_norm);
    cce_tensor_free(&m->output);
    cce_tensor_free(&m->mtp_pre);
    cce_tensor_free(&m->mtp_post);
    cce_tensor_free(&m->rope_freqs);
    free(m->k_cache);
    free(m->v_cache);
    free(m);
}
