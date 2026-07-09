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
#include <sys/mman.h>   /* CNET_GGUF_MMAP: mmap/madvise/munmap */
/* 64-bit-safe seek: GGUF files exceed 2 GB and `long` is 32-bit on MinGW,
   hence _fseeki64 there; glibc spells it fseeko (needs _DEFAULT_SOURCE). */
#define _fseeki64(f, off, whence) fseeko((f), (off_t)(off), (whence))
#endif

/* Oracle memory mode (CNET_ORACLE_INT8=1): quantize each specialist to int8
   AT LOAD and drop its FP payload immediately. A 48-layer 12B FP32 forest
   (~50 GB) becomes ~12.5 GB — the difference between paging and running on a
   64 GB box. Shape metadata is kept because cce_block_forward derives dims
   from weights.shape; the int8 forward path returns before any FP access.
   Opt-in only: default loads are byte-identical to before. */
static int gguf_oracle_int8_mode(void) {
    const char* e = getenv("CNET_ORACLE_INT8");
    return e && e[0] == '1';
}

/* CNET_LOAD_TRACE=1: unbuffered per-stage load progress on stderr. Costs
   nothing when off; invaluable when a multi-GB load dies before stdout
   ever flushes. */
static int gguf_load_trace(void) {
    const char* e = getenv("CNET_LOAD_TRACE");
    return e && e[0] == '1';
}
#define GTRACE(...) do { if (gguf_load_trace()) { \
    fprintf(stderr, "[load] " __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } } while (0)

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
    /* numeric arrays (per-layer lists: sliding_window_pattern,
       head_count_kv, ...): retained as int64, owned. NULL for
       string/oversized arrays. */
    int64_t* arr;
    uint64_t arr_n;
} gguf_kv;

struct cce_gguf {
    FILE* f;
    void* map;         /* CNET_GGUF_MMAP=1: whole-file mmap backing g->f via
                          fmemopen — every dequant read faults from the shared
                          page cache instead of read()+stdio double-buffering,
                          and g->map is the direct DMA source for the resident-
                          quantized VRAM path (see spike/). NULL = FILE* path. */
    size_t map_size;
    char path[512];
    uint32_t version;
    uint64_t n_tensors_hdr;
    uint64_t n_kv_hdr;

    cce_gguf_tensor_meta* tensors;
    int n_tensors;

    gguf_kv* kvs;
    int n_kvs;

    uint32_t alignment;   /* general.alignment (default 32): tensor data
                             starts at the NEXT aligned boundary after the
                             tensor table, not at the raw file position */

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
            /* retain small NUMERIC arrays (per-layer architecture lists);
               strings and megal-arrays (tokenizer tables) are skipped */
            int keep = (elem_type != GGUF_TYPE_STRING && n <= 4096 && kv);
            if (keep) {
                kv->arr = (int64_t*)calloc((size_t)n, sizeof(int64_t));
                kv->arr_n = kv->arr ? n : 0;
            }
            for (uint64_t k=0; k < n; k++) {
                if (elem_type == GGUF_TYPE_STRING) {
                    uint64_t sl = 0; gguf_read_u64(f, &sl); fseek(f, (long)sl, SEEK_CUR);
                } else {
                    uint8_t buf[8] = {0};
                    size_t esz = 4;
                    if (elem_type==0 || elem_type==1 || elem_type==7) esz=1;
                    else if (elem_type==2||elem_type==3) esz=2;
                    else if (elem_type==10||elem_type==11||elem_type==12) esz=8;
                    if (keep && kv->arr) {
                        if (fread(buf, 1, esz, f) != esz) return false;
                        switch (elem_type) {
                        case GGUF_TYPE_UINT8:  kv->arr[k] = *(uint8_t*)buf; break;
                        case GGUF_TYPE_INT8:   kv->arr[k] = *(int8_t*)buf; break;
                        case GGUF_TYPE_BOOL:   kv->arr[k] = *(uint8_t*)buf ? 1 : 0; break;
                        case GGUF_TYPE_UINT16: kv->arr[k] = *(uint16_t*)buf; break;
                        case GGUF_TYPE_INT16:  kv->arr[k] = *(int16_t*)buf; break;
                        case GGUF_TYPE_UINT32: kv->arr[k] = *(uint32_t*)buf; break;
                        case GGUF_TYPE_INT32:  kv->arr[k] = *(int32_t*)buf; break;
                        case GGUF_TYPE_UINT64: kv->arr[k] = (int64_t)*(uint64_t*)buf; break;
                        case GGUF_TYPE_INT64:  kv->arr[k] = *(int64_t*)buf; break;
                        case GGUF_TYPE_FLOAT32: kv->arr[k] = (int64_t)*(float*)buf; break;
                        default: kv->arr[k] = 0; break;
                        }
                    } else {
                        fseek(f, (long)esz, SEEK_CUR);
                    }
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
    if (kv->arr) { free(kv->arr); kv->arr = NULL; kv->arr_n = 0; }
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

    /* Metadata. nk/nt come straight from the file header; a corrupt or
       hostile file can make them astronomically large. calloc then returns
       NULL and the record loops below would deref it — guard here. */
    g->kvs = nk ? (gguf_kv*)calloc((size_t)nk, sizeof(gguf_kv)) : NULL;
    if (nk && !g->kvs) { fclose(g->f); free(g); return CCE_ERR_OOM; }
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
    g->alignment = 32;
    for (int i = 0; i < g->n_kvs; i++) {
        const char* k = g->kvs[i].key;
        if (strcmp(k, "general.alignment") == 0) {
            if (g->kvs[i].type == GGUF_TYPE_UINT32 && g->kvs[i].val.u32 > 0)
                g->alignment = g->kvs[i].val.u32;
            else if (g->kvs[i].type == GGUF_TYPE_UINT64 && g->kvs[i].val.u64 > 0)
                g->alignment = (uint32_t)g->kvs[i].val.u64;
        }
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
    g->tensors = nt ? (cce_gguf_tensor_meta*)calloc((size_t)nt, sizeof(cce_gguf_tensor_meta)) : NULL;
    if (nt && !g->tensors) {
        for (int i = 0; i < g->n_kvs; i++) gguf_free_kv(&g->kvs[i]);
        free(g->kvs); fclose(g->f); free(g); return CCE_ERR_OOM;
    }
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
        /* shape[] is CCE_MAX_DIMS wide; a corrupt file can claim more dims.
           Clamp ndim so the elems loop below never reads past the array
           (the on-file dims beyond the cap are simply skipped). */
        if (nd > (uint32_t)CCE_MAX_DIMS) nd = (uint32_t)CCE_MAX_DIMS;
        t->ndim = (int)nd;
        for (uint32_t d = 0; d < nd; d++) {
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

    /* Data section starts at the NEXT general.alignment boundary (default
       32) after the tensor info table — NOT at the raw file position. The
       missing align-up read every tensor a few bytes shifted: garbage
       dequant, NaN streams, and every downstream gate (determinism,
       equivalence max|dlogit|, digests) satisfied VACUOUSLY by NaN. A file
       whose table happens to end aligned loads fine — which is how this
       survived the small-file tests while poisoning both campaign models. */
    long data_base = ftell(g->f);
    {
        uint64_t a = g->alignment ? g->alignment : 32;
        g->data_offset = ((uint64_t)data_base + a - 1) / a * a;
    }

    /* We don't close the file; keep it open for on-demand loads */

    /* Hop 1 of the mmap+DMA loader (CNET_GGUF_MMAP=1): back the load-time
       FILE* with a whole-file mmap via fmemopen. Every existing fseek/fread
       in cce_gguf_load_f32 then reads from the shared, page-cached mapping
       unchanged — byte-identical dequant, no read()/stdio double-buffer —
       and g->map becomes the direct DMA source for the resident-quantized
       VRAM forward. Fail-safe: any failure keeps the original FILE*. Parse
       above already ran on the real file, so only the load phase changes. */
    if (getenv("CNET_GGUF_MMAP") && getenv("CNET_GGUF_MMAP")[0] == '1') {
        long fsz = ftell(g->f);
        if (fseek(g->f, 0, SEEK_END) == 0) {
            long total = ftell(g->f);
            if (total > 0) {
                int fd = fileno(g->f);
                void* mp = mmap(NULL, (size_t)total, PROT_READ, MAP_SHARED, fd, 0);
                if (mp != MAP_FAILED) {
                    FILE* mf = fmemopen(mp, (size_t)total, "rb");
                    if (mf) {
                        madvise(mp, (size_t)total, MADV_WILLNEED);
                        fclose(g->f);
                        g->f = mf;
                        g->map = mp;
                        g->map_size = (size_t)total;
                        GTRACE("CNET_GGUF_MMAP: mapped %ld bytes, load reads "
                               "fault from page cache", total);
                    } else {
                        munmap(mp, (size_t)total);
                    }
                }
            }
        }
        if (!g->map) fseek(g->f, fsz, SEEK_SET);   /* restore on fallback */
    }

    *out = g;
    return CCE_OK;
}

/* Direct pointer to a tensor's raw (still-quantized) bytes inside the
   mmap — the single-hop DMA source for the resident-quantized VRAM path.
   Only available in CNET_GGUF_MMAP mode; returns CCE_ERR_UNSUPPORTED with
   the FILE* loader so callers fall back to cce_gguf_load_f32. */
cce_result cce_gguf_tensor_bytes(const cce_gguf* g, int idx,
                                 const void** ptr, size_t* nbytes) {
    if (!g || idx < 0 || idx >= g->n_tensors || !ptr || !nbytes)
        return CCE_ERR_INVALID_ARG;
    if (!g->map) return CCE_ERR_UNSUPPORTED;
    const cce_gguf_tensor_meta* m = &g->tensors[idx];
    uint64_t abs_off = g->data_offset + m->data_offset;
    size_t next = (idx + 1 < g->n_tensors)
        ? g->data_offset + g->tensors[idx + 1].data_offset
        : g->map_size;
    if (abs_off >= g->map_size || next > g->map_size || next <= abs_off)
        return CCE_ERR_IO;
    *ptr = (const uint8_t*)g->map + abs_off;
    *nbytes = (size_t)(next - abs_off);
    return CCE_OK;
}

void cce_gguf_free(cce_gguf* g) {
    if (!g) return;
    if (g->f) fclose(g->f);
    if (g->map) munmap(g->map, g->map_size);
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
    if (_fseeki64(g->f, (int64_t)abs_off, SEEK_SET) != 0) {
        GTRACE("load_f32 %s: seek FAILED abs_off=%llu", m->name, (unsigned long long)abs_off);
        return CCE_ERR_IO;
    }

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
            /* Correct Q4_K dequant (llama.cpp dequantize_row_q4_K): per 32-value
               sub-block scale/min are 6-bit-packed in scales[12] via the
               get_scale_min_k4 layout; value = d*sc*nib - dmin*m. The old code
               dropped the per-sub-block scales AND the -dmin*m term, yielding
               all-positive magnitudes -> a broken oracle. */
            const uint8_t *q = qs;
            int is = 0;
            size_t out = b * (size_t)QK_K;
            for (int j = 0; j < QK_K && out < elems; j += 64) {
                uint8_t sc, mm; int jj = is;
                if (jj < 4) { sc = scales[jj] & 63; mm = scales[jj+4] & 63; }
                else { sc = (scales[jj+4] & 0xF) | ((scales[jj-4] >> 6) << 4);
                       mm = (scales[jj+4] >> 4)  | ((scales[jj]   >> 6) << 4); }
                float d1 = d * sc, m1 = dmin * mm;
                jj = is + 1;
                if (jj < 4) { sc = scales[jj] & 63; mm = scales[jj+4] & 63; }
                else { sc = (scales[jj+4] & 0xF) | ((scales[jj-4] >> 6) << 4);
                       mm = (scales[jj+4] >> 4)  | ((scales[jj]   >> 6) << 4); }
                float d2 = d * sc, m2 = dmin * mm;
                for (int l = 0; l < 32 && out < elems; l++) buf[out++] = d1 * (q[l] & 0xF) - m1;
                for (int l = 0; l < 32 && out < elems; l++) buf[out++] = d2 * (q[l] >> 4)  - m2;
                q += 32; is += 2;
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
    } else if (m->ggml_type == 14 /* GGML_TYPE_Q6_K */) {
        /* Q6_K (exact llama.cpp block_q6_K): per 256-weight superblock
           ql[128] (low nibbles), qh[64] (high 2 bits), int8 scales[16]
           (one per 16 weights), f16 d — in THIS on-disk order (d LAST).
           w = d * scales[group] * (q6 - 32). Q4_K_M files use Q6_K for
           select attn_v/ffn_down tensors, so this type is REQUIRED to load
           real Q4_K_M checkpoints completely. */
        const int QK_K = 256;
        size_t nblocks = (elems + QK_K - 1) / QK_K;
        for (size_t b = 0; b < nblocks; b++) {
            uint8_t ql[128], qh[64];
            int8_t  sc[16];
            uint16_t d16;
            if (fread(ql, 1, 128, g->f) != 128) return CCE_ERR_IO;
            if (fread(qh, 1, 64, g->f) != 64)   return CCE_ERR_IO;
            if (fread(sc, 1, 16, g->f) != 16)   return CCE_ERR_IO;
            if (fread(&d16, 2, 1, g->f) != 1)   return CCE_ERR_IO;
            float d = gguf_f16_to_f32(d16);
            size_t base = b * QK_K;
            const uint8_t *pql = ql, *pqh = qh;
            const int8_t  *psc = sc;
            for (int n = 0; n < QK_K; n += 128) {
                for (int l = 0; l < 32; l++) {
                    int is = l / 16;
                    int q1 = (int)((pql[l +  0] & 0xF) | (((pqh[l] >> 0) & 3) << 4)) - 32;
                    int q2 = (int)((pql[l + 32] & 0xF) | (((pqh[l] >> 2) & 3) << 4)) - 32;
                    int q3 = (int)((pql[l +  0] >> 4)  | (((pqh[l] >> 4) & 3) << 4)) - 32;
                    int q4 = (int)((pql[l + 32] >> 4)  | (((pqh[l] >> 6) & 3) << 4)) - 32;
                    size_t o = base + (size_t)n + l;
                    if (o +  0 < elems) buf[o +  0] = d * psc[is + 0] * q1;
                    if (o + 32 < elems) buf[o + 32] = d * psc[is + 2] * q2;
                    if (o + 64 < elems) buf[o + 64] = d * psc[is + 4] * q3;
                    if (o + 96 < elems) buf[o + 96] = d * psc[is + 6] * q4;
                }
                pql += 64; pqh += 32; psc += 8;
            }
        }
    } else {
        GTRACE("load_f32 %s: UNSUPPORTED ggml_type=%d", m->name, m->ggml_type);
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

/* Per-layer architecture list (e.g. "attention.head_count_kv",
   "attention.sliding_window_pattern"): copies up to cap values, returns
   the count found (0 = key absent or non-numeric/oversized array). Keys are
   arch-prefixed in the file, so match by suffix. */
size_t cce_gguf_get_int_array(const cce_gguf* g, const char* key_suffix,
                              int64_t* out, size_t cap) {
    if (!g || !key_suffix || !out) return 0;
    size_t sl = strlen(key_suffix);
    for (int i = 0; i < g->n_kvs; i++) {
        const char* k = g->kvs[i].key;
        size_t kl = strlen(k);
        if (kl >= sl && strcmp(k + kl - sl, key_suffix) == 0 &&
            g->kvs[i].arr && g->kvs[i].arr_n > 0) {
            size_t n = g->kvs[i].arr_n < cap ? g->kvs[i].arr_n : cap;
            memcpy(out, g->kvs[i].arr, n * sizeof(int64_t));
            return n;
        }
    }
    return 0;
}

/* Scalar metadata by key suffix (arch-prefixed keys); fallback if absent. */
static double gguf_get_scalar(const cce_gguf* g, const char* key_suffix,
                              double fallback) {
    size_t sl = strlen(key_suffix);
    for (int i = 0; i < g->n_kvs; i++) {
        const char* k = g->kvs[i].key;
        size_t kl = strlen(k);
        if (kl >= sl && strcmp(k + kl - sl, key_suffix) == 0) {
            switch (g->kvs[i].type) {
            case GGUF_TYPE_UINT32: return g->kvs[i].val.u32;
            case GGUF_TYPE_INT32:  return g->kvs[i].val.i32;
            case GGUF_TYPE_UINT64: return (double)g->kvs[i].val.u64;
            case GGUF_TYPE_FLOAT32: return g->kvs[i].val.f32;
            default: break;
            }
        }
    }
    return fallback;
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

/* ---- Per-layer attention geometry (see cce_attn_geom in the header) ----
   Derived from TENSOR SHAPES first (the file's bytes are the truth),
   metadata second (layer-type lists and head widths), REFUSING on any
   inconsistency — a mis-dimensioned architecture must never forward.
   Motivation: gemma4 varies head_dim per layer type (swa 256 / global 512),
   runs asymmetric K/V head counts (MQA k=1, v=4 on global layers), and the
   old uniform-dims attention silently consumed uninitialized buffers. */
static cce_result gguf_build_attn_geom(cce_gguf_qwen2* m, const cce_gguf* g) {
    int L = m->n_layer;
    int D = m->n_embd;
    const char* arch = cce_gguf_get_arch(g);
    int64_t pattern[512];
    size_t pat_n;
    double klen, klen_swa, vlen, vlen_swa, window, fb, fb_swa, rdim, rdim_swa;
    int l;

    if (L <= 0 || D <= 0) return CCE_ERR_INVALID_ARG;
    m->geom = (struct cce_attn_geom*)calloc((size_t)L, sizeof *m->geom);
    if (!m->geom) return CCE_ERR_OOM;

    pat_n = cce_gguf_get_int_array(g, "attention.sliding_window_pattern",
                                   pattern, 512);
    klen     = gguf_get_scalar(g, "attention.key_length", 0);
    klen_swa = gguf_get_scalar(g, "attention.key_length_swa", klen);
    vlen     = gguf_get_scalar(g, "attention.value_length", klen);
    vlen_swa = gguf_get_scalar(g, "attention.value_length_swa", klen_swa);
    window   = gguf_get_scalar(g, "attention.sliding_window", 0);
    fb       = (m->rope_freq_base > 0.0f) ? m->rope_freq_base : 10000.0f;
    fb_swa   = gguf_get_scalar(g, "rope.freq_base_swa", fb);
    rdim     = gguf_get_scalar(g, "rope.dimension_count", 0);
    rdim_swa = gguf_get_scalar(g, "rope.dimension_count_swa", rdim);

    m->embed_scale = (strncmp(arch, "gemma", 5) == 0) ? sqrtf((float)D) : 1.0f;
    m->final_softcap = (float)gguf_get_scalar(g, "final_logit_softcapping", 0);

    m->k_slot_floats = 0;
    m->v_slot_floats = 0;
    for (l = 0; l < L; l++) {
        struct cce_attn_geom* ge = &m->geom[l];
        char tn[128];
        cce_gguf_tensor_meta tm;
        int idx;
        double hd, vhd;

        snprintf(tn, sizeof tn, "blk.%d.attn_q.weight", l);
        idx = cce_gguf_find_tensor(g, tn);
        if (idx < 0 || cce_gguf_get_tensor_meta(g, idx, &tm) != CCE_OK ||
            tm.ndim != 2 || tm.shape[0] != D) {
            fprintf(stderr, "attn geom: layer %d has no usable attn_q "
                            "(shared-KV drafts are not standalone models) — "
                            "refusing load\n", l);
            return CCE_ERR_UNSUPPORTED;
        }
        ge->q_dim = tm.shape[1];

        snprintf(tn, sizeof tn, "blk.%d.attn_k.weight", l);
        idx = cce_gguf_find_tensor(g, tn);
        if (idx >= 0 && cce_gguf_get_tensor_meta(g, idx, &tm) == CCE_OK &&
            tm.ndim == 2 && tm.shape[0] == D)
            ge->k_dim = tm.shape[1];
        snprintf(tn, sizeof tn, "blk.%d.attn_v.weight", l);
        idx = cce_gguf_find_tensor(g, tn);
        if (idx >= 0 && cce_gguf_get_tensor_meta(g, idx, &tm) == CCE_OK &&
            tm.ndim == 2 && tm.shape[0] == D)
            ge->v_dim = tm.shape[1];
        if (ge->k_dim > 0 && ge->v_dim <= 0) {
            /* gemma4 global layers ship K but NO V tensor. The only
               decomposition consistent with o_proj width (n_q*value_length)
               and head_count_kv is V TIED to the K projection (its raw,
               pre-norm/pre-rope output) — an inference, flagged in the spec
               pending external ground truth. */
            ge->v_dim = ge->k_dim;
            ge->v_tied = 1;
        }
        if (ge->k_dim <= 0 || ge->v_dim <= 0) {
            fprintf(stderr, "attn geom: layer %d lacks attn_k/attn_v — "
                            "refusing load (KV-sharing archs unsupported)\n",
                    l);
            return CCE_ERR_UNSUPPORTED;
        }

        ge->swa = (pat_n > (size_t)l) ? (pattern[l] != 0) : 0;
        hd  = ge->swa ? klen_swa : klen;
        vhd = ge->swa ? vlen_swa : vlen;
        if (hd <= 0) {  /* legacy qwen2/llama: no key_length metadata */
            hd = (m->head_dim > 0) ? m->head_dim
                                   : (m->n_head > 0 ? D / m->n_head : 0);
            vhd = hd;
        }
        ge->head_dim = (int)hd;
        ge->v_head_dim = (int)vhd;
        if (ge->head_dim <= 0 || ge->v_head_dim <= 0 ||
            ge->q_dim % ge->head_dim || ge->k_dim % ge->head_dim ||
            ge->v_dim % ge->v_head_dim) {
            fprintf(stderr, "attn geom: layer %d dims not divisible "
                            "(q=%d k=%d v=%d hd=%d vhd=%d) — refusing\n", l,
                    ge->q_dim, ge->k_dim, ge->v_dim, ge->head_dim,
                    ge->v_head_dim);
            return CCE_ERR_UNSUPPORTED;
        }
        ge->n_q = ge->q_dim / ge->head_dim;
        ge->n_k = ge->k_dim / ge->head_dim;
        ge->n_v = ge->v_dim / ge->v_head_dim;
        if (ge->n_k <= 0 || ge->n_v <= 0 || ge->n_q % ge->n_k ||
            ge->n_q % ge->n_v) {
            fprintf(stderr, "attn geom: layer %d head grouping invalid "
                            "(nq=%d nk=%d nv=%d) — refusing\n", l, ge->n_q,
                    ge->n_k, ge->n_v);
            return CCE_ERR_UNSUPPORTED;
        }

        /* o_proj must consume the concatenated V slices */
        snprintf(tn, sizeof tn, "blk.%d.attn_output.weight", l);
        idx = cce_gguf_find_tensor(g, tn);
        if (idx >= 0 && cce_gguf_get_tensor_meta(g, idx, &tm) == CCE_OK &&
            tm.ndim == 2 &&
            tm.shape[0] != ge->n_q * ge->v_head_dim) {
            fprintf(stderr, "attn geom: layer %d o_proj in=%d != nq*vhd=%d "
                            "— refusing\n", l, tm.shape[0],
                    ge->n_q * ge->v_head_dim);
            return CCE_ERR_UNSUPPORTED;
        }

        /* per-layer q/k norms (when present) must match head width */
        if (m->attn_q_norm && m->attn_q_norm[l].data &&
            m->attn_q_norm[l].numel != (size_t)ge->head_dim) {
            fprintf(stderr, "attn geom: layer %d q_norm numel %zu != "
                            "head_dim %d — refusing\n", l,
                    m->attn_q_norm[l].numel, ge->head_dim);
            return CCE_ERR_UNSUPPORTED;
        }
        if (m->attn_k_norm && m->attn_k_norm[l].data &&
            m->attn_k_norm[l].numel != (size_t)ge->head_dim) {
            fprintf(stderr, "attn geom: layer %d k_norm numel %zu != "
                            "head_dim %d — refusing\n", l,
                    m->attn_k_norm[l].numel, ge->head_dim);
            return CCE_ERR_UNSUPPORTED;
        }

        ge->rope_base = (float)(ge->swa ? fb_swa : fb);
        ge->rope_dim = (int)(ge->swa ? rdim_swa : rdim);
        if (ge->rope_dim <= 0 || ge->rope_dim > ge->head_dim)
            ge->rope_dim = ge->head_dim;
        ge->window = ge->swa ? (int)window : 0;

        ge->k_off = m->k_slot_floats;
        ge->v_off = m->v_slot_floats;
        m->k_slot_floats += (size_t)ge->k_dim;
        m->v_slot_floats += (size_t)ge->v_dim;
    }
    return CCE_OK;
}

/* Uniform geometry for models populated outside the GGUF loader (see
   header). Trusts the scalar hparams the caller already validated. */
cce_result cce_gguf_qwen2_geom_uniform(cce_gguf_qwen2* m) {
    int l;
    if (!m || m->n_layer <= 0 || m->n_head <= 0) return CCE_ERR_INVALID_ARG;
    if (m->geom) { free(m->geom); m->geom = NULL; }
    m->geom = (struct cce_attn_geom*)calloc((size_t)m->n_layer,
                                            sizeof *m->geom);
    if (!m->geom) return CCE_ERR_OOM;
    m->k_slot_floats = 0;
    m->v_slot_floats = 0;
    if (m->embed_scale <= 0.0f) m->embed_scale = 1.0f;
    for (l = 0; l < m->n_layer; l++) {
        struct cce_attn_geom* ge = &m->geom[l];
        int hd = (m->head_dim > 0) ? m->head_dim : m->n_embd / m->n_head;
        int kv = (m->n_kv_head > 0) ? m->n_kv_head : m->n_head;
        ge->head_dim = hd;
        ge->v_head_dim = hd;
        ge->n_q = m->n_head;
        ge->n_k = kv;
        ge->n_v = kv;
        ge->q_dim = m->n_head * hd;
        ge->k_dim = kv * hd;
        ge->v_dim = kv * hd;
        ge->swa = 0;
        ge->window = 0;
        ge->rope_base = (m->rope_freq_base > 0.0f) ? m->rope_freq_base
                                                   : 10000.0f;
        ge->rope_dim = hd;
        ge->k_off = m->k_slot_floats;
        ge->v_off = m->v_slot_floats;
        m->k_slot_floats += (size_t)ge->k_dim;
        m->v_slot_floats += (size_t)ge->v_dim;
        if (ge->n_q % ge->n_k) return CCE_ERR_INVALID_ARG;
    }
    return CCE_OK;
}

/* Simple RMSNorm (no mean subtraction) */
static cce_result gguf_rms_norm_impl(const cce_tensor* in, const cce_tensor* weight, float eps, cce_tensor* out, int add_one) {
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
            /* Use the stored weight DIRECTLY. gemma's (1+w) is already baked
               into the GGUF norm tensors by the llama.cpp converter
               (convert_hf_to_gguf adds 1). Adding it again here double-counts
               and blows up q*k -> attention collapses to self. */
            (void)add_one;
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
void cce_gguf_qwen2_set_head_window(cce_gguf_qwen2 *m, const int *ids, int n) {
    if (!m) return;
    m->head_window = (n > 0) ? ids : NULL;
    m->head_window_n = (n > 0) ? n : 0;
}
void cce_gguf_qwen2_set_clgemm(cce_gguf_qwen2 *m, cce_clgemm *h) {
    if (m) m->clgemm = h;
}

/* Depth instrumentation: an optional tap called after every layer with the
   residual stream (probe tooling measures at which depth the DECISIONS the
   oracle consumes stop changing). Also honors m->layer_cap (> 0): the loop
   stops after that many layers and the head reads the current stream. A
   capped forward is a DIFFERENT function from the full model — any caller
   enabling it MUST gate decision-equivalence against the full depth and
   refuse on mismatch, exactly like the GPU path (see depth_probe and the
   flagship startup sweep). Default 0 = full depth, byte-identical. */
typedef void (*cce_gguf_layer_tap_fn)(int layer, const float *x, int n_tokens,
                                      int dim, void *uctx);
static cce_gguf_layer_tap_fn g_gguf_layer_tap = NULL;
static void *g_gguf_layer_tap_ctx = NULL;
void cce_gguf_set_layer_tap(cce_gguf_layer_tap_fn fn, void *uctx) {
    g_gguf_layer_tap = fn;
    g_gguf_layer_tap_ctx = uctx;
}

static cce_result apply_linear_rows(cce_clgemm *gpu, cce_cascade* cas,
                                    const cce_tensor* in, cce_tensor* out) {
    if (!cas || !in || !out || in->ndim != 2 || out->ndim != 2) return CCE_ERR_INVALID_ARG;
    int T    = in->shape[0];
    int din  = in->shape[1];
    int dout = out->shape[1];
    if (out->shape[0] != T) return CCE_ERR_INVALID_ARG;

    /* GPU fast path: exactly the shapes cce_gguf_add_linear_branch builds —
       ONE LINEAR_HEAD block, plain-float OR int8 weight-only (Rung 5: the
       q8 kernel reproduces cce_block's int8 matvec bit-exactly, so the 11GB
       of int8 layer weights stream from ~640 GB/s GDDR6 instead of DDR5).
       Anything else falls through to the CPU path unchanged. */
    if (gpu && cas->num_blocks == 1 && T >= 1 && T <= 8) {
        const cce_block *blk = &cas->blocks[0];
        if (blk->type == CCE_BLOCK_LINEAR_HEAD && blk->w_q && blk->w_scale &&
            !blk->w_trit && blk->weights.ndim == 2 &&
            blk->weights.shape[0] == din && blk->weights.shape[1] == dout &&
            !(getenv("CNET_GPU_INT8") && getenv("CNET_GPU_INT8")[0] == '0')) {
            const float *bias =
                (blk->bias.numel == (size_t)dout) ? blk->bias.data : NULL;
            if (cce_clgemm_matmul_q8(gpu, in->data, (size_t)T, (size_t)din,
                                     (const signed char *)blk->w_q,
                                     blk->w_scale, bias, (size_t)dout,
                                     out->data) == 0) {
                static int verify_q8 = -1;
                if (verify_q8 < 0) {
                    const char *e = getenv("CNET_GPU_VERIFY");
                    verify_q8 = (e && e[0] == '1') ? 1 : 0;
                }
                if (verify_q8) {
                    int t2, o2, bad = 0;
                    for (t2 = 0; t2 < T && !bad; ++t2) {
                        for (o2 = 0; o2 < dout && !bad; ++o2) {
                            float acc = 0.0f;
                            int k2;
                            const float *arow = in->data + (size_t)t2 * din;
                            for (k2 = 0; k2 < din; ++k2)
                                acc += arow[k2] *
                                       (float)blk->w_q[(size_t)k2 * dout + o2];
                            acc = (bias ? bias[o2] : 0.0f) +
                                  blk->w_scale[o2] * acc;
                            if (out->data[(size_t)t2 * dout + o2] != acc) {
                                fprintf(stderr,
                                        "GPU_VERIFY: Q8 DIVERGE [%dx%d] "
                                        "t=%d o=%d gpu=%.9g cpu=%.9g\n",
                                        din, dout, t2, o2,
                                        (double)out->data[(size_t)t2 * dout + o2],
                                        (double)acc);
                                bad = 1;
                            }
                        }
                    }
                }
                return CCE_OK;
            }
        }
        if (blk->type == CCE_BLOCK_LINEAR_HEAD && !blk->w_q && !blk->w_trit &&
            blk->weights.ndim == 2 && blk->weights.shape[0] == din &&
            blk->weights.shape[1] == dout) {
            const float *bias =
                (blk->bias.numel == (size_t)dout) ? blk->bias.data : NULL;
            static int verify0 = -1;
            if (verify0 < 0) {
                const char *e0 = getenv("CNET_GPU_VERIFY");
                verify0 = (e0 && e0[0] == '1') ? 1 : 0;
            }
            if (cce_clgemm_matmul(gpu, in->data, (size_t)T,
                                  (size_t)din, blk->weights.data, bias,
                                  (size_t)dout, out->data) != 0) {
                if (verify0)
                    fprintf(stderr, "GPU_VERIFY: FALLBACK [%dx%d]\n", din,
                            dout);
            } else {
                if (verify0) {
                    /* GPU-vs-GPU: same inputs twice — catches call-to-call
                       nondeterminism the per-call CPU check cannot */
                    float *c2 = (float *)malloc((size_t)T * dout *
                                                sizeof *c2);
                    if (c2 && cce_clgemm_matmul(gpu, in->data, (size_t)T,
                                                (size_t)din,
                                                blk->weights.data, bias,
                                                (size_t)dout, c2) == 0) {
                        size_t q2;
                        for (q2 = 0; q2 < (size_t)T * dout; ++q2)
                            if (c2[q2] != out->data[q2]) {
                                fprintf(stderr,
                                        "GPU_VERIFY: NONDET [%dx%d] at %zu "
                                        "run1=%.9g run2=%.9g\n", din, dout,
                                        q2, (double)out->data[q2],
                                        (double)c2[q2]);
                                break;
                            }
                    }
                    free(c2);
                }
                /* CNET_GPU_VERIFY=1: recompute on CPU and report the first
                   diverging element per call — the live-fire localizer for
                   any GPU-vs-CPU drift (kernel is FP_CONTRACT OFF, so the
                   contract is BIT-identity, not closeness). */
                static int verify = -1;
                if (verify < 0) {
                    const char *e = getenv("CNET_GPU_VERIFY");
                    verify = (e && e[0] == '1') ? 1 : 0;
                }
                if (verify) {
                    int t2, o2, bad = 0;
                    for (t2 = 0; t2 < T && !bad; ++t2) {
                        for (o2 = 0; o2 < dout && !bad; ++o2) {
                            float acc = bias ? bias[o2] : 0.0f;
                            int k2;
                            const float *arow = in->data + (size_t)t2 * din;
                            for (k2 = 0; k2 < din; ++k2)
                                acc += arow[k2] *
                                       blk->weights.data[(size_t)k2 * dout + o2];
                            if (out->data[(size_t)t2 * dout + o2] != acc) {
                                fprintf(stderr,
                                        "GPU_VERIFY: DIVERGE [%dx%d] t=%d "
                                        "o=%d gpu=%.9g cpu=%.9g W=%p\n",
                                        din, dout, t2, o2,
                                        (double)out->data[(size_t)t2 * dout + o2],
                                        (double)acc,
                                        (const void *)blk->weights.data);
                                bad = 1;
                            }
                        }
                    }
                }
                return CCE_OK;
            }
        }
    }

    cce_tensor row_in0 = {0};
    (void)row_in0;
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
    cce_result lrc = cce_gguf_load_tensor_by_name(gguf, weight_name, &w);
    if (lrc != CCE_OK || w.ndim != 2) {
        int ti = cce_gguf_find_tensor(gguf, weight_name);
        cce_gguf_tensor_meta tm; tm.ggml_type = 0xFFFF;
        if (ti >= 0) cce_gguf_get_tensor_meta(gguf, ti, &tm);
        GTRACE("branch %s: dims pre-read FAILED (%s rc=%d ndim=%d ggml_type=%u)",
               branch_name, weight_name, (int)lrc, w.ndim, tm.ggml_type);
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
        GTRACE("branch %s: populate FAILED rc=%d (in=%d out=%d)", branch_name, (int)rc, in_d, out_d);
        cce_cascade_destroy(cas);
        return -1;
    }

    /* Oracle int8 mode: quantize BEFORE the branch add, so the forest never
       sees (and never tries to archive-persist) the FP payload. The add's
       persistence step then refuses cleanly (payload-less block) and the
       branch lives HOT in RAM as int8 — which is all a mining oracle needs.
       This also avoids writing a ~50 GB FP archive for a 12B model.
       lm_head + MTP stay FP — the SAME skip the post-hoc quantizers make
       "for quality": the head is the decision maker the campaign extracts,
       and int8-ing it perturbs exactly the rankings being mined. (FP head
       also makes it clgemm-eligible again, so it runs on the GPUs.) */
    if (gguf_oracle_int8_mode() && !strstr(branch_name, "lm_head") &&
        !strstr(branch_name, "mtp.")) {
        for (int b = 0; b < cas->num_blocks; b++) {
            cce_block* ob = &cas->blocks[b];
            if (cce_block_quantize_int8(ob) == CCE_OK) {
                int s0 = ob->weights.shape[0], s1 = ob->weights.shape[1];
                size_t ne = ob->weights.numel;
                cce_tensor_free(&ob->weights);          /* drop the FP payload */
                ob->weights.shape[0] = s0;              /* ...but keep the shape */
                ob->weights.shape[1] = s1;
                ob->weights.ndim = 2;
                ob->weights.numel = ne;
                /* An oracle never trains: drop the Adam buffers too. Leaving
                   them was a 2x-FP-sized leak per block — the OOM that killed
                   the first 12B loads (~1.7 GB retained per layer). */
                cce_tensor_free(&ob->momentum_weights);
                cce_tensor_free(&ob->momentum_bias);
                cce_tensor_free(&ob->second_moment_w);
                cce_tensor_free(&ob->second_moment_b);
            }
        }
    }

    int idx = -1;
    rc = cce_forest_add_cascade_branch(forest, cas, branch_name, &idx);
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
        return -1;
    }
    return idx;
}

static cce_result gguf_build_qwen2_forest_at(cce_forest** out_forest, const cce_gguf* gguf, float init_scale, const char* tmpf);

cce_result cce_gguf_build_qwen2_forest(cce_forest** out_forest, const cce_gguf* gguf, float init_scale) {
    return gguf_build_qwen2_forest_at(out_forest, gguf, init_scale,
                                      "gguf_qwen2_forest.cce");
}

static cce_result gguf_build_qwen2_forest_at(cce_forest** out_forest, const cce_gguf* gguf, float init_scale, const char* tmpf) {
    if (!out_forest || !gguf) return CCE_ERR_INVALID_ARG;

    int n_layer = cce_gguf_get_n_layer(gguf);
    if (n_layer <= 0) n_layer = 24;

    cce_forest* forest = NULL;
    remove(tmpf);
    if (cce_forest_open(&forest, tmpf, 512) != CCE_OK) {
        return CCE_ERR_IO;
    }

    char wname[256], bname[256], brname[128];
    int add_fails = 0;

    /* Refusal boundary: a projection ABSENT from the tensor table is an
       architecture variant — the forward's own arch handling decides what
       that means (precedent: the gemma4-MTP draft has no attn_k/attn_v at
       all and its forward handles that; the flagship campaign ran on it).
       But a tensor that EXISTS in the file and fails to load (unsupported
       quant type, IO, OOM) means a silently amputated forest that "loads"
       and then forwards garbage — that is refused loudly. */
    #define GGUF_ADD_REQ(w, bn, br) do { \
        if (cce_gguf_add_linear_branch(forest, gguf, (w), (bn), (br), init_scale) < 0) { \
            if (cce_gguf_find_tensor(gguf, (w)) >= 0) { \
                GTRACE("present tensor %s FAILED to load -> fatal", (w)); \
                add_fails++; \
            } else { \
                GTRACE("branch %s: tensor absent (arch variant, forward decides)", (br)); \
            } \
        } } while (0)

    for (int l = 0; l < n_layer; l++) {
        GTRACE("forest layer %d/%d", l, n_layer);
        // Attention projections (separate for GQA)
        snprintf(wname, sizeof(wname), "blk.%d.attn_q.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.attn_q.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.q_proj", l);
        GGUF_ADD_REQ(wname, bname[0]?bname:NULL, brname);

        snprintf(wname, sizeof(wname), "blk.%d.attn_k.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.attn_k.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.k_proj", l);
        GGUF_ADD_REQ(wname, bname[0]?bname:NULL, brname);

        snprintf(wname, sizeof(wname), "blk.%d.attn_v.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.attn_v.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.v_proj", l);
        GGUF_ADD_REQ(wname, bname[0]?bname:NULL, brname);

        snprintf(wname, sizeof(wname), "blk.%d.attn_output.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.attn_output.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.o_proj", l);
        GGUF_ADD_REQ(wname, bname[0]?bname:NULL, brname);

        // MLP SwiGLU
        snprintf(wname, sizeof(wname), "blk.%d.ffn_gate.weight", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.gate_proj", l);
        GGUF_ADD_REQ(wname, NULL, brname);

        snprintf(wname, sizeof(wname), "blk.%d.ffn_up.weight", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.up_proj", l);
        GGUF_ADD_REQ(wname, NULL, brname);

        snprintf(wname, sizeof(wname), "blk.%d.ffn_down.weight", l);
        snprintf(bname, sizeof(bname), "blk.%d.ffn_down.bias", l);
        snprintf(brname, sizeof(brname), "qwen2.blk.%d.down_proj", l);
        GGUF_ADD_REQ(wname, bname[0]?bname:NULL, brname);
    }

    // Head only (tok_emb lookup stays in dedicated tensor; we do not need a linear branch for it)
    const char* head_name = (cce_gguf_find_tensor(gguf, "output.weight") >= 0) ? "output.weight" : "token_embd.weight";
    GGUF_ADD_REQ(head_name, NULL, "qwen2.lm_head");
    #undef GGUF_ADD_REQ

    if (add_fails > 0) {
        GTRACE("forest INCOMPLETE: %d required branches failed -> refusing", add_fails);
        cce_forest_close(forest);
        remove(tmpf);
        return CCE_ERR_UNSUPPORTED;
    }

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
static cce_result apply_linear_rows(cce_clgemm *gpu, cce_cascade* cas, const cce_tensor* in, cce_tensor* out);

/* Basic Qwen2 forward glue (RMS + GQA + SwiGLU) - full per-layer with KV cache */
static void gguf_silu(const cce_tensor* in, cce_tensor* out) {
    for (size_t i = 0; i < in->numel; i++) {
        float x = in->data[i];
        out->data[i] = x / (1.0f + expf(-x));  // silu(x) = x * sigmoid(x)
    }
}

static void gguf_apply_rope(float* q, float* k, int t, int head_dim, int pos, float base, int n_heads, int n_kv_heads) {
    (void)n_heads; (void)n_kv_heads;
    /* NEOX-style rotary (gemma & qwen2): pair dim i with i+head_dim/2, NOT the
       adjacent (i, i+1) GPT-J interleave. Wrong pairing scrambles all positions
       -> attention can't localize -> model collapses to an input-independent
       prior. head_dim here is the rope width (rope_dim); dims beyond it are
       left unrotated by the caller. */
    int half = head_dim / 2;
    for (int i = 0; i < half; i++) {
        float freq = 1.0f / powf(base, (float)(2 * i) / head_dim);
        float val = (float)pos * freq;
        float cosv = cosf(val);
        float sinv = sinf(val);
        if (q) {
            float q0 = q[t * head_dim + i];
            float q1 = q[t * head_dim + i + half];
            q[t * head_dim + i]        = q0 * cosv - q1 * sinv;
            q[t * head_dim + i + half] = q0 * sinv + q1 * cosv;
        }
        if (k) {
            float k0 = k[t * head_dim + i];
            float k1 = k[t * head_dim + i + half];
            k[t * head_dim + i]        = k0 * cosv - k1 * sinv;
            k[t * head_dim + i + half] = k0 * sinv + k1 * cosv;
        }
    }
}

cce_result cce_gguf_qwen2_forward(cce_gguf_qwen2* m, const int* tokens, int n_tokens, float* logits_out, int logits_cap) {
    if (!m || !tokens || n_tokens < 1 || !logits_out) return CCE_ERR_INVALID_ARG;
    if (!m->geom || m->k_slot_floats == 0) {
        fprintf(stderr, "cce_gguf: no attention geometry — model was not "
                        "loaded through a geometry-aware loader; refusing\n");
        return CCE_ERR_UNSUPPORTED;
    }

    int D = m->n_embd;
    int V = m->vocab_size ? m->vocab_size : 151936;
    int start_pos = m->cur_pos;

    if ((m->probe_batch ? start_pos + 1 : start_pos + n_tokens) > m->max_ctx)
        return CCE_ERR_INVALID_ARG;

    /* per-instance GPU handle wins; the process-global is the default */
    cce_clgemm *gpu = m->clgemm ? m->clgemm : g_gguf_clgemm;

    float eps = (m->rms_eps > 0.0f) ? m->rms_eps : 1e-6f;

    cce_tensor x = {0};
    int xsh[2] = {n_tokens, D};
    if (cce_tensor_alloc(&x, xsh, 2) != CCE_OK) return CCE_ERR_OOM;

    /* Embed (+ gemma-family sqrt(D) scaling — part of the architecture,
       not a tuning knob) */
    for (int t = 0; t < n_tokens; t++) {
        int tok = tokens[t];
        if (tok < 0 || tok >= V) tok = 0;
        memcpy(x.data + (size_t)t * D, m->tok_emb.data + (size_t)tok * D, D * sizeof(float));
    }
    if (m->embed_scale > 0.0f && m->embed_scale != 1.0f) {
        for (size_t i = 0; i < (size_t)n_tokens * D; i++)
            x.data[i] *= m->embed_scale;
    }

    /* CNET_FWD_TRACE=1: stage checksums of the first two forwards — the
       nondeterminism bisector. Zero cost when off. */
    static int fwd_trace = -1;
    int trace_this = 0;
    if (fwd_trace < 0) {
        const char *e = getenv("CNET_FWD_TRACE");
        fwd_trace = (e && e[0] == '1') ? 2 : 0;
    }
    if (fwd_trace > 0) { trace_this = 1; fwd_trace--; }
#define FWD_CK(tag, ptr, cnt) do { if (trace_this) { \
        double ck_ = 0.0; size_t ii_; \
        for (ii_ = 0; ii_ < (size_t)(cnt); ++ii_) ck_ += fabs((double)(ptr)[ii_]); \
        printf("FWD_TRACE %s: %.10g\n", (tag), ck_); } } while (0)
    FWD_CK("embed", x.data, (size_t)n_tokens * D);

    float *scores = (float*)malloc((size_t)m->max_ctx * sizeof *scores);
    if (!scores) { cce_tensor_free(&x); return CCE_ERR_OOM; }

    char name[128];
    for (int l = 0; l < m->n_layer; l++) {
        const struct cce_attn_geom *ge = &m->geom[l];
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

        if (ge->v_tied) v_cas = k_cas;   /* V = raw K projection (tied) */
        if (!q_cas || !k_cas || !v_cas || !o_cas || !gate_cas || !up_cas ||
            !down_cas) {
            fprintf(stderr, "cce_gguf: layer %d cascades missing — "
                            "refusing\n", l);
            free(scores); cce_tensor_free(&x);
            return CCE_ERR_NOT_FOUND;
        }

        int lnsh[2] = {n_tokens, D};
        cce_tensor ln1 = {0}, q = {0}, k = {0}, v = {0};
        cce_tensor_alloc(&ln1, lnsh, 2);
        gguf_rms_norm_impl(&x, &m->attn_norm[l], eps, &ln1, (m->embed_scale > 1.0f));

        cce_tensor_alloc(&q, (int[]){n_tokens, ge->q_dim}, 2);
        cce_tensor_alloc(&k, (int[]){n_tokens, ge->k_dim}, 2);
        cce_tensor_alloc(&v, (int[]){n_tokens, ge->v_dim}, 2);
        if (apply_linear_rows(gpu, q_cas, &ln1, &q) != CCE_OK ||
            apply_linear_rows(gpu, k_cas, &ln1, &k) != CCE_OK ||
            (ge->v_tied
                 ? (memcpy(v.data, k.data,
                           (size_t)n_tokens * ge->k_dim * sizeof(float)),
                    CCE_OK)
                 : apply_linear_rows(gpu, v_cas, &ln1, &v)) != CCE_OK) {
            /* REFUSAL BOUNDARY: a failed projection must never leave its
               output as uninitialized heap (the pre-geometry bug that mined
               garbage attention for months). */
            fprintf(stderr, "cce_gguf: q/k/v projection failed at layer %d "
                            "(q_dim=%d k_dim=%d v_dim=%d) — refusing\n", l,
                    ge->q_dim, ge->k_dim, ge->v_dim);
            free(scores);
            cce_tensor_free(&ln1); cce_tensor_free(&q);
            cce_tensor_free(&k); cce_tensor_free(&v); cce_tensor_free(&x);
            return CCE_ERR_UNSUPPORTED;
        }
        if (l == 0) { FWD_CK("ln1(l0)", ln1.data, (size_t)n_tokens * D);
                      FWD_CK("q(l0)", q.data, (size_t)n_tokens * ge->q_dim); }

        /* qk-norm (per head, when the tensors exist) THEN RoPE — with the
           LAYER's theta and rotation width (gemma4: swa layers theta 10k /
           dim 256, global layers theta 1e6 / dim 512). */
        const float *qnw = (m->attn_q_norm && m->attn_q_norm[l].data)
                               ? m->attn_q_norm[l].data : NULL;
        const float *knw = (m->attn_k_norm && m->attn_k_norm[l].data)
                               ? m->attn_k_norm[l].data : NULL;
        for (int t = 0; t < n_tokens; t++) {
            int pos = m->probe_batch ? start_pos : start_pos + t;
            for (int h = 0; h < ge->n_q; h++) {
                float *qh = q.data + (size_t)t * ge->q_dim +
                            (size_t)h * ge->head_dim;
                if (qnw) {
                    float ss = 0.0f;
                    for (int d2 = 0; d2 < ge->head_dim; d2++)
                        ss += qh[d2] * qh[d2];
                    ss = 1.0f / sqrtf(ss / ge->head_dim + eps);
                    for (int d2 = 0; d2 < ge->head_dim; d2++)
                        qh[d2] = (qh[d2] * ss) * qnw[d2];  /* GGUF bakes gemma +1 */
                }
                gguf_apply_rope(qh, NULL, 0, ge->rope_dim, pos,
                                ge->rope_base, ge->n_q, ge->n_k);
            }
            for (int h = 0; h < ge->n_k; h++) {
                float *kh = k.data + (size_t)t * ge->k_dim +
                            (size_t)h * ge->head_dim;
                if (knw) {
                    float ss = 0.0f;
                    for (int d2 = 0; d2 < ge->head_dim; d2++)
                        ss += kh[d2] * kh[d2];
                    ss = 1.0f / sqrtf(ss / ge->head_dim + eps);
                    for (int d2 = 0; d2 < ge->head_dim; d2++)
                        kh[d2] = (kh[d2] * ss) * knw[d2];  /* GGUF bakes gemma +1 */
                }
                gguf_apply_rope(NULL, kh, 0, ge->rope_dim, pos,
                                ge->rope_base, ge->n_q, ge->n_k);
            }
            /* stash to the per-layer slots BEFORE attention: every read
               below comes from the cache (prefix reuse = pinned rows).
               Probe rows are INDEPENDENT continuations — they must never
               enter the shared cache (they would clobber one slot); their
               k/v stay in the local tensors and attention reads them
               per-row below. */
            if (!m->probe_batch) {
                memcpy(m->k_cache + (size_t)pos * m->k_slot_floats + ge->k_off,
                       k.data + (size_t)t * ge->k_dim,
                       (size_t)ge->k_dim * sizeof(float));
                memcpy(m->v_cache + (size_t)pos * m->v_slot_floats + ge->v_off,
                       v.data + (size_t)t * ge->v_dim,
                       (size_t)ge->v_dim * sizeof(float));
            }
        }

        /* attention: GQA with (possibly asymmetric) K/V head groups +
           causal + optional sliding window */
        int o_in = ge->n_q * ge->v_head_dim;
        cce_tensor attn_out = {0};
        cce_tensor_alloc(&attn_out, (int[]){n_tokens, o_in}, 2);
        float scale = 1.0f / sqrtf((float)ge->head_dim);
        for (int h = 0; h < ge->n_q; h++) {
            int kh_i = h / (ge->n_q / ge->n_k);
            int vh_i = h / (ge->n_q / ge->n_v);
            for (int t = 0; t < n_tokens; t++) {
                int abs_t = m->probe_batch ? start_pos : start_pos + t;
                int jmin = 0;
                if (ge->window > 0 && abs_t - ge->window + 1 > 0)
                    jmin = abs_t - ge->window + 1;
                const float *qh = q.data + (size_t)t * ge->q_dim +
                                  (size_t)h * ge->head_dim;
                for (int j = jmin; j <= abs_t; j++) {
                    const float *kh = (m->probe_batch && j == abs_t)
                        ? k.data + (size_t)t * ge->k_dim +
                              (size_t)kh_i * ge->head_dim
                        : m->k_cache +
                              (size_t)j * m->k_slot_floats + ge->k_off +
                              (size_t)kh_i * ge->head_dim;
                    float sacc = 0.0f;
                    for (int d2 = 0; d2 < ge->head_dim; d2++)
                        sacc += qh[d2] * kh[d2];
                    scores[j] = sacc * scale;
                }
                if (trace_this && l == 0 && h == 0 && t == n_tokens - 1) {
                    const float *k_self = m->k_cache + (size_t)abs_t * m->k_slot_floats + ge->k_off + (size_t)kh_i * ge->head_dim;
                    const float *k_first = m->k_cache + (size_t)jmin * m->k_slot_floats + ge->k_off + (size_t)kh_i * ge->head_dim;
                    fprintf(stderr, "PRESOFT rope_base=%.0f scale=%.4f scores[", ge->rope_base, scale);
                    for (int j = jmin; j <= abs_t; j++) fprintf(stderr, "%.2f ", scores[j]);
                    fprintf(stderr, "] q[0:4]=%.3f,%.3f,%.3f,%.3f kself[0:4]=%.3f,%.3f,%.3f,%.3f kfirst[0:4]=%.3f,%.3f,%.3f,%.3f\n",
                            qh[0],qh[1],qh[2],qh[3], k_self[0],k_self[1],k_self[2],k_self[3], k_first[0],k_first[1],k_first[2],k_first[3]);
                }
                float maxs = -1e30f;
                for (int j = jmin; j <= abs_t; j++)
                    if (scores[j] > maxs) maxs = scores[j];
                float sum = 0.0f;
                for (int j = jmin; j <= abs_t; j++) {
                    scores[j] = expf(scores[j] - maxs);
                    sum += scores[j];
                }
                for (int j = jmin; j <= abs_t; j++) scores[j] /= sum;
                if (trace_this && l == 0 && h == 0 && t == n_tokens - 1) {
                    fprintf(stderr, "ATTN l0 h0 lastq: swa=%d win=%d n_q=%d n_k=%d n_v=%d vtied=%d hd=%d vhd=%d ropedim=%d weights[",
                            ge->swa, ge->window, ge->n_q, ge->n_k, ge->n_v, ge->v_tied, ge->head_dim, ge->v_head_dim, ge->rope_dim);
                    for (int j = jmin; j <= abs_t; j++) fprintf(stderr, "%.3f ", scores[j]);
                    fprintf(stderr, "]\n");
                }
                float *oh = attn_out.data + (size_t)t * o_in +
                            (size_t)h * ge->v_head_dim;
                memset(oh, 0, (size_t)ge->v_head_dim * sizeof(float));
                for (int j = jmin; j <= abs_t; j++) {
                    const float *vh = (m->probe_batch && j == abs_t)
                        ? v.data + (size_t)t * ge->v_dim +
                              (size_t)vh_i * ge->v_head_dim
                        : m->v_cache +
                              (size_t)j * m->v_slot_floats + ge->v_off +
                              (size_t)vh_i * ge->v_head_dim;
                    for (int d2 = 0; d2 < ge->v_head_dim; d2++)
                        oh[d2] += scores[j] * vh[d2];
                }
            }
        }

        cce_tensor after_attn = {0};
        cce_tensor_alloc(&after_attn, lnsh, 2);
        if (apply_linear_rows(gpu, o_cas, &attn_out, &after_attn) != CCE_OK) {
            fprintf(stderr, "cce_gguf: o_proj failed at layer %d — "
                            "refusing\n", l);
            free(scores);
            cce_tensor_free(&ln1); cce_tensor_free(&q); cce_tensor_free(&k);
            cce_tensor_free(&v); cce_tensor_free(&attn_out);
            cce_tensor_free(&after_attn); cce_tensor_free(&x);
            return CCE_ERR_UNSUPPORTED;
        }
        cce_tensor_free(&attn_out);
        if (l == 0) FWD_CK("attn(l0)", after_attn.data, (size_t)n_tokens * D);

        if (m->post_attention_norm && m->post_attention_norm[l].data) {
            cce_tensor tmp = {0};
            cce_tensor_alloc(&tmp, lnsh, 2);
            gguf_rms_norm_impl(&after_attn, &m->post_attention_norm[l], eps, &tmp, (m->embed_scale > 1.0f));
            memcpy(after_attn.data, tmp.data, (size_t)n_tokens * D * sizeof(float));
            cce_tensor_free(&tmp);
        }
        if (m->layer_output_scale && m->layer_output_scale[l].data &&
            m->layer_output_scale[l].numel > 0) {
            float sc = m->layer_output_scale[l].data[0];
            for (size_t i = 0; i < (size_t)n_tokens * D; i++)
                after_attn.data[i] *= sc;
        }

        /* residual */
        for (size_t i = 0; i < (size_t)n_tokens * D; i++)
            after_attn.data[i] += x.data[i];

        /* MLP */
        cce_tensor ln2 = {0}, gate = {0}, upv = {0}, mid = {0}, down = {0};
        int mlp_hidden = m->feed_forward_length > 0 ? m->feed_forward_length : 4864;
        cce_tensor_alloc(&ln2, lnsh, 2);
        gguf_rms_norm_impl(&after_attn, &m->ffn_norm[l], eps, &ln2, (m->embed_scale > 1.0f));

        cce_tensor_alloc(&gate, (int[]){n_tokens, mlp_hidden}, 2);
        cce_tensor_alloc(&upv, (int[]){n_tokens, mlp_hidden}, 2);
        if (apply_linear_rows(gpu, gate_cas, &ln2, &gate) != CCE_OK ||
            apply_linear_rows(gpu, up_cas, &ln2, &upv) != CCE_OK) {
            fprintf(stderr, "cce_gguf: gate/up failed at layer %d — refusing\n", l);
            free(scores);
            cce_tensor_free(&ln1); cce_tensor_free(&q); cce_tensor_free(&k);
            cce_tensor_free(&v); cce_tensor_free(&after_attn);
            cce_tensor_free(&ln2); cce_tensor_free(&gate); cce_tensor_free(&upv);
            cce_tensor_free(&x);
            return CCE_ERR_UNSUPPORTED;
        }

        cce_tensor_alloc(&mid, (int[]){n_tokens, mlp_hidden}, 2);
        gguf_silu(&gate, &gate);
        for (size_t i = 0; i < (size_t)n_tokens * mlp_hidden; i++)
            mid.data[i] = gate.data[i] * upv.data[i];

        cce_tensor_alloc(&down, lnsh, 2);
        if (apply_linear_rows(gpu, down_cas, &mid, &down) != CCE_OK) {
            fprintf(stderr, "cce_gguf: down failed at layer %d — refusing\n", l);
            free(scores);
            cce_tensor_free(&ln1); cce_tensor_free(&q); cce_tensor_free(&k);
            cce_tensor_free(&v); cce_tensor_free(&after_attn);
            cce_tensor_free(&ln2); cce_tensor_free(&gate); cce_tensor_free(&upv);
            cce_tensor_free(&mid); cce_tensor_free(&down); cce_tensor_free(&x);
            return CCE_ERR_UNSUPPORTED;
        }

        /* post-FFW norm: loaded for gemma-style models but never applied
           by the old forward — applied to the MLP branch before its
           residual add, mirroring post_attention_norm. */
        if (m->post_ffw_norm && m->post_ffw_norm[l].data) {
            cce_tensor tmp = {0};
            cce_tensor_alloc(&tmp, lnsh, 2);
            gguf_rms_norm_impl(&down, &m->post_ffw_norm[l], eps, &tmp, (m->embed_scale > 1.0f));
            memcpy(down.data, tmp.data, (size_t)n_tokens * D * sizeof(float));
            cce_tensor_free(&tmp);
        }

        for (size_t i = 0; i < (size_t)n_tokens * D; i++)
            x.data[i] = after_attn.data[i] + down.data[i];
        if (l == 0) FWD_CK("x(l0)", x.data, (size_t)n_tokens * D);

        cce_tensor_free(&ln1); cce_tensor_free(&q); cce_tensor_free(&k);
        cce_tensor_free(&v); cce_tensor_free(&after_attn);
        cce_tensor_free(&ln2); cce_tensor_free(&gate); cce_tensor_free(&upv);
        cce_tensor_free(&mid); cce_tensor_free(&down);

        if (g_gguf_layer_tap)
            g_gguf_layer_tap(l, x.data, n_tokens, D, g_gguf_layer_tap_ctx);
        if (m->layer_cap > 0 && l + 1 >= m->layer_cap) break;
    }
    free(scores);

    /* final norm + head */
    cce_tensor fn = {0};
    cce_tensor_alloc(&fn, xsh, 2);
    gguf_rms_norm_impl(&x, &m->output_norm, eps, &fn, (m->embed_scale > 1.0f));

    /* HEAD: normally only the LAST row (the only row anyone reads); in
       probe-batch mode EVERY row is an independent probe and gets its own
       logits at logits_out + row*logits_cap. */
    cce_tensor logits_t = {0};
    int lsh[2] = {1, V};
    cce_tensor_alloc(&logits_t, lsh, 2);
    cce_cascade* head_cas = cce_forest_get_resident(m->forest, "qwen2.lm_head");
    {
    int row0 = m->probe_batch ? 0 : n_tokens - 1;
    int brow;
    for (brow = row0; brow < n_tokens; ++brow) {
    float *row_out = m->probe_batch
        ? logits_out + (size_t)(brow - row0) * logits_cap
        : logits_out;
    cce_tensor fn_last = {0};
    fn_last.data = fn.data + (size_t)brow * D;
    fn_last.shape[0] = 1;
    fn_last.shape[1] = D;
    fn_last.ndim = 2;
    fn_last.numel = (size_t)D;
    bool head_ok = false;
    /* RESTRICTED HEAD: compute only the mined window's logits — a per-column
       dot from the FP head weights, bit-identical to the full head's values
       on those ids (same k-ascending order, FP_CONTRACT off), at ~1/1000th
       the head work. logits_t is written only at the window positions; the
       oracle reads exactly those. Startup gates leave head_window_n = 0 and
       take the full-head path below. */
    if (m->head_window_n > 0 && head_cas && head_cas->num_blocks == 1) {
        const cce_block *hb = &head_cas->blocks[0];
        if (hb->weights.data && !hb->w_q && !hb->w_trit &&
            hb->weights.ndim == 2 && hb->weights.shape[0] == D &&
            hb->weights.shape[1] == V) {
            const float *last = fn_last.data;
            const float *bias = (hb->bias.numel == (size_t)V) ? hb->bias.data : NULL;
            for (int wi = 0; wi < m->head_window_n; ++wi) {
                int id = m->head_window[wi];
                if (id < 0 || id >= V) continue;
                float acc = bias ? bias[id] : 0.0f;
                const float *wcol = hb->weights.data + id;   /* W[d*V + id] */
                for (int d = 0; d < D; ++d) acc += last[d] * wcol[(size_t)d * V];
                logits_t.data[id] = acc;
            }
            head_ok = true;
        }
    }
    if (!head_ok && head_cas) {
        if (apply_linear_rows(gpu, head_cas, &fn_last, &logits_t) == CCE_OK) head_ok = true;
    }
    if (!head_ok && m->output.data && m->output.ndim == 2) {
        int od0 = m->output.shape[0];
        int od1 = m->output.shape[1];
        float* last = fn_last.data;
        for (int vi = 0; vi < V && vi < logits_cap; vi++) {
            float sacc = 0.0f;
            if (od1 == D && od0 >= V) {
                for (int d=0; d<D; d++) sacc += last[d] * m->output.data[(size_t)vi * D + d];
            } else if (od0 == D && od1 >= V) {
                for (int d=0; d<D; d++) sacc += last[d] * m->output.data[(size_t)d * od1 + vi];
            }
            logits_t.data[vi] = sacc;
        }
    }

    /* final_softcap is NOT applied to the returned logits: tanh is
       monotonic, so pre-cap ranking is decision-identical in exact math —
       while fp32 saturation (c*tanhf -> exactly +-c for |l|>~9c, measured
       on this model) manufactures TIES that scramble top-k order. The
       oracle consumes decisions; rank on the uncapped values. (m->
       final_softcap is retained for future logit-level comparisons.) */

    {
        int out_len = (V < logits_cap) ? V : logits_cap;
        memcpy(row_out, logits_t.data, out_len * sizeof(float));
    }
    }
    }

    if (!m->probe_batch)
        m->cur_pos += n_tokens;

    cce_tensor_free(&x); cce_tensor_free(&fn); cce_tensor_free(&logits_t);
    return CCE_OK;
}


cce_result cce_gguf_qwen2_forward_probes(cce_gguf_qwen2* m,
    const int* probe_tokens, int n_probes, float* logits_out,
    int logits_cap) {
    cce_result rc;
    int saved_pos;
    if (!m || !probe_tokens || n_probes < 1 || !logits_out)
        return CCE_ERR_INVALID_ARG;
    saved_pos = m->cur_pos;
    m->probe_batch = 1;
    rc = cce_gguf_qwen2_forward(m, probe_tokens, n_probes, logits_out,
                                logits_cap);
    m->probe_batch = 0;
    m->cur_pos = saved_pos;
    return rc;
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

    GTRACE("hparams: L=%d D=%d H=%d KV=%d V=%d; building forest",
           m->n_layer, m->n_embd, m->n_head, m->n_kv_head, m->vocab_size);
    {
        /* unique per instance: oracle-pool lanes load this model N times in
           ONE process; a shared fixed scratch name would unlink the previous
           lane's live backing archive */
        static int g_forest_seq = 0;
        snprintf(m->forest_scratch, sizeof m->forest_scratch,
                 "gguf_qwen2_forest.%d.cce", ++g_forest_seq);
    }
    rc = gguf_build_qwen2_forest_at(&m->forest, g, 0.02f, m->forest_scratch);
    if (rc != CCE_OK) {
        free(m);
        cce_gguf_free(g);
        return rc;
    }
    GTRACE("forest built: %d branches", m->forest ? m->forest->num_branches : -1);

    /* Load norms */
    m->attn_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    m->ffn_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    m->attn_q_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
    m->attn_k_norm = (cce_tensor*)calloc(m->n_layer, sizeof(cce_tensor));
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
        snprintf(name, sizeof(name), "blk.%d.attn_k_norm.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->attn_k_norm[l]);
        snprintf(name, sizeof(name), "blk.%d.post_attention_norm.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->post_attention_norm[l]);
        snprintf(name, sizeof(name), "blk.%d.post_ffw_norm.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->post_ffw_norm[l]);
        snprintf(name, sizeof(name), "blk.%d.layer_output_scale.weight", l);
        cce_gguf_load_tensor_by_name(g, name, &m->layer_output_scale[l]);
    }

    GTRACE("norms loaded; loading rope/embedding");
    cce_gguf_load_tensor_by_name(g, "rope_freqs.weight", &m->rope_freqs);

    cce_gguf_load_tensor_by_name(g, "token_embd.weight", &m->tok_emb);
    GTRACE("tok_emb loaded (%d x %d)", m->tok_emb.ndim > 1 ? m->tok_emb.shape[0] : -1,
           m->tok_emb.ndim > 1 ? m->tok_emb.shape[1] : -1);
    cce_gguf_load_tensor_by_name(g, "output_norm.weight", &m->output_norm);
    if (cce_gguf_find_tensor(g, "output.weight") >= 0) {
        cce_gguf_load_tensor_by_name(g, "output.weight", &m->output);
    } else {
        /* tied head: alias the already-loaded embedding instead of
           dequanting a second multi-GB fp32 copy of the same tensor
           (owns_memory=0 keeps cce_tensor_free from double-freeing). */
        m->output = m->tok_emb;
        m->output.owns_memory = 0;
    }
    GTRACE("output/head loaded");

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
    {   /* short-context runs (campaign mining probes are <=4 tokens): let the
           caller cap the KV allocation. A 48-layer/262k-vocab model would
           otherwise calloc ~13 GB of cache it never touches. Opt-in env. */
        const char* e = getenv("CNET_MAX_CTX");
        if (e) { int v = atoi(e); if (v >= 8 && v < m->max_ctx) m->max_ctx = v; }
    }
    m->cur_pos = 0;

    {
        cce_result grc = gguf_build_attn_geom(m, g);
        if (grc != CCE_OK) {
            cce_gguf_free(g);
            cce_gguf_qwen2_free(m);
            return grc;
        }
        GTRACE("attn geom: %d layers, k_slot=%zu v_slot=%zu floats/pos",
               m->n_layer, m->k_slot_floats, m->v_slot_floats);
    }
    GTRACE("kv alloc: max_ctx=%d -> %zu + %zu floats", m->max_ctx,
           (size_t)m->max_ctx * m->k_slot_floats,
           (size_t)m->max_ctx * m->v_slot_floats);
    m->k_cache = (float*)calloc((size_t)m->max_ctx * m->k_slot_floats, sizeof(float));
    m->v_cache = (float*)calloc((size_t)m->max_ctx * m->v_slot_floats, sizeof(float));

    cce_gguf_free(g);  /* we don't need the raw loader anymore */
    GTRACE("load complete");
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

    /* uniform geometry from the restored scalar hparams (packed snapshots
       predate per-layer geometry; gemma4-style models refuse here rather
       than forward wrong) */
    if (cce_gguf_qwen2_geom_uniform(m) != CCE_OK) {
        fclose(f);
        cce_gguf_qwen2_free(m);
        return CCE_ERR_UNSUPPORTED;
    }
    m->k_cache = (float*)calloc((size_t)m->max_ctx * m->k_slot_floats, sizeof(float));
    m->v_cache = (float*)calloc((size_t)m->max_ctx * m->v_slot_floats, sizeof(float));

    fclose(f);
    *out = m;
    return CCE_OK;
}

void cce_gguf_qwen2_free(cce_gguf_qwen2* m) {
    if (!m) return;
    if (m->forest) cce_forest_close(m->forest);
    // cleanup internal temp backing files to prevent junk accumulation
    if (m->forest_scratch[0]) {
        remove(m->forest_scratch);   /* this instance's unique archive */
    } else {
        remove("gguf_qwen2_forest.cce");
        remove("gguf_qwen2_packed.cce");
        remove("st_llama_forest.cce"); /* same struct built by cce_st_llama_load */
    }
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
    if (m->attn_k_norm) {
        for (int l=0; l<m->n_layer; l++) cce_tensor_free(&m->attn_k_norm[l]);
        free(m->attn_k_norm);
    }
    free(m->geom);
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
