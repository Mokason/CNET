#include "../../include/cce/cce_safetensors.h"
#include "../../include/cce/cce_sparse_kv.h"
#include "../../include/cce/cce_compression.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <ctype.h>
#include <stdarg.h>
#include <math.h>
#include <time.h>
#include <limits.h>
#include "../../include/cnet_platform.h"  /* CNET_HAVE_CURL */
#if CNET_HAVE_CURL
#include <curl/curl.h>
#endif
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Internal representation */
#define CCE_ST_MAX_META_KV 16    /* captured "__metadata__" string pairs */

struct cce_safetensors {
    cce_safetensor_meta metas[CCE_ST_MAX_TENSORS];
    int count;
    char path[256];              /* single file, or the index.json when sharded */
    uint64_t header_len;         /* single-file header len (unused when sharded) */
    uint64_t data_section_size;  /* total bytes after the header (summed over shards) */
    char last_err[256];
    /* "__metadata__" string->string pairs (single-file loads; first N kept) */
    int  meta_kv_count;
    char meta_kv_key[CCE_ST_MAX_META_KV][CCE_ST_MAX_NAME];
    char meta_kv_val[CCE_ST_MAX_META_KV][CCE_ST_MAX_NAME];
    /* sharded checkpoint support (model.safetensors.index.json).
       shard_count == 0 means the classic single-file layout; otherwise each
       tensor knows which shard file it lives in and reads use that shard's
       own header offset. */
    int      shard_count;
    int      tensor_shard[CCE_ST_MAX_TENSORS];      /* meta idx -> shard idx */
    char     shard_path[CCE_ST_MAX_SHARDS][256];
    uint64_t shard_header_len[CCE_ST_MAX_SHARDS];
};

/* 64-bit-safe seek/size: real HF shards are ~5 GB and `long` is 32-bit on
   Windows (MinGW), so plain fseek/ftell would wrap past 2 GB. */
static int st_fseek64(FILE* f, uint64_t off) {
#ifdef _WIN32
    return _fseeki64(f, (long long)off, SEEK_SET);
#else
    return fseeko(f, (off_t)off, SEEK_SET);
#endif
}
static int64_t st_fsize64(FILE* f) {
#ifdef _WIN32
    if (_fseeki64(f, 0, SEEK_END) != 0) return -1;
    return _ftelli64(f);
#else
    if (fseeko(f, 0, SEEK_END) != 0) return -1;
    return (int64_t)ftello(f);
#endif
}

/* thread-local-ish last error for diagnostics (simple) */
static char g_last_err[256];

static void st_set_err(cce_safetensors* st, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_err, sizeof(g_last_err), fmt, ap);
    va_end(ap);
    if (st) {
        strncpy(st->last_err, g_last_err, sizeof(st->last_err)-1);
        st->last_err[sizeof(st->last_err)-1] = 0;
    }
}

const char* cce_safetensors_last_error(void) {
    return g_last_err[0] ? g_last_err : "no error";
}

/* ---- Minimal safe restricted JSON parser for safetensors header ----
   Only handles:
     { "name": { "dtype": "F32", "shape": [..], "data_offsets": [u,u] }, ... }
   No nesting beyond the value objects, no escapes beyond basic, strict.
*/

static void skip_ws(const char* s, int* i, int n) {
    while (*i < n && isspace((unsigned char)s[*i])) (*i)++;
}

static int parse_string(const char* s, int* i, int n, char* out, size_t outcap) {
    if (*i >= n || s[*i] != '"') return 0;
    (*i)++;
    size_t oi = 0;
    while (*i < n) {
        unsigned char c = (unsigned char)s[*i];
        if (c == '\\') {
            (*i)++;
            if (*i >= n) return 0;
            c = (unsigned char)s[*i];
            char mapped = 0;
            if (c == '"' || c == '\\' || c == '/') mapped = c;
            else if (c == 'b') mapped = '\b';
            else if (c == 'f') mapped = '\f';
            else if (c == 'n') mapped = '\n';
            else if (c == 'r') mapped = '\r';
            else if (c == 't') mapped = '\t';
            else if (c == 'u') {
                /* skip unicode escape, treat as ? */
                (*i) += 4;
                if (*i >= n) return 0;
                mapped = '?';
            }
            if (mapped) {
                if (oi + 1 < outcap) out[oi++] = mapped;
            }
            (*i)++;
            continue;
        }
        if (c == '"') {
            (*i)++;
            if (oi < outcap) out[oi] = 0;
            return 1;
        }
        if (c < 0x20) return 0;
        if (oi + 1 < outcap) out[oi++] = (char)c;
        (*i)++;
    }
    return 0;
}

static int parse_number_u64(const char* s, int* i, int n, uint64_t* out) {
    skip_ws(s, i, n);
    uint64_t v = 0;
    int digits = 0;
    if (*i < n && s[*i] == '-') return 0; /* no negatives for offsets */
    while (*i < n && isdigit((unsigned char)s[*i])) {
        v = v * 10 + (s[*i] - '0');
        (*i)++;
        digits++;
        if (digits > 20) return 0;
    }
    *out = v;
    return digits > 0;
}

static int parse_int(const char* s, int* i, int n, int* out) {
    skip_ws(s, i, n);
    int sign = 1;
    if (*i < n && s[*i] == '-') { sign = -1; (*i)++; }
    int v = 0, digits = 0;
    while (*i < n && isdigit((unsigned char)s[*i])) {
        v = v * 10 + (s[*i] - '0');
        (*i)++;
        digits++;
    }
    *out = sign * v;
    return digits > 0;
}

static int parse_shape_array(const char* s, int* i, int n, int* shape, int* ndim) {
    skip_ws(s, i, n);
    if (*i >= n || s[*i] != '[') return 0;
    (*i)++;
    skip_ws(s, i, n);
    *ndim = 0;
    if (*i < n && s[*i] == ']') { (*i)++; return 1; }
    while (*i < n) {
        int dim;
        if (!parse_int(s, i, n, &dim)) return 0;
        if (*ndim < CCE_MAX_DIMS) shape[*ndim] = dim;
        (*ndim)++;
        skip_ws(s, i, n);
        if (*i < n && s[*i] == ']') { (*i)++; return 1; }
        if (*i >= n || s[*i] != ',') return 0;
        (*i)++;
        skip_ws(s, i, n);
    }
    return 0;
}

static int parse_offsets_array(const char* s, int* i, int n, uint64_t* off0, uint64_t* off1) {
    skip_ws(s, i, n);
    if (*i >= n || s[*i] != '[') return 0;
    (*i)++;
    skip_ws(s, i, n);
    if (!parse_number_u64(s, i, n, off0)) return 0;
    skip_ws(s, i, n);
    if (*i >= n || s[*i] != ',') return 0;
    (*i)++;
    if (!parse_number_u64(s, i, n, off1)) return 0;
    skip_ws(s, i, n);
    if (*i >= n || s[*i] != ']') return 0;
    (*i)++;
    if (*off1 < *off0) return 0;
    return 1;
}

/* Skip an arbitrary JSON value (object/array/string/number/keyword), tracking
   nested braces/brackets and string escapes. Used to ignore the special
   "__metadata__" header entry that real safetensors files (e.g. every HF export)
   place first in the header — it is NOT a tensor and must not be parsed as one. */
static int skip_json_value(const char* s, int* i, int n) {
    skip_ws(s, i, n);
    if (*i >= n) return 0;
    char c = s[*i];
    if (c == '{' || c == '[') {
        char open = c, close = (c == '{') ? '}' : ']';
        int depth = 0;
        while (*i < n) {
            char d = s[*i];
            if (d == '"') {                       /* skip a quoted string token */
                (*i)++;
                while (*i < n) {
                    if (s[*i] == '\\') { *i += 2; continue; }
                    if (s[*i] == '"') { (*i)++; break; }
                    (*i)++;
                }
                continue;
            }
            if (d == open)  { depth++; (*i)++; continue; }
            if (d == close) { depth--; (*i)++; if (depth == 0) return 1; continue; }
            (*i)++;
        }
        return 0;
    }
    if (c == '"') {
        char tmp[8];
        return parse_string(s, i, n, tmp, sizeof(tmp)); /* discards, scans to closing quote */
    }
    while (*i < n && s[*i] != ',' && s[*i] != '}' && s[*i] != ']' &&
           !isspace((unsigned char)s[*i])) (*i)++;
    return 1;
}

/* Parse the "__metadata__" value — a flat string->string map — capturing up to
   max_kv pairs into st (non-string values are skipped, not errors). Returns 1
   on a well-formed map, 0 on malformed json (caller then falls back to skip). */
static int parse_meta_map(const char* s, int* i, int n, cce_safetensors* st) {
    skip_ws(s, i, n);
    if (*i >= n || s[*i] != '{') return 0;
    (*i)++;
    while (*i < n) {
        skip_ws(s, i, n);
        if (*i < n && s[*i] == '}') { (*i)++; return 1; }

        char key[CCE_ST_MAX_NAME];
        if (!parse_string(s, i, n, key, sizeof(key))) return 0;
        skip_ws(s, i, n);
        if (*i >= n || s[*i] != ':') return 0;
        (*i)++;
        skip_ws(s, i, n);

        if (*i < n && s[*i] == '"') {
            char val[CCE_ST_MAX_NAME];
            if (!parse_string(s, i, n, val, sizeof(val))) return 0;
            if (st && st->meta_kv_count < CCE_ST_MAX_META_KV) {
                int k = st->meta_kv_count++;
                snprintf(st->meta_kv_key[k], sizeof(st->meta_kv_key[k]), "%s", key);
                snprintf(st->meta_kv_val[k], sizeof(st->meta_kv_val[k]), "%s", val);
            }
        } else {
            if (!skip_json_value(s, i, n)) return 0;  /* spec says strings only; tolerate */
        }

        skip_ws(s, i, n);
        if (*i < n && s[*i] == ',') { (*i)++; continue; }
    }
    return 0;
}

/* Parse one tensor entry object after the colon */
static int parse_tensor_entry(const char* s, int* i, int n,
                              cce_safetensor_meta* m) {
    skip_ws(s, i, n);
    if (*i >= n || s[*i] != '{') return 0;
    (*i)++;
    int have_dtype = 0, have_shape = 0, have_off = 0;
    memset(m, 0, sizeof(*m));
    m->ndim = 0;

    while (*i < n) {
        skip_ws(s, i, n);
        if (*i < n && s[*i] == '}') { (*i)++; break; }

        char key[64];
        if (!parse_string(s, i, n, key, sizeof(key))) return 0;
        skip_ws(s, i, n);
        if (*i >= n || s[*i] != ':') return 0;
        (*i)++;
        skip_ws(s, i, n);

        if (strcmp(key, "dtype") == 0) {
            if (!parse_string(s, i, n, m->dtype, sizeof(m->dtype))) return 0;
            have_dtype = 1;
        } else if (strcmp(key, "shape") == 0) {
            if (!parse_shape_array(s, i, n, m->shape, &m->ndim)) return 0;
            have_shape = 1;
        } else if (strcmp(key, "data_offsets") == 0) {
            if (!parse_offsets_array(s, i, n, &m->data_offset, &m->data_size)) return 0;
            /* data_size here temporarily holds end; fix below */
            have_off = 1;
        } else {
            /* unknown key: skip simple value to stay robust but we prefer strict */
            /* for safety we fail on unknown to avoid surprises */
            return 0;
        }

        skip_ws(s, i, n);
        if (*i < n && s[*i] == ',') { (*i)++; continue; }
        if (*i < n && s[*i] == '}') continue;
    }

    if (!have_dtype || !have_shape || !have_off) return 0;

    /* fix data_size to be the byte length */
    uint64_t end = m->data_size;
    m->data_size = (end > m->data_offset) ? (end - m->data_offset) : 0;
    return 1;
}

static int parse_safetensors_header(const char* json, size_t jlen,
                                    cce_safetensor_meta* metas, int max_meta,
                                    int* out_count,
                                    cce_safetensors* meta_sink /* may be NULL */) {
    int i = 0, n = (int)jlen;
    *out_count = 0;
    skip_ws(json, &i, n);
    if (i >= n || json[i] != '{') return CCE_ERR_UNSUPPORTED;

    i++; /* '{' */
    while (i < n) {
        skip_ws(json, &i, n);
        if (i < n && json[i] == '}') { i++; break; }

        char name[CCE_ST_MAX_NAME];
        if (!parse_string(json, &i, n, name, sizeof(name))) return CCE_ERR_UNSUPPORTED;

        skip_ws(json, &i, n);
        if (i >= n || json[i] != ':') return CCE_ERR_UNSUPPORTED;
        i++;
        skip_ws(json, &i, n);

        /* The "__metadata__" entry is a free-form string->string map, not a
           tensor. Capture its pairs when a sink is given (falling back to a
           plain skip on malformed content) and continue. */
        if (strcmp(name, "__metadata__") == 0) {
            int save = i;
            if (!meta_sink || !parse_meta_map(json, &i, n, meta_sink)) {
                i = save;
                if (meta_sink) meta_sink->meta_kv_count = 0; /* partial capture is worthless */
                if (!skip_json_value(json, &i, n)) return CCE_ERR_UNSUPPORTED;
            }
            skip_ws(json, &i, n);
            if (i < n && json[i] == ',') { i++; }
            continue;
        }

        if (*out_count >= max_meta) return CCE_ERR_UNSUPPORTED;

        cce_safetensor_meta* m = &metas[*out_count];
        if (!parse_tensor_entry(json, &i, n, m)) return CCE_ERR_UNSUPPORTED;
        strncpy(m->name, name, CCE_ST_MAX_NAME-1);
        m->name[CCE_ST_MAX_NAME-1] = 0;

        (*out_count)++;

        skip_ws(json, &i, n);
        if (i < n && json[i] == ',') { i++; continue; }
    }
    return CCE_OK;
}

/* Compute numel from shape */
static size_t shape_numel(const int* shape, int ndim) {
    size_t n = 1;
    for (int d = 0; d < ndim; d++) {
        if (shape[d] < 0) return 0;
        n *= (size_t)shape[d];
        /* overflow guard simple */
        if (n > (1ULL << 40)) return 0;
    }
    return n;
}

/* ---- public API ---- */

/* Parse ONE .safetensors file (header first, validate everything before any
   data read). Extracted from the original cce_safetensors_load so the sharded
   path can run it per shard. On success fills metas/out_count/out_hlen/
   out_dsize; on failure sets g_last_err and returns the error. */
static cce_result st_parse_file(const char* path,
                                cce_safetensor_meta* metas, int max_meta,
                                int* out_count, uint64_t* out_hlen,
                                uint64_t* out_dsize,
                                cce_safetensors* meta_sink /* may be NULL */) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(g_last_err, sizeof(g_last_err), "cannot open: %s", path);
        return CCE_ERR_IO;
    }

    /* read header len (le64) */
    unsigned char lenbuf[8];
    if (fread(lenbuf, 1, 8, f) != 8) {
        fclose(f); return CCE_ERR_IO;
    }
    uint64_t hlen = 0;
    for (int b = 0; b < 8; b++) {
        hlen |= ((uint64_t)lenbuf[b]) << (b * 8);
    }

    /* Safety: cap header. 64 MiB is generous for metadata. */
    const uint64_t MAX_HEADER = 64ULL * 1024 * 1024;
    if (hlen == 0 || hlen > MAX_HEADER) {
        fclose(f);
        snprintf(g_last_err, sizeof(g_last_err), "bad header len %llu", (unsigned long long)hlen);
        return CCE_ERR_UNSUPPORTED;
    }

    /* read header */
    char* hbuf = (char*)malloc((size_t)hlen + 1);
    if (!hbuf) { fclose(f); return CCE_ERR_OOM; }
    size_t got = fread(hbuf, 1, (size_t)hlen, f);
    if (got != (size_t)hlen) {
        free(hbuf); fclose(f); return CCE_ERR_IO;
    }
    hbuf[hlen] = 0;

    /* get file size to validate data section (64-bit safe for >2GB shards) */
    int64_t fsize = st_fsize64(f);
    fclose(f); /* will reopen on demand for data */
    if (fsize < 0) { free(hbuf); return CCE_ERR_IO; }

    uint64_t data_start = 8ULL + hlen;
    if ((uint64_t)fsize < data_start) {
        free(hbuf);
        return CCE_ERR_UNSUPPORTED;
    }
    uint64_t data_size = (uint64_t)fsize - data_start;

    int parsed = 0;
    cce_result rc = parse_safetensors_header(hbuf, (size_t)hlen,
                                             metas, max_meta, &parsed, meta_sink);
    free(hbuf);
    if (rc != CCE_OK) {
        st_set_err(NULL, "header json parse failed: %s", path);
        return rc;
    }

    /* Validate every tensor's offsets against data_size */
    for (int k = 0; k < parsed; k++) {
        cce_safetensor_meta* m = &metas[k];
        if (m->data_size == 0) {
            /* allow zero-size? rare, reject */
            st_set_err(NULL, "zero size tensor: %s", m->name);
            return CCE_ERR_UNSUPPORTED;
        }
        if (m->data_offset + m->data_size > data_size) {
            st_set_err(NULL, "offset overflow for %s (%llu + %llu > %llu)",
                       m->name,
                       (unsigned long long)m->data_offset,
                       (unsigned long long)m->data_size,
                       (unsigned long long)data_size);
            return CCE_ERR_UNSUPPORTED;
        }
        /* basic dtype check */
        if (strncmp(m->dtype, "F32", 3) != 0 &&
            strncmp(m->dtype, "F16", 3) != 0 &&
            strncmp(m->dtype, "BF16", 4) != 0) {
            /* still accept but load_as will reject non-f32/f16 later */
        }
        /* compute expected bytes vs declared */
        size_t elems = shape_numel(m->shape, m->ndim);
        size_t expected = 0;
        if (strncmp(m->dtype, "F32", 3) == 0) expected = elems * 4;
        else if (strncmp(m->dtype, "F16", 3) == 0 || strncmp(m->dtype, "BF16", 4) == 0) expected = elems * 2;
        else if (strncmp(m->dtype, "I32", 3) == 0) expected = elems * 4;
        if (expected && m->data_size < expected) {
            st_set_err(NULL, "size mismatch %s", m->name);
            return CCE_ERR_UNSUPPORTED;
        }
    }

    *out_count = parsed;
    *out_hlen = hlen;
    *out_dsize = data_size;
    return CCE_OK;
}

/* Does this file have the single-file safetensors shape (8-byte sane header
   len followed by '{')? A file that passes here is NEVER treated as a shard
   index — the real format always wins the sniff. */
static int st_looks_single_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char hdr[9];
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (got < 9) return 0;
    uint64_t hlen = 0;
    for (int b = 0; b < 8; b++) hlen |= ((uint64_t)hdr[b]) << (b * 8);
    return hlen > 0 && hlen <= 64ULL * 1024 * 1024 && hdr[8] == '{';
}

/* Does this file look like an HF sharded-checkpoint index
   (model.safetensors.index.json)? Plain JSON — first non-whitespace char is
   '{' and "weight_map" appears near the start. */
static int st_looks_shard_index(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8192];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = 0;
    size_t i = 0;
    while (i < got && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\r' || buf[i] == '\n')) i++;
    if (i >= got || buf[i] != '{') return 0;
    return strstr(buf, "\"weight_map\"") != NULL;
}

cce_result cce_safetensors_load(const char* path, cce_safetensors** st_out) {
    if (!path || !st_out) return CCE_ERR_INVALID_ARG;
    *st_out = NULL;

    /* Sharded checkpoint index? Only when the file is NOT a real single-file
       safetensors (binary header always takes precedence). */
    if (!st_looks_single_file(path) && st_looks_shard_index(path))
        return cce_safetensors_load_sharded(path, st_out);

    cce_safetensors* st = (cce_safetensors*)calloc(1, sizeof(cce_safetensors));
    if (!st) return CCE_ERR_OOM;
    strncpy(st->path, path, sizeof(st->path)-1);

    cce_result rc = st_parse_file(path, st->metas, CCE_ST_MAX_TENSORS,
                                  &st->count, &st->header_len, &st->data_section_size,
                                  st /* capture __metadata__ */);
    if (rc != CCE_OK) { free(st); return rc; }

    /* success */
    g_last_err[0] = 0;
    *st_out = st;
    return CCE_OK;
}

/* ---- sharded checkpoints (model.safetensors.index.json) ----
   Index shape: { "metadata": {...}, "weight_map": { "tensor": "shard-file", ... } }
   Strictness (validate-then-canonicalize, refuse don't guess):
     - shard filenames must be plain siblings (no /, \, .., or drive colon)
     - every tensor found in a shard must be in weight_map AND mapped to that
       exact shard (this also catches duplicate names across shards)
     - every weight_map entry must be found in its shard
*/
cce_result cce_safetensors_load_sharded(const char* index_path, cce_safetensors** st_out) {
    if (!index_path || !st_out) return CCE_ERR_INVALID_ARG;
    *st_out = NULL;

    /* slurp the index json (cap 4 MiB — real HF indexes are ~100 KB) */
    FILE* f = fopen(index_path, "rb");
    if (!f) {
        snprintf(g_last_err, sizeof(g_last_err), "cannot open index: %s", index_path);
        return CCE_ERR_IO;
    }
    int64_t isz = st_fsize64(f);
    if (isz <= 0 || isz > 4LL * 1024 * 1024) {
        fclose(f);
        st_set_err(NULL, "index size out of range: %lld", (long long)isz);
        return CCE_ERR_UNSUPPORTED;
    }
    if (st_fseek64(f, 0) != 0) { fclose(f); return CCE_ERR_IO; }
    char* json = (char*)malloc((size_t)isz + 1);
    if (!json) { fclose(f); return CCE_ERR_OOM; }
    if (fread(json, 1, (size_t)isz, f) != (size_t)isz) { free(json); fclose(f); return CCE_ERR_IO; }
    fclose(f);
    json[isz] = 0;

    /* weight_map working set (heap; struct-of-arrays kept simple) */
    typedef struct { char name[CCE_ST_MAX_NAME]; int file; int found; } st_wm_entry;
    st_wm_entry* wm = (st_wm_entry*)calloc(CCE_ST_MAX_TENSORS, sizeof(st_wm_entry));
    char (*files)[128] = (char (*)[128])calloc(CCE_ST_MAX_SHARDS, 128);
    cce_safetensor_meta* tmp = (cce_safetensor_meta*)calloc(CCE_ST_MAX_TENSORS, sizeof(cce_safetensor_meta));
    cce_safetensors* st = (cce_safetensors*)calloc(1, sizeof(cce_safetensors));
    int wm_count = 0, file_count = 0;
    cce_result rc = CCE_ERR_UNSUPPORTED;
    if (!wm || !files || !tmp || !st) { rc = CCE_ERR_OOM; goto fail; }
    strncpy(st->path, index_path, sizeof(st->path)-1);

    /* parse the top-level object */
    {
        int i = 0, n = (int)isz;
        skip_ws(json, &i, n);
        if (i >= n || json[i] != '{') { st_set_err(NULL, "index: not a json object"); goto fail; }
        i++;
        int saw_weight_map = 0;
        while (i < n) {
            skip_ws(json, &i, n);
            if (i < n && json[i] == '}') { i++; break; }
            char key[64];
            if (!parse_string(json, &i, n, key, sizeof(key))) { st_set_err(NULL, "index: bad key"); goto fail; }
            skip_ws(json, &i, n);
            if (i >= n || json[i] != ':') { st_set_err(NULL, "index: missing ':'"); goto fail; }
            i++;
            skip_ws(json, &i, n);
            if (strcmp(key, "weight_map") == 0) {
                if (saw_weight_map) { st_set_err(NULL, "index: duplicate weight_map"); goto fail; }
                saw_weight_map = 1;
                if (i >= n || json[i] != '{') { st_set_err(NULL, "index: weight_map not an object"); goto fail; }
                i++;
                while (i < n) {
                    skip_ws(json, &i, n);
                    if (i < n && json[i] == '}') { i++; break; }
                    char tname[CCE_ST_MAX_NAME], fname[128];
                    if (!parse_string(json, &i, n, tname, sizeof(tname))) { st_set_err(NULL, "index: bad tensor name"); goto fail; }
                    skip_ws(json, &i, n);
                    if (i >= n || json[i] != ':') { st_set_err(NULL, "index: weight_map missing ':'"); goto fail; }
                    i++;
                    skip_ws(json, &i, n);
                    if (!parse_string(json, &i, n, fname, sizeof(fname))) { st_set_err(NULL, "index: bad shard filename"); goto fail; }
                    /* shard files must be plain siblings of the index */
                    if (!fname[0] || strchr(fname, '/') || strchr(fname, '\\') ||
                        strchr(fname, ':') || strstr(fname, "..")) {
                        st_set_err(NULL, "index: refused shard filename '%s'", fname);
                        goto fail;
                    }
                    if (wm_count >= CCE_ST_MAX_TENSORS) { st_set_err(NULL, "index: too many tensors (max %d)", CCE_ST_MAX_TENSORS); goto fail; }
                    int fi = -1;
                    for (int s = 0; s < file_count; s++) if (strcmp(files[s], fname) == 0) { fi = s; break; }
                    if (fi < 0) {
                        if (file_count >= CCE_ST_MAX_SHARDS) { st_set_err(NULL, "index: too many shards (max %d)", CCE_ST_MAX_SHARDS); goto fail; }
                        snprintf(files[file_count], sizeof(files[file_count]), "%s", fname);
                        fi = file_count++;
                    }
                    /* duplicate tensor name in the map itself */
                    for (int j = 0; j < wm_count; j++) {
                        if (strcmp(wm[j].name, tname) == 0) { st_set_err(NULL, "index: duplicate weight_map entry %s", tname); goto fail; }
                    }
                    snprintf(wm[wm_count].name, sizeof(wm[wm_count].name), "%s", tname);
                    wm[wm_count].file = fi;
                    wm_count++;
                    skip_ws(json, &i, n);
                    if (i < n && json[i] == ',') { i++; continue; }
                }
            } else {
                if (!skip_json_value(json, &i, n)) { st_set_err(NULL, "index: bad value for %s", key); goto fail; }
            }
            skip_ws(json, &i, n);
            if (i < n && json[i] == ',') { i++; continue; }
        }
        if (!saw_weight_map || wm_count == 0) { st_set_err(NULL, "index: empty or missing weight_map"); goto fail; }
    }

    /* resolve shard paths relative to the index directory */
    {
        int dirlen = 0;
        for (int q = 0; index_path[q]; q++)
            if (index_path[q] == '/' || index_path[q] == '\\') dirlen = q + 1;
        for (int s = 0; s < file_count; s++) {
            int need = snprintf(st->shard_path[s], sizeof(st->shard_path[s]),
                                "%.*s%s", dirlen, index_path, files[s]);
            if (need < 0 || need >= (int)sizeof(st->shard_path[s])) {
                st_set_err(NULL, "index: shard path too long");
                goto fail;
            }
        }
        st->shard_count = file_count;
    }

    /* parse each shard; every tensor must match the weight_map exactly */
    for (int s = 0; s < file_count; s++) {
        int cnt = 0; uint64_t hlen = 0, dsz = 0;
        rc = st_parse_file(st->shard_path[s], tmp, CCE_ST_MAX_TENSORS, &cnt, &hlen, &dsz, NULL);
        if (rc != CCE_OK) goto fail;   /* g_last_err already names the shard problem */
        st->shard_header_len[s] = hlen;
        st->data_section_size += dsz;
        for (int k = 0; k < cnt; k++) {
            int j = -1;
            for (int q = 0; q < wm_count; q++) if (strcmp(wm[q].name, tmp[k].name) == 0) { j = q; break; }
            if (j < 0) {
                st_set_err(NULL, "shard %s: tensor %s not in weight_map", files[s], tmp[k].name);
                rc = CCE_ERR_UNSUPPORTED; goto fail;
            }
            if (wm[j].file != s) {
                st_set_err(NULL, "tensor %s found in %s but weight_map assigns %s",
                           tmp[k].name, files[s], files[wm[j].file]);
                rc = CCE_ERR_UNSUPPORTED; goto fail;
            }
            if (wm[j].found) {
                st_set_err(NULL, "duplicate tensor %s", tmp[k].name);
                rc = CCE_ERR_UNSUPPORTED; goto fail;
            }
            if (st->count >= CCE_ST_MAX_TENSORS) {
                st_set_err(NULL, "too many tensors across shards (max %d)", CCE_ST_MAX_TENSORS);
                rc = CCE_ERR_UNSUPPORTED; goto fail;
            }
            st->metas[st->count] = tmp[k];
            st->tensor_shard[st->count] = s;
            st->count++;
            wm[j].found = 1;
        }
    }
    for (int j = 0; j < wm_count; j++) {
        if (!wm[j].found) {
            st_set_err(NULL, "weight_map entry %s missing from shard %s",
                       wm[j].name, files[wm[j].file]);
            rc = CCE_ERR_UNSUPPORTED; goto fail;
        }
    }

    free(tmp); free(files); free(wm); free(json);
    g_last_err[0] = 0;
    *st_out = st;
    return CCE_OK;

fail:
    free(tmp); free(files); free(wm); free(json); free(st);
    return rc;
}

int cce_safetensors_shard_count(const cce_safetensors* st) {
    return st ? st->shard_count : 0;
}

void cce_safetensors_free(cce_safetensors* st) {
    if (!st) return;
    free(st);
}

int cce_safetensors_count(const cce_safetensors* st) {
    return st ? st->count : 0;
}

cce_result cce_safetensors_get_meta(const cce_safetensors* st, int idx,
                                    cce_safetensor_meta* out_meta) {
    if (!st || idx < 0 || idx >= st->count || !out_meta) return CCE_ERR_INVALID_ARG;
    *out_meta = st->metas[idx];
    return CCE_OK;
}

int cce_safetensors_find(const cce_safetensors* st, const char* name) {
    if (!st || !name) return -1;
    for (int i = 0; i < st->count; i++) {
        if (strcmp(st->metas[i].name, name) == 0) return i;
    }
    return -1;
}

int cce_safetensors_meta_lookup(const cce_safetensors* st, const char* key,
                                char* buf, size_t cap) {
    if (!st || !key) return 0;
    for (int i = 0; i < st->meta_kv_count; i++) {
        if (strcmp(st->meta_kv_key[i], key) == 0) {
            if (buf && cap) snprintf(buf, cap, "%s", st->meta_kv_val[i]);
            return 1;
        }
    }
    return 0;
}

/* Internal: read raw bytes for a tensor into a malloc'd buffer.
   Caller frees. */
static unsigned char* st_read_raw(const cce_safetensors* st, int idx, size_t* out_bytes) {
    if (!st || idx < 0 || idx >= st->count) return NULL;
    const cce_safetensor_meta* m = &st->metas[idx];
    const char* fpath = st->path;
    uint64_t hlen = st->header_len;
    if (st->shard_count > 0) {           /* sharded: read from the tensor's shard */
        int s = st->tensor_shard[idx];
        fpath = st->shard_path[s];
        hlen = st->shard_header_len[s];
    }
    FILE* f = fopen(fpath, "rb");
    if (!f) return NULL;
    uint64_t abs_off = 8ULL + hlen + m->data_offset;
    if (st_fseek64(f, abs_off) != 0) { fclose(f); return NULL; }
    size_t n = (size_t)m->data_size;
    unsigned char* buf = (unsigned char*)malloc(n);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, n, f) != n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    if (out_bytes) *out_bytes = n;
    return buf;
}

static float f16_to_f32(uint16_t h) {
    /* minimal IEEE half to float (no subnormal/NaN perfection needed for weights) */
    uint32_t sign = (h & 0x8000u) << 16;
    int32_t exp = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FFu;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) f = sign; /* +/- 0 */
        else {
            /* subnormal */
            exp = -14;
            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            f = sign | ((uint32_t)(exp + 127) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        f = sign | 0x7F800000u | (mant << 13); /* inf/nan */
    } else {
        f = sign | ((uint32_t)(exp - 15 + 127) << 23) | (mant << 13);
    }
    float r; memcpy(&r, &f, 4);
    return r;
}

cce_result cce_safetensors_load_f32(const cce_safetensors* st, int idx,
                                    float* buf, size_t cap_elems) {
    if (!st || idx < 0 || idx >= st->count || !buf) return CCE_ERR_INVALID_ARG;
    const cce_safetensor_meta* m = &st->metas[idx];
    size_t elems = shape_numel(m->shape, m->ndim);
    if (cap_elems < elems) return CCE_ERR_INVALID_ARG;

    size_t raw_bytes = 0;
    unsigned char* raw = st_read_raw(st, idx, &raw_bytes);
    if (!raw) return CCE_ERR_IO;

    int is_f32 = (strncmp(m->dtype, "F32", 3) == 0);
    int is_f16 = (strncmp(m->dtype, "F16", 3) == 0);
    int is_bf16 = (strncmp(m->dtype, "BF16", 4) == 0);

    if (is_f32) {
        if (raw_bytes != elems * 4) { free(raw); return CCE_ERR_UNSUPPORTED; }
        memcpy(buf, raw, elems * 4);
    } else if (is_f16 || is_bf16) {
        if (raw_bytes != elems * 2) { free(raw); return CCE_ERR_UNSUPPORTED; }
        const uint16_t* hp = (const uint16_t*)raw;
        for (size_t e = 0; e < elems; e++) {
            /* BF16 is upper 16 of float bits; treat similar to F16 for rough import */
            uint16_t hv = hp[e];
            if (is_bf16) {
                /* simple: treat the bits as if they were the high half */
                uint32_t fb = ((uint32_t)hv) << 16;
                float fv; memcpy(&fv, &fb, 4);
                buf[e] = fv;
            } else {
                buf[e] = f16_to_f32(hv);
            }
        }
    } else {
        free(raw);
        return CCE_ERR_UNSUPPORTED;
    }
    free(raw);
    return CCE_OK;
}

cce_result cce_safetensors_load_as_tensor(const cce_safetensors* st, int idx,
                                          cce_tensor* out_tensor) {
    if (!st || idx < 0 || idx >= st->count || !out_tensor) return CCE_ERR_INVALID_ARG;
    const cce_safetensor_meta* m = &st->metas[idx];
    size_t elems = shape_numel(m->shape, m->ndim);
    if (elems == 0) return CCE_ERR_UNSUPPORTED;

    /* allocate owned tensor */
    cce_result rc = cce_tensor_alloc(out_tensor, m->shape, m->ndim);
    if (rc != CCE_OK) return rc;

    rc = cce_safetensors_load_f32(st, idx, out_tensor->data, elems);
    if (rc != CCE_OK) {
        cce_tensor_free(out_tensor);
        memset(out_tensor, 0, sizeof(*out_tensor));
    }
    return rc;
}

void cce_safetensors_print_info(const cce_safetensors* st, const char* label) {
    if (!st) return;
    printf("safetensors: %s (%d tensors, header %llu bytes, data %llu bytes)\n",
           label ? label : st->path, st->count,
           (unsigned long long)st->header_len,
           (unsigned long long)st->data_section_size);
    if (st->shard_count > 0)
        printf("  sharded checkpoint: %d shards via index %s\n", st->shard_count, st->path);
    for (int i = 0; i < st->count && i < 16; i++) {
        const cce_safetensor_meta* m = &st->metas[i];
        printf("  [%d] %s %s [", i, m->name, m->dtype);
        for (int d = 0; d < m->ndim; d++) printf("%s%d", d?",":"", m->shape[d]);
        printf("] off=%llu sz=%llu\n",
               (unsigned long long)m->data_offset,
               (unsigned long long)m->data_size);
    }
    if (st->count > 16) printf("  ... %d more\n", st->count - 16);
}

/* ---- populate helpers ---- */

static void transpose_copy_f32(const float* src, float* dst,
                               int rows, int cols) {
    /* src is rows x cols (row major), dst gets cols x rows view of transpose */
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            dst[c * rows + r] = src[r * cols + c];
        }
    }
}

cce_result cce_safetensors_populate_block(cce_block* blk,
                                          const cce_safetensors* st,
                                          const char* weight_name,
                                          const char* bias_name,
                                          int transpose_weight) {
    if (!blk || !st || !weight_name) return CCE_ERR_INVALID_ARG;

    int wi = cce_safetensors_find(st, weight_name);
    if (wi < 0) return CCE_ERR_NOT_FOUND;

    cce_tensor wtmp = {0};
    cce_result rc = cce_safetensors_load_as_tensor(st, wi, &wtmp);
    if (rc != CCE_OK) return rc;

    /* If block weights not sized yet (rare), we could realloc but prefer caller inits first.
       Here we check compatibility against existing allocation. */
    if (blk->weights.data == NULL || blk->weights.numel == 0) {
        cce_tensor_free(&wtmp);
        return CCE_ERR_INVALID_ARG; /* caller must have allocated via block init */
    }

    int blk_in  = blk->weights.shape[0];
    int blk_out = blk->weights.shape[1];

    const float* wsrc = wtmp.data;
    float* wdst = blk->weights.data;

    if (transpose_weight) {
        /* assume loaded tensor is [out, in] -> store as CCE [in, out] */
        if (wtmp.ndim >= 2) {
            int t_out = wtmp.shape[0];
            int t_in  = wtmp.shape[1];
            if (t_in == blk_in && t_out == blk_out) {
                transpose_copy_f32(wsrc, wdst, t_out, t_in);
            } else {
                cce_tensor_free(&wtmp);
                return CCE_ERR_UNSUPPORTED;
            }
        } else {
            memcpy(wdst, wsrc, wtmp.numel * sizeof(float));
        }
    } else {
        if (wtmp.numel != blk->weights.numel) {
            cce_tensor_free(&wtmp);
            return CCE_ERR_UNSUPPORTED;
        }
        memcpy(wdst, wsrc, wtmp.numel * sizeof(float));
    }
    cce_tensor_free(&wtmp);

    /* bias */
    if (bias_name) {
        int bi = cce_safetensors_find(st, bias_name);
        if (bi >= 0) {
            cce_tensor btmp = {0};
            if (cce_safetensors_load_as_tensor(st, bi, &btmp) == CCE_OK) {
                if (btmp.numel == blk->bias.numel && blk->bias.data) {
                    memcpy(blk->bias.data, btmp.data, btmp.numel * sizeof(float));
                }
                cce_tensor_free(&btmp);
            }
        }
    }
    return CCE_OK;
}

cce_result cce_safetensors_build_linear_cascade(cce_cascade** out_cas,
                                                const cce_safetensors* st,
                                                const char* layer_prefix,
                                                int num_layers,
                                                int head_last,
                                                float init_scale_fallback) {
    if (!out_cas || !st || num_layers <= 0) return CCE_ERR_INVALID_ARG;
    *out_cas = NULL;

    cce_result rc = cce_cascade_create(out_cas, num_layers + 4);
    if (rc != CCE_OK) return rc;
    cce_cascade* cas = *out_cas;

    char wname[128], bname[128];

    for (int l = 0; l < num_layers; l++) {
        snprintf(wname, sizeof(wname), "%s%d.weight", layer_prefix ? layer_prefix : "layer", l);
        snprintf(bname, sizeof(bname), "%s%d.bias", layer_prefix ? layer_prefix : "layer", l);

        int wi = cce_safetensors_find(st, wname);
        if (wi < 0) {
            /* try alternate common names */
            snprintf(wname, sizeof(wname), "layers.%d.weight", l);
            wi = cce_safetensors_find(st, wname);
        }
        if (wi < 0) {
            cce_cascade_destroy(cas);
            *out_cas = NULL;
            return CCE_ERR_NOT_FOUND;
        }

        cce_tensor wtmp = {0};
        rc = cce_safetensors_load_as_tensor(st, wi, &wtmp);
        if (rc != CCE_OK) { cce_cascade_destroy(cas); *out_cas = NULL; return rc; }

        int in_d = (wtmp.ndim >= 2) ? wtmp.shape[ (wtmp.ndim>1?0:0) ] : (int)wtmp.numel;
        int out_d = (wtmp.ndim >= 2) ? wtmp.shape[1] : 1;

        /* CCE block stores [in, out] */
        int is_head = (head_last && l == num_layers-1) ? 1 : 0;
        rc = is_head
            ? cce_cascade_add_linear_head(cas, in_d, out_d, init_scale_fallback)
            : cce_cascade_add_linear(cas, in_d, out_d, init_scale_fallback);
        if (rc != CCE_OK) {
            cce_tensor_free(&wtmp);
            cce_cascade_destroy(cas); *out_cas = NULL; return rc;
        }

        /* populate the last added block */
        cce_block* last_blk = &cas->blocks[cas->num_blocks - 1];
        /* overwrite allocated weights with loaded (transpose typical) */
        if (last_blk->weights.numel == wtmp.numel) {
            memcpy(last_blk->weights.data, wtmp.data, wtmp.numel * sizeof(float));
        } else {
            /* shape mismatch: assume torch [out,in] */
            int t_out = (wtmp.ndim>=2) ? wtmp.shape[0] : out_d;
            int t_in  = (wtmp.ndim>=2) ? wtmp.shape[1] : in_d;
            if (t_in == in_d && t_out == out_d && last_blk->weights.shape[0]==in_d) {
                transpose_copy_f32(wtmp.data, last_blk->weights.data, t_out, t_in);
            }
        }
        cce_tensor_free(&wtmp);

        /* bias optional */
        snprintf(bname, sizeof(bname), "%s%d.bias", layer_prefix ? layer_prefix : "layer", l);
        int bi = cce_safetensors_find(st, bname);
        if (bi < 0) {
            snprintf(bname, sizeof(bname), "layers.%d.bias", l);
            bi = cce_safetensors_find(st, bname);
        }
        if (bi >= 0 && last_blk->bias.data && last_blk->bias.numel > 0) {
            cce_tensor btmp = {0};
            if (cce_safetensors_load_as_tensor(st, bi, &btmp) == CCE_OK) {
                if (btmp.numel == last_blk->bias.numel)
                    memcpy(last_blk->bias.data, btmp.data, btmp.numel * sizeof(float));
                cce_tensor_free(&btmp);
            }
        }

    }

    return CCE_OK;
}

cce_result cce_safetensors_add_linear_branch(cce_forest* forest,
                                             const cce_safetensors* st,
                                             const char* branch_name,
                                             const char* layer_prefix,
                                             int num_layers,
                                             int head_last,
                                             float init_scale_fallback,
                                             int* out_branch_idx) {
    if (!forest || !st || !branch_name) return CCE_ERR_INVALID_ARG;
    cce_cascade* cas = NULL;
    cce_result rc = cce_safetensors_build_linear_cascade(&cas, st, layer_prefix, num_layers, head_last, init_scale_fallback);
    if (rc != CCE_OK) return rc;

    rc = cce_forest_add_cascade_branch(forest, cas, branch_name, out_branch_idx);
    /* forest_add takes ownership of the cascade pointer? From earlier code it stores the pointer */
    /* cce_cascade_destroy will be called by forest close if needed, but to avoid double free we don't destroy here */
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
    }
    return rc;
}

/* ---- BTN population (full CNet) ---- */

int btn_populate_from_safetensors(BinaryTransformNetwork* btn,
                                  const cce_safetensors* st,
                                  const char* ih_name, const char* hb_name,
                                  const char* ho_name, const char* ob_name,
                                  int transpose_ih, int transpose_ho) {
    if (!btn || !st) return -1;

    int copied = 0;

    /* input_hidden */
    if (ih_name) {
        int idx = cce_safetensors_find(st, ih_name);
        if (idx >= 0 && btn->input_hidden) {
            cce_tensor t = {0};
            if (cce_safetensors_load_as_tensor(st, idx, &t) == CCE_OK) {
                size_t n = (size_t)btn->input_count * btn->max_hidden_count;
                if (t.numel == n) {
                    for (size_t k=0; k < n; k++) btn->input_hidden[k] = (double)t.data[k];
                    copied++;
                } else if (t.ndim == 2) {
                    /* try transpose layout */
                    int r = t.shape[0], c = t.shape[1];
                    if ((size_t)r * c == n) {
                        for (size_t h=0; h < (size_t)btn->max_hidden_count; h++)
                            for (size_t inp=0; inp < (size_t)btn->input_count; inp++) {
                                size_t di = h * btn->input_count + inp;
                                size_t si = transpose_ih ? (size_t)inp * c + h : (size_t)h * c + inp;
                                if (si < t.numel) btn->input_hidden[di] = (double)t.data[si];
                            }
                        copied++;
                    }
                }
                cce_tensor_free(&t);
            }
        }
    }

    /* hidden bias */
    if (hb_name) {
        int idx = cce_safetensors_find(st, hb_name);
        if (idx >= 0 && btn->hidden_bias) {
            cce_tensor t = {0};
            if (cce_safetensors_load_as_tensor(st, idx, &t) == CCE_OK &&
                t.numel == (size_t)btn->max_hidden_count) {
                for (size_t k=0; k < t.numel; k++) btn->hidden_bias[k] = (double)t.data[k];
                copied++;
            }
            cce_tensor_free(&t);
        }
    }

    /* hidden->output weights */
    if (ho_name) {
        int idx = cce_safetensors_find(st, ho_name);
        if (idx >= 0 && btn->hidden_output_weights) {
            cce_tensor t = {0};
            if (cce_safetensors_load_as_tensor(st, idx, &t) == CCE_OK) {
                size_t n = (size_t)btn->output_count * btn->max_hidden_count;
                if (t.numel == n) {
                    for (size_t k = 0; k < n; k++) btn->hidden_output_weights[k] = (double)t.data[k];
                    copied++;
                } else if (t.ndim == 2) {
                    int r = t.shape[0], c = t.shape[1];
                    if ((size_t)r * c == n) {
                        for (size_t o=0; o < (size_t)btn->output_count; o++)
                            for (size_t h=0; h < (size_t)btn->max_hidden_count; h++) {
                                size_t di = o * btn->max_hidden_count + h;
                                size_t si = transpose_ho ? (size_t)h * r + o : (size_t)o * c + h;
                                if (si < t.numel) btn->hidden_output_weights[di] = (double)t.data[si];
                            }
                        copied++;
                    }
                }
                cce_tensor_free(&t);
            }
        }
    }

    /* output bias */
    if (ob_name) {
        int idx = cce_safetensors_find(st, ob_name);
        if (idx >= 0 && btn->output_bias) {
            cce_tensor t = {0};
            if (cce_safetensors_load_as_tensor(st, idx, &t) == CCE_OK &&
                t.numel == (size_t)btn->output_count) {
                for (size_t k=0; k<t.numel; k++) btn->output_bias[k] = (double)t.data[k];
                copied++;
            }
            cce_tensor_free(&t);
        }
    }

    return copied;
}

int btn_populate_from_safetensors_heuristic(BinaryTransformNetwork* btn,
                                            const cce_safetensors* st,
                                            int transpose) {
    if (!btn || !st) return -1;
    /* try common exported patterns */
    int c = 0;
    c += btn_populate_from_safetensors(btn, st, "input_hidden", "hidden_bias",
                                       "hidden_output_weights", "output_bias", transpose, transpose) > 0 ? 1 : 0;
    if (c == 0) c += btn_populate_from_safetensors(btn, st, "0.weight", "0.bias",
                                                   "2.weight", "2.bias", transpose, transpose) > 0 ? 1 : 0;
    if (c == 0) c += btn_populate_from_safetensors(btn, st, "fc1.weight", "fc1.bias",
                                                   "fc2.weight", "fc2.bias", transpose, transpose) > 0 ? 1 : 0;
    if (c == 0) c += btn_populate_from_safetensors(btn, st, "weight", "bias",
                                                   NULL, NULL, transpose, 0) > 0 ? 1 : 0;
    return c;
}

/* Helper to get a cascade specialist by branch name from a forest */
static cce_cascade* get_cascade_by_name(cce_forest* f, const char* name) {
    if (!f || !name) return NULL;
    for (int i = 0; i < f->num_branches; ++i) {
        if (strcmp(f->branches[i].name, name) == 0)
            return f->branches[i].cascade;
    }
    return NULL;
}

/* ==================== HF / URL support implementation ==================== */

#if CNET_HAVE_CURL
static const char* st_get_hf_token(void) {
    const char* t = getenv("HF_TOKEN");
    if (!t || !*t) t = getenv("HUGGING_FACE_HUB_TOKEN");
    return (t && *t) ? t : NULL;
}

/* Create the temporary file atomically so another local process cannot replace
   a predictable download path with a symlink. */
static int st_make_temp_path(char* buf, size_t cap, const char* prefix) {
    if (!buf || cap < 32) return -1;
#ifdef _WIN32
    char temp_dir[MAX_PATH];
    DWORD length;
    (void)prefix;
    if (cap < MAX_PATH) return -1;
    length = GetTempPathA(MAX_PATH, temp_dir);
    if (length == 0 || length >= MAX_PATH) return -1;
    return GetTempFileNameA(temp_dir, "cne", 0, buf) == 0 ? -1 : 0;
#else
    const char* temp_dir = getenv("TMPDIR");
    int length;
    int fd;
    if (!temp_dir || !*temp_dir) temp_dir = "/tmp";
    length = snprintf(buf, cap, "%s/%sXXXXXX", temp_dir,
                      prefix ? prefix : "cnet_st_");
    if (length < 0 || (size_t)length >= cap) return -1;
    fd = mkstemp(buf);
    if (fd < 0) return -1;
    if (close(fd) != 0) {
        remove(buf);
        return -1;
    }
    return 0;
#endif
}
#endif /* CNET_HAVE_CURL -- token and temp path feed only the download path */

#define CCE_ST_MAX_DOWNLOAD_BYTES (UINT64_C(8) * 1024 * 1024 * 1024)
#define CCE_ST_MAX_REDIRECTS 5L
#define CCE_ST_CONNECT_TIMEOUT_MS 10000L
#define CCE_ST_DOWNLOAD_TIMEOUT_MS 600000L

typedef struct {
    FILE* file;
    uint64_t bytes;
    uint64_t limit;
    int too_large;
} st_download_sink;

#if CNET_HAVE_CURL
static size_t st_download_write(void* data, size_t size, size_t count, void* user) {
    st_download_sink* sink = (st_download_sink*)user;
    if (!sink || !sink->file || (size != 0 && count > SIZE_MAX / size))
        return CURL_WRITEFUNC_ERROR;

    size_t chunk = size * count;
    if (sink->bytes > sink->limit ||
        (uint64_t)chunk > sink->limit - sink->bytes) {
        sink->too_large = 1;
        return CURL_WRITEFUNC_ERROR;
    }
    if (chunk != 0 && fwrite(data, 1, chunk, sink->file) != chunk)
        return CURL_WRITEFUNC_ERROR;
    sink->bytes += (uint64_t)chunk;
    return chunk;
}

#endif /* CNET_HAVE_CURL -- st_download_write */

/* ---- Outbound host policy (SSRF containment) -------------------------------
   cce_safetensors_load_url() dials a URL the caller may not fully control, so
   egress is default-deny. Two independent layers, because either alone leaks:

     1. name policy    (st_host_allowed)  - the URL host must match the built-in
        Hugging Face set or an operator entry in CCE_ST_URL_ALLOWLIST.
     2. address policy (st_prereq_public_ip) - libcurl invokes this after DNS on
        *every* hop, so a redirect or a rebound DNS record aimed at loopback,
        RFC1918, link-local (incl. 169.254.169.254 cloud metadata) or CGNAT
        space is refused at connect time, after the name check has passed.

   Layer 2 is what makes FOLLOWLOCATION safe: layer 1 only ever sees the first
   URL. IP literals and localhost are rejected ahead of the allowlist so an
   operator entry cannot re-enable them. */
#define CCE_ST_ENV_ALLOWLIST "CCE_ST_URL_ALLOWLIST"

#if CNET_HAVE_CURL || defined(CCE_SAFETENSORS_TESTING)
static const char* const st_default_allowlist[] = {
    "huggingface.co",
    "hf.co",
};

/* Case-insensitive "host is `pattern` or a subdomain of `pattern`". Matching on
   a label boundary is what stops huggingface.co.evil.invalid from passing. */
/* ASCII-only case-insensitive compare, replacing curl_strequal.
 *
 * Deliberately used on BOTH the curl and no-curl builds. Keeping curl_strequal
 * for one and a local comparator for the other would mean two implementations
 * that could disagree about when two hostnames are "the same" -- which is
 * exactly how an egress-allowlist bypass gets introduced.
 *
 * Folding is restricted to A-Z rather than calling tolower() because tolower()
 * is locale-dependent: under a Turkish locale 'I' folds to a dotless 'i', so
 * whether a host matched the allowlist would depend on the operator's LANG. */
static int st_ascii_ieq(const char* a, const char* b) {
    for (;; ++a, ++b) {
        unsigned char x = (unsigned char)*a;
        unsigned char y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (x == 0) return 1;
    }
}

static int st_host_matches(const char* host, const char* pattern) {
    size_t hl, pl;
    if (!host || !pattern || !*pattern) return 0;
    hl = strlen(host);
    pl = strlen(pattern);
    if (hl == pl) return st_ascii_ieq(host, pattern);
    return hl > pl && host[hl - pl - 1] == '.' && st_ascii_ieq(host + hl - pl, pattern);
}

/* Strict dotted-quad parse. Hand-rolled to avoid pulling inet_pton (and the
   ws2tcpip.h/arpa-inet split) into an otherwise portable translation unit. */
static int st_parse_ipv4(const char* s, unsigned char out[4]) {
    int octet = 0;
    for (; octet < 4; ++octet) {
        int value = 0, digits = 0;
        if (octet > 0) {
            if (*s != '.') return 0;
            ++s;
        }
        while (*s >= '0' && *s <= '9') {
            value = value * 10 + (*s - '0');
            if (++digits > 3 || value > 255) return 0;
            ++s;
        }
        if (digits == 0) return 0;
        out[octet] = (unsigned char)value;
    }
    return *s == '\0';
}

static int st_ipv4_is_public(const unsigned char a[4]) {
    if (a[0] == 0 || a[0] == 10 || a[0] == 127) return 0;        /* this-net, RFC1918, loopback */
    if (a[0] == 169 && a[1] == 254) return 0;                    /* link-local + metadata */
    if (a[0] == 172 && a[1] >= 16 && a[1] <= 31) return 0;       /* RFC1918 */
    if (a[0] == 192 && a[1] == 168) return 0;                    /* RFC1918 */
    if (a[0] == 192 && a[1] == 0 && a[2] == 0) return 0;         /* IETF protocol assignments */
    if (a[0] == 100 && a[1] >= 64 && a[1] <= 127) return 0;      /* CGNAT */
    if (a[0] == 198 && (a[1] == 18 || a[1] == 19)) return 0;     /* benchmarking */
    if (a[0] >= 224) return 0;                                   /* multicast + reserved */
    return 1;
}

/* Textual IPv6 reject-list: loopback/unspecified, unique-local (fc00::/7),
   link-local (fe80::/10), and IPv4-mapped forms carrying a private v4. */
static int st_ipv6_is_public(const char* ip) {
    unsigned char v4[4];
    const char* mapped;
    if (!ip || !*ip) return 0;
    if (strcmp(ip, "::1") == 0 || strcmp(ip, "::") == 0) return 0;
    if ((ip[0] == 'f' || ip[0] == 'F') && ip[1] != '\0') {
        char c1 = (char)tolower((unsigned char)ip[1]);
        if (c1 == 'c' || c1 == 'd') return 0;                    /* fc00::/7 */
        if (c1 == 'e' && ip[2] != '\0') {
            char c2 = (char)tolower((unsigned char)ip[2]);
            if (c2 >= '8' && c2 <= '9') return 0;                /* fe80::/10 */
            if (c2 == 'a' || c2 == 'b') return 0;
        }
    }
    mapped = strrchr(ip, ':');
    if (mapped && strchr(mapped, '.') == NULL) return 1;
    if (mapped && st_parse_ipv4(mapped + 1, v4)) return st_ipv4_is_public(v4);
    return 1;
}

static int st_ip_literal_is_public(const char* ip) {
    unsigned char v4[4];
    if (!ip || !*ip) return 0;
    if (st_parse_ipv4(ip, v4)) return st_ipv4_is_public(v4);
    if (strchr(ip, ':') != NULL) return st_ipv6_is_public(ip);
    return 1;  /* not an address literal; the name policy governs it */
}

static int st_host_is_ip_literal(const char* host) {
    unsigned char v4[4];
    if (!host || !*host) return 0;
    return st_parse_ipv4(host, v4) || strchr(host, ':') != NULL;
}

/* Name policy. Returns 1 only for a host the operator has actually blessed. */
static int st_host_allowed(const char* host) {
    const char* env;
    size_t i;

    if (!host || !*host) return 0;
    /* Unconditional denies: an allowlist entry must not be able to re-open
       these, and a bare IP has no name to police in the first place. */
    if (st_host_is_ip_literal(host)) return 0;
    if (st_host_matches(host, "localhost") || st_host_matches(host, "local") ||
        st_host_matches(host, "internal") || st_host_matches(host, "home.arpa"))
        return 0;

    for (i = 0; i < sizeof(st_default_allowlist) / sizeof(st_default_allowlist[0]); ++i)
        if (st_host_matches(host, st_default_allowlist[i])) return 1;

    env = getenv(CCE_ST_ENV_ALLOWLIST);
    if (env && *env) {
        /* Comma-separated; bounded copy so a hostile env cannot overrun. */
        char buf[512];
        char* save;
        size_t len = strlen(env);
        if (len >= sizeof(buf)) return 0;
        memcpy(buf, env, len + 1);
        for (save = buf; *save;) {
            char* comma = strchr(save, ',');
            if (comma) *comma = '\0';
            while (*save == ' ' || *save == '\t') ++save;
            if (*save && st_host_matches(host, save)) return 1;
            if (!comma) break;
            save = comma + 1;
        }
    }
    return 0;
}

/* Address policy: runs once per connection, including each redirect hop. */
#if CNET_HAVE_CURL
static int st_prereq_public_ip(void* clientp, char* conn_primary_ip,
                               char* conn_local_ip, int conn_primary_port,
                               int conn_local_port) {
    (void)clientp; (void)conn_local_ip; (void)conn_primary_port; (void)conn_local_port;
    if (!conn_primary_ip || !st_ip_literal_is_public(conn_primary_ip))
        return CURL_PREREQFUNC_ABORT;
    return CURL_PREREQFUNC_OK;
}
#endif /* CNET_HAVE_CURL -- layer 2 needs libcurl's post-DNS prereq hook.
        * Layer 1 (st_host_allowed) and the IP-literal policy above are pure
        * logic and stay compiled in unconditionally, so
        * cce_safetensors_test_host_policy / _test_ip_public still gate the
        * SSRF name policy on a curl-less build. */

#ifdef CCE_SAFETENSORS_TESTING
int cce_safetensors_test_host_policy(const char* host) { return st_host_allowed(host); }
int cce_safetensors_test_ip_public(const char* ip) { return st_ip_literal_is_public(ip); }
#endif
#endif /* CNET_HAVE_CURL || CCE_SAFETENSORS_TESTING */

/* Pure URL-syntax validation: shape, scheme and credential checks only.
   `is_hf_host` scopes the bearer token; `host_allowed` reports the egress name
   policy. Policy is reported, never enforced here, so callers that only need to
   classify a URL stay separable from the ones that dial it. */
#if CNET_HAVE_CURL
static int st_parse_https_url(const char* url, int* is_hf_host, int* host_allowed) {
    CURLU* parsed = NULL;
    char* scheme = NULL;
    char* host = NULL;
    char* user = NULL;
    char* password = NULL;
    int valid = 0;

    if (!url || !*url || strpbrk(url, "\r\n\t") != NULL) return 0;
    parsed = curl_url();
    if (!parsed) return 0;
    if (curl_url_set(parsed, CURLUPART_URL, url, 0) != CURLUE_OK) goto done;
    if (curl_url_get(parsed, CURLUPART_SCHEME, &scheme, 0) != CURLUE_OK ||
        strcmp(scheme, "https") != 0) goto done;
    if (curl_url_get(parsed, CURLUPART_HOST, &host, 0) != CURLUE_OK || !*host) goto done;
    if (curl_url_get(parsed, CURLUPART_USER, &user, 0) == CURLUE_OK ||
        curl_url_get(parsed, CURLUPART_PASSWORD, &password, 0) == CURLUE_OK) goto done;
    if (is_hf_host) {
        size_t host_len = strlen(host);
        static const char hf_host[] = "huggingface.co";
        size_t hf_len = sizeof(hf_host) - 1;
        *is_hf_host = curl_strequal(host, hf_host) ||
                      (host_len > hf_len &&
                       host[host_len - hf_len - 1] == '.' &&
                       curl_strequal(host + host_len - hf_len, hf_host));
    }
    if (host_allowed) *host_allowed = st_host_allowed(host);
    valid = 1;

done:
    curl_free(scheme);
    curl_free(host);
    curl_free(user);
    curl_free(password);
    curl_url_cleanup(parsed);
    return valid;
}

static int st_is_https_url(const char* url) {
    return st_parse_https_url(url, NULL, NULL);
}

static int st_download_curl(const char* url, const char* dest, const char* bearer) {
    CURL* curl = NULL;
    struct curl_slist* headers = NULL;
    char* auth = NULL;
    FILE* file = NULL;
    CURLcode rc = CURLE_FAILED_INIT;
    st_download_sink sink = {0};

    if (!st_is_https_url(url) || !dest) return -1;
    if (bearer && strpbrk(bearer, "\r\n") != NULL) return -1;

    file = fopen(dest, "wb");
    curl = curl_easy_init();
    if (!file || !curl) goto done;

    sink.file = file;
    sink.limit = CCE_ST_MAX_DOWNLOAD_BYTES;
    if (bearer && *bearer) {
        size_t bearer_len = strlen(bearer);
        const char prefix[] = "Authorization: Bearer ";
        if (bearer_len > SIZE_MAX - sizeof(prefix)) goto done;
        auth = (char*)malloc(sizeof(prefix) + bearer_len);
        if (!auth) goto done;
        memcpy(auth, prefix, sizeof(prefix) - 1);
        memcpy(auth + sizeof(prefix) - 1, bearer, bearer_len + 1);
        headers = curl_slist_append(headers, auth);
        if (!headers) goto done;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, CCE_ST_MAX_REDIRECTS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, CCE_ST_CONNECT_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, CCE_ST_DOWNLOAD_TIMEOUT_MS);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE_LARGE,
                     (curl_off_t)CCE_ST_MAX_DOWNLOAD_BYTES);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "CNET-CCE-safetensors/1.0");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, st_download_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    /* Re-checked on every redirect hop, so FOLLOWLOCATION cannot be steered at
       loopback/RFC1918/metadata addresses that the name policy never sees. */
    curl_easy_setopt(curl, CURLOPT_PREREQFUNCTION, st_prereq_public_ip);

    rc = curl_easy_perform(curl);

done:
    if (file && fclose(file) != 0 && rc == CURLE_OK) rc = CURLE_WRITE_ERROR;
    curl_slist_free_all(headers);
    if (curl) curl_easy_cleanup(curl);
    free(auth);
    if (sink.too_large)
        st_set_err(NULL, "download exceeds %" PRIu64 "-byte limit",
                   CCE_ST_MAX_DOWNLOAD_BYTES);
    else if (rc != CURLE_OK)
        st_set_err(NULL, "HTTPS download failed: %s", curl_easy_strerror(rc));
    return rc == CURLE_OK ? 0 : -1;
}

/* Sole egress choke point: every download (single file and index shards) lands
   here, so the name policy is enforced once, for all of them. */
static int st_download_to_file(const char* url, const char* dest) {
    int is_hf_host = 0;
    int host_allowed = 0;
    const char* token = NULL;
    if (!st_parse_https_url(url, &is_hf_host, &host_allowed)) return -1;
    if (!host_allowed) {
        st_set_err(NULL,
                   "host not permitted by egress policy; set " CCE_ST_ENV_ALLOWLIST
                   " to opt in additional hosts");
        return -1;
    }
    if (is_hf_host) token = st_get_hf_token();
    return st_download_curl(url, dest, token);
}
#endif /* CNET_HAVE_CURL -- st_parse_https_url .. st_download_to_file.
        * URL parsing deliberately uses libcurl's own CURLU parser rather than a
        * hand-rolled one: this is the code that decides whether a host is on the
        * egress allowlist, and reimplementing URL syntax for the no-curl build
        * would mean two parsers that must agree about what a host IS -- exactly
        * the kind of divergence that produces an SSRF bypass. With no curl there
        * is nothing to dial, so the whole layer is absent instead. */

#ifdef CCE_SAFETENSORS_TESTING
#if !CNET_HAVE_CURL
/* Built without libcurl: there is no download path to cap and no CURLU parser
 * to scope a token against. Return 0 (not 1) so a curl-less build can never be
 * mistaken for one that PASSED these checks -- callers must treat 0 as
 * "unproven here", which is what tests/cce_safetensors_test.c does. */
int cce_safetensors_test_download_cap(void) { return 0; }
int cce_safetensors_test_token_scope(void) { return 0; }
#else
int cce_safetensors_test_download_cap(void) {
    unsigned char data[5] = {0};
    FILE* file = tmpfile();
    st_download_sink sink = {file, 0, 4, 0};
    if (!file) return 0;
    size_t first = st_download_write(data, 1, 4, &sink);
    size_t overflow = st_download_write(data + 4, 1, 1, &sink);
    int passed = first == 4 && overflow == CURL_WRITEFUNC_ERROR &&
                 sink.too_large && sink.bytes == 4;
    fclose(file);
    return passed;
}

int cce_safetensors_test_token_scope(void) {
    int is_hf_host = 0;
    return st_parse_https_url("https://huggingface.co/model", &is_hf_host, NULL) &&
           is_hf_host &&
           st_parse_https_url("https://cdn.huggingface.co/model", &is_hf_host, NULL) &&
           is_hf_host &&
           st_parse_https_url("https://huggingface.co.evil.invalid/model",
                              &is_hf_host, NULL) &&
           !is_hf_host &&
           st_parse_https_url("https://example.invalid/model", &is_hf_host, NULL) &&
           !is_hf_host;
}
#endif /* !CNET_HAVE_CURL */
#endif

/* Build HF URL into caller buffer. */
int cce_hf_build_resolve_url(char* buf, size_t cap,
                             const char* repo, const char* filename, const char* revision) {
    if (!buf || cap < 32 || !repo || !filename) return -1;
    const char* rev = (revision && *revision) ? revision : "main";
    /* sanitize lightly: no spaces etc, but we trust caller for now */
    return snprintf(buf, cap,
                    "https://huggingface.co/%s/resolve/%s/%s",
                    repo, rev, filename);
}

#if !CNET_HAVE_CURL
/* No libcurl: fail closed and say so. Loading a LOCAL safetensors file
 * (cce_safetensors_load) is unaffected -- only fetching a remote one is gone. */
cce_result cce_safetensors_load_url(const char* url, cce_safetensors** st_out) {
    if (!url || !st_out) return CCE_ERR_INVALID_ARG;
    *st_out = NULL;
    st_set_err(NULL, "built without libcurl: cannot fetch a SafeTensors URL; "
                     "download the file and use cce_safetensors_load()");
    return CCE_ERR_UNSUPPORTED;
}
#else
cce_result cce_safetensors_load_url(const char* url, cce_safetensors** st_out) {
    if (!url || !st_out) return CCE_ERR_INVALID_ARG;
    *st_out = NULL;
    int host_allowed = 0;
    if (!st_parse_https_url(url, NULL, &host_allowed)) {
        st_set_err(NULL, "SafeTensors URL must be a valid HTTPS URL without credentials");
        return CCE_ERR_INVALID_ARG;
    }
    if (!host_allowed) {
        st_set_err(NULL,
                   "SafeTensors URL host is not on the egress allowlist; set "
                   CCE_ST_ENV_ALLOWLIST " to opt in additional hosts");
        return CCE_ERR_INVALID_ARG;
    }

    char tmp[512];
    if (st_make_temp_path(tmp, sizeof(tmp), "cnet_hf_") != 0) {
        st_set_err(NULL, "cannot create secure temporary download file");
        return CCE_ERR_IO;
    }

    if (st_download_to_file(url, tmp) != 0) {
        remove(tmp);
        return CCE_ERR_IO;
    }

    cce_result rc = cce_safetensors_load(tmp, st_out);

    /* cleanup temp regardless */
    remove(tmp);

    if (rc != CCE_OK && *st_out) {
        cce_safetensors_free(*st_out);
        *st_out = NULL;
    }
    return rc;
}
#endif /* !CNET_HAVE_CURL */

cce_result cce_safetensors_load_hf(cce_safetensors** st_out,
                                   const char* repo,
                                   const char* filename,
                                   const char* revision) {
    if (!repo || !filename) return CCE_ERR_INVALID_ARG;

    char url[1024];
    if (cce_hf_build_resolve_url(url, sizeof(url), repo, filename, revision) < 0) {
        return CCE_ERR_INVALID_ARG;
    }

    return cce_safetensors_load_url(url, st_out);
}

/* Helper: create a 1-block linear cascade from a weight tensor in the safetensors,
   populate it (with transpose for typical torch [out,in] -> our [in,out]), and
   add it to the forest under a clean branch name.
   Returns the branch index or negative on error. */
static int add_linear_specialist_from_st(cce_forest* forest,
                                         const cce_safetensors* st,
                                         const char* weight_name,
                                         const char* branch_name,
                                         float init_scale,
                                         int transpose /* 1 = torch Linear [out,in];
                                                          0 = GPT-2 Conv1D [in,out] */) {
    int wi = cce_safetensors_find(st, weight_name);
    if (wi < 0) return -1;

    cce_tensor w = {0};
    if (cce_safetensors_load_as_tensor(st, wi, &w) != CCE_OK || w.ndim != 2) {
        cce_tensor_free(&w);
        return -1;
    }

    int in_d, out_d;
    if (transpose) { in_d = w.shape[1]; out_d = w.shape[0]; }  /* torch [out, in] */
    else           { in_d = w.shape[0]; out_d = w.shape[1]; }  /* Conv1D [in, out] */

    cce_cascade* cas = NULL;
    if (cce_cascade_create(&cas, 2) != CCE_OK) {
        cce_tensor_free(&w);
        return -1;
    }

    /* IMPORTANT: use a LINEAR_HEAD block (pure affine, no sigmoid). Transformer
       projections (qkv/proj/mlp) and the logits head must be linear; GELU/softmax
       are applied explicitly in the forward. CCE_BLOCK_LINEAR would squash every
       projection (and the logits) through a sigmoid -> garbage. */
    cce_result rc = cce_cascade_add_linear_head(cas, in_d, out_d, init_scale);
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
        cce_tensor_free(&w);
        return -1;
    }

    cce_block* blk = &cas->blocks[0];
    /* Derive the matching bias tensor name ("...weight" -> "...bias"). torch
       nn.Linear stores a bias we must carry over (qkv/proj/mlp all have one;
       the tied-style head has none -> populate_block leaves bias at zero). */
    char bias_name[CCE_ST_MAX_NAME + 8];
    snprintf(bias_name, sizeof(bias_name), "%s", weight_name);
    {
        char* suffix = strstr(bias_name, ".weight");
        if (suffix) memcpy(suffix, ".bias\0\0", 7); else bias_name[0] = 0;
    }
    /* transpose=1: torch Linear [out,in] -> CCE [in,out]; 0: Conv1D copies verbatim */
    rc = cce_safetensors_populate_block(blk, st, weight_name,
                                        bias_name[0] ? bias_name : NULL, transpose);
    if (rc != CCE_OK) {
        /* fallback: direct copy if shapes happened to match */
        if (blk->weights.numel == w.numel) {
            memcpy(blk->weights.data, w.data, w.numel * sizeof(float));
        }
    }

    int branch_idx = -1;
    rc = cce_forest_add_cascade_branch(forest, cas, branch_name, &branch_idx);
    if (rc != CCE_OK) {
        cce_cascade_destroy(cas);
        branch_idx = -1;
    }
    /* Note: forest now owns the cascade */

    cce_tensor_free(&w);
    return branch_idx;
}

/* Checked specialist add: a failed add (missing/bad tensor, forest full or
   archive error) must abort the load — a silently dropped specialist produces
   a model that forwards garbage. Returns 1 on success; on failure records the
   error and reports the exact branch on stderr, returning 0 so the caller can
   fail closed. */
static int supra_add_checked(cce_forest* forest, const cce_safetensors* st,
                             const char* tname, const char* bname,
                             float init_scale, int transpose) {
    if (add_linear_specialist_from_st(forest, st, tname, bname, init_scale, transpose) >= 0)
        return 1;
    st_set_err(NULL, "specialist add failed: %s (from tensor %s)", bname, tname);
    fprintf(stderr, "cce_supra_load_decomposed: failed to add specialist '%s' "
                    "from tensor '%s' -- refusing load\n", bname, tname);
    return 0;
}

#define SUPRA_REPO "SupraLabs/Supra-A2A-Nano-Exp"

/* Resolve the effective cache directory (download-once store). */
static void cce_supra_cache_dir(const char* in, char* out, size_t cap) {
    const char* d = (in && *in) ? in : "supra_cache";
    snprintf(out, cap, "%s", d);
}

/* Best-effort create the cache directory (single level is enough here). */
static void supra_ensure_dir(const char* d) {
    if (!d || !*d || strcmp(d, ".") == 0) return;
#ifdef _WIN32
    _mkdir(d);
#else
    mkdir(d, 0777);
#endif
}

/* Open a repo safetensors file, downloading ONCE into cache_dir and reusing it
   thereafter. On a cache hit we parse the local copy (no network). A cached file
   that fails to parse is treated as corrupt, removed, and re-downloaded. */
static cce_result supra_open_repo_st(const char* cache_dir, const char* filename,
                                     const char* revision, cce_safetensors** out_st) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", cache_dir, filename);

    if (cce_safetensors_load(path, out_st) == CCE_OK) return CCE_OK; /* cache hit */

    remove(path); /* missing or corrupt -> (re)download once */
    char url[1024];
    if (cce_hf_build_resolve_url(url, sizeof(url), SUPRA_REPO, filename, revision) < 0)
        return CCE_ERR_INVALID_ARG;
#if CNET_HAVE_CURL
    if (st_download_to_file(url, path) != 0) { remove(path); return CCE_ERR_IO; }
#else
    st_set_err(NULL, "built without libcurl: cannot download the Supra weights; "
                     "fetch them manually into the cache directory");
    return CCE_ERR_UNSUPPORTED;
#endif

    cce_result rc = cce_safetensors_load(path, out_st);
    if (rc != CCE_OK) remove(path); /* never keep a bad download in the cache */
    return rc;
}

/* Ensure a non-safetensors repo file (e.g. tokenizer.json) is present locally,
   downloading once. Fills out_path with the local cache path. */
static cce_result cce_supra_fetch_file(const char* cache_dir, const char* filename,
                                       const char* revision, char* out_path, size_t cap) {
    snprintf(out_path, cap, "%s/%s", cache_dir, filename);
    FILE* fp = fopen(out_path, "rb");
    if (fp) { fclose(fp); return CCE_OK; } /* cache hit */
    char url[1024];
    if (cce_hf_build_resolve_url(url, sizeof(url), SUPRA_REPO, filename, revision) < 0)
        return CCE_ERR_INVALID_ARG;
#if CNET_HAVE_CURL
    if (st_download_to_file(url, out_path) != 0) { remove(out_path); return CCE_ERR_IO; }
#else
    st_set_err(NULL, "built without libcurl: cannot download the Supra weights; "
                     "fetch them manually into the cache directory");
    return CCE_ERR_UNSUPPORTED;
#endif
    return CCE_OK;
}

/* ---- tensor-name schemas (which checkpoint namings the decomposer speaks) ----
   A small template table, one row per naming family. Per-layer entries are
   printf formats taking the layer index; ".weight"/".bias" are appended.
   conv1d_blocks says how the BLOCK projections are stored on disk:
     0 = torch nn.Linear [out,in]  (transposed into the forest's [in,out])
     1 = GPT-2 Conv1D    [in,out]  (copied verbatim)
   The explicit head (when present) is always nn.Linear [vocab,n_embd] in both
   families (HF GPT-2's lm_head is a Linear even though its blocks are Conv1D);
   a missing head tensor means GPT-2-style weight tying (head = tok_emb). */
typedef struct {
    const char* name;          /* schema id (also published on the struct) */
    const char* tok_emb;       /* [vocab, n_embd] — also the schema probe key */
    const char* pos_emb;       /* [block_size, n_embd] */
    const char* ln1_fmt;       /* per-layer prefixes (".weight"/".bias" appended) */
    const char* ln2_fmt;
    const char* qkv_fmt;       /* fused qkv projection */
    const char* attn_proj_fmt;
    const char* mlp_up_fmt;
    const char* mlp_down_fmt;
    const char* ln_f;          /* final-LN prefix */
    const char* head;          /* explicit logits head weight (absent -> tied) */
    int         conv1d_blocks;
} supra_name_schema;

static const supra_name_schema k_supra_schemas[] = {
    { "supra", "tok_emb.weight", "pos_emb.weight",
      "blocks.%d.ln1",      "blocks.%d.ln2",
      "blocks.%d.attn.qkv", "blocks.%d.attn.proj",
      "blocks.%d.mlp.0",    "blocks.%d.mlp.2",
      "ln_f", "head.weight", 0 },
    { "gpt2",  "wte.weight", "wpe.weight",
      "h.%d.ln_1",          "h.%d.ln_2",
      "h.%d.attn.c_attn",   "h.%d.attn.c_proj",
      "h.%d.mlp.c_fc",      "h.%d.mlp.c_proj",
      "ln_f", "lm_head.weight", 1 },
};

/* Pick the first schema whose embedding-table tensor exists in the file. */
static const supra_name_schema* supra_detect_schema(const cce_safetensors* st) {
    for (size_t s = 0; s < sizeof(k_supra_schemas)/sizeof(k_supra_schemas[0]); ++s)
        if (cce_safetensors_find(st, k_supra_schemas[s].tok_emb) >= 0)
            return &k_supra_schemas[s];
    return NULL;
}

/* Build "<fmt % layer><suffix>" (e.g. "h.%d.ln_1" + 3 + ".weight"). */
static void supra_tname(char* out, size_t cap, const char* fmt, int layer,
                        const char* suffix) {
    char base[CCE_ST_MAX_NAME - 16];   /* headroom for ".weight"/".bias" */
    snprintf(base, sizeof(base), fmt, layer);
    snprintf(out, cap, "%s%s", base, suffix);
}

/* n_layer = number of consecutive per-layer LN1 weight tensors, from 0.
   Cross-check: an LN1 weight numbered >= that count means the layer indices
   are NOT consecutive (e.g. h.0..h.2 present, h.3 missing, h.4 present) and
   a plain count would silently load a truncated model. Returns -1 on such a
   numbering gap so the caller can refuse the load (fail closed). */
static int supra_count_layers(const cce_safetensors* st, const supra_name_schema* sc) {
    char buf[CCE_ST_MAX_NAME];
    char scanfmt[CCE_ST_MAX_NAME + 16];
    int l = 0, i;
    while (l < CCE_ST_MAX_TENSORS) {
        supra_tname(buf, sizeof(buf), sc->ln1_fmt, l, ".weight");
        if (cce_safetensors_find(st, buf) < 0) break;
        ++l;
    }
    /* Gap scan over every tensor name: reuse ln1_fmt as a scanf pattern (its
       "%d" now PARSES the layer index); "%n" pins the full-name match. */
    snprintf(scanfmt, sizeof(scanfmt), "%s.weight%%n", sc->ln1_fmt);
    for (i = 0; i < st->count; ++i) {
        int idx = -1, used = -1;
        if (sscanf(st->metas[i].name, scanfmt, &idx, &used) >= 1 &&
            used == (int)strlen(st->metas[i].name) && idx >= l)
            return -1;
    }
    return l;
}

/* Best-effort integer field scan over a small sibling config.json (the file HF
   checkpoints ship next to the weights, e.g. GPT-2's {"n_head": 12, ...}).
   A scan, not a JSON parser: finds "key" and reads the number after ':'. */
static int supra_config_int(const char* cache_dir, const char* key, int* out_val) {
    char path[512];
    snprintf(path, sizeof(path), "%s/config.json", cache_dir);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    char buf[65536];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = 0;
    char pat[80];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(buf, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == ':') p++;
    if (*p < '0' || *p > '9') return 0;
    *out_val = atoi(p);
    return 1;
}

/* n_head cannot be recovered from tensor shapes. Priority: safetensors
   "__metadata__" (key "n_head"), then config.json ("n_head" or HF's
   "num_attention_heads"). The final 4 is Supra's documented head count and is
   legitimate ONLY for the "supra" schema; for any other schema a guessed
   n_head silently builds wrong attention geometry, so return 0 and let the
   caller refuse the load (fail closed). */
static int supra_resolve_n_head(const cce_safetensors* st, const char* cache_dir,
                                const supra_name_schema* sc) {
    char val[CCE_ST_MAX_NAME];
    int v = 0;
    if (cce_safetensors_meta_lookup(st, "n_head", val, sizeof(val)) &&
        (v = atoi(val)) > 0) return v;
    if (supra_config_int(cache_dir, "n_head", &v) && v > 0) return v;
    if (supra_config_int(cache_dir, "num_attention_heads", &v) && v > 0) return v;
    return (strcmp(sc->name, "supra") == 0) ? 4 : 0;
}

/* Heap LN arrays sized [n_layer] (was fixed [4]). calloc'd so cce_tensor_free
   on never-loaded entries is a no-op. */
static cce_result supra_alloc_ln_arrays(cce_supra_decomposed* m, int n_layer) {
    m->ln1_w = (cce_tensor*)calloc((size_t)n_layer, sizeof(cce_tensor));
    m->ln1_b = (cce_tensor*)calloc((size_t)n_layer, sizeof(cce_tensor));
    m->ln2_w = (cce_tensor*)calloc((size_t)n_layer, sizeof(cce_tensor));
    m->ln2_b = (cce_tensor*)calloc((size_t)n_layer, sizeof(cce_tensor));
    if (!m->ln1_w || !m->ln1_b || !m->ln2_w || !m->ln2_b) {
        free(m->ln1_w); free(m->ln1_b); free(m->ln2_w); free(m->ln2_b);
        m->ln1_w = m->ln1_b = m->ln2_w = m->ln2_b = NULL;
        return CCE_ERR_OOM;
    }
    return CCE_OK;
}

cce_result cce_supra_load_decomposed(cce_supra_decomposed** out, const char* cache_dir,
                                     const char* revision) {
    if (!out) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    cce_supra_decomposed* m = (cce_supra_decomposed*)calloc(1, sizeof(*m));
    if (!m) return CCE_ERR_OOM;

    char cdir[256];
    cce_supra_cache_dir(cache_dir, cdir, sizeof(cdir));
    supra_ensure_dir(cdir);

    /* --- GPT weights (cached: download once, reuse every session) --- */
    cce_safetensors* gpt = NULL;
    cce_result rc = supra_open_repo_st(cdir, "model.safetensors", revision, &gpt);
    if (rc != CCE_OK || !gpt) {
        free(m);
        return rc ? rc : CCE_ERR_IO;
    }

    /* Which naming family is this checkpoint? (schema table, not an if-chain) */
    const supra_name_schema* sc = supra_detect_schema(gpt);
    if (!sc) {
        st_set_err(NULL, "no known tensor-name schema (tried: supra, gpt2)");
        cce_safetensors_free(gpt);
        free(m);
        return CCE_ERR_UNSUPPORTED;
    }
    m->naming_schema = sc->name;
    int transpose = sc->conv1d_blocks ? 0 : 1;

    /* Recover hyperparams: shapes for the sizes, per-layer tensor count for
       n_layer, metadata/config.json for n_head (see supra_resolve_n_head). */
    int wi = cce_safetensors_find(gpt, sc->tok_emb);
    if (wi >= 0) {
        cce_safetensor_meta meta = {0};
        cce_safetensors_get_meta(gpt, wi, &meta);
        m->vocab_size = meta.shape[0];
        m->n_embd     = meta.shape[1];
    }
    wi = cce_safetensors_find(gpt, sc->pos_emb);
    if (wi >= 0) {
        cce_safetensor_meta meta = {0};
        cce_safetensors_get_meta(gpt, wi, &meta);
        m->block_size = meta.shape[0];
    }
    m->n_layer = supra_count_layers(gpt, sc);
    if (m->n_layer < 0) {
        st_set_err(NULL, "layer numbering gap in schema '%s'", sc->name);
        fprintf(stderr, "cce_supra_load_decomposed: layer numbering gap (a "
                        "per-layer tensor exists past the consecutive count) "
                        "-- refusing load\n");
        cce_safetensors_free(gpt);
        free(m);
        return CCE_ERR_UNSUPPORTED;
    }
    m->n_head = supra_resolve_n_head(gpt, cdir, sc);
    if (m->n_head < 1) {
        st_set_err(NULL, "n_head missing for schema '%s'", sc->name);
        fprintf(stderr, "cce_supra_load_decomposed: schema '%s' carries no "
                        "n_head (safetensors __metadata__ \"n_head\" and "
                        "config.json n_head/num_attention_heads all absent) "
                        "-- refusing load\n", sc->name);
        cce_safetensors_free(gpt);
        free(m);
        return CCE_ERR_UNSUPPORTED;
    }
    if (m->n_layer < 1 ||
        (m->n_embd > 0 && m->n_embd % m->n_head != 0)) {
        st_set_err(NULL, "bad recovered config: n_layer=%d n_head=%d n_embd=%d",
                   m->n_layer, m->n_head, m->n_embd);
        cce_safetensors_free(gpt);
        free(m);
        return CCE_ERR_UNSUPPORTED;
    }

    /* Archive for the linear specialists (rebuilt each session from cached weights;
       cheap CPU work — the expensive 118 MB download is what the cache avoids).
       Capacity follows the DISCOVERED n_layer — 4 specialists per block, plus
       the logits head, the optional VQ codebook and spare — never a fixed cap
       (a fixed 64 silently dropped specialists for any model with n_layer >= 16). */
    char tmp_path[320];
    snprintf(tmp_path, sizeof(tmp_path), "%s/supra_a2a_nano_decomp.cce", cdir);
    remove(tmp_path);
    if (cce_forest_open(&m->forest, tmp_path, 4 * m->n_layer + 8) != CCE_OK) {
        cce_safetensors_free(gpt);
        free(m);
        return CCE_ERR_IO;
    }
    if (supra_alloc_ln_arrays(m, m->n_layer) != CCE_OK) {
        cce_safetensors_free(gpt);
        cce_forest_close(m->forest);
        free(m);
        return CCE_ERR_OOM;
    }

    /* Load embedding tables as proper cce_tensor (fix for "raw tables" limitation) */
    if (cce_safetensors_find(gpt, sc->tok_emb) >= 0) {
        cce_safetensors_load_as_tensor(gpt, cce_safetensors_find(gpt, sc->tok_emb), &m->tok_emb);
    }
    if (cce_safetensors_find(gpt, sc->pos_emb) >= 0) {
        cce_safetensors_load_as_tensor(gpt, cce_safetensors_find(gpt, sc->pos_emb), &m->pos_emb);
    }

    /* Load LayerNorm weights (fix for missing real gamma/beta) */
    for (int l = 0; l < m->n_layer; l++) {
        char buf[128];
        supra_tname(buf, sizeof(buf), sc->ln1_fmt, l, ".weight");
        if (cce_safetensors_find(gpt, buf) >= 0)
            cce_safetensors_load_as_tensor(gpt, cce_safetensors_find(gpt, buf), &m->ln1_w[l]);
        supra_tname(buf, sizeof(buf), sc->ln1_fmt, l, ".bias");
        if (cce_safetensors_find(gpt, buf) >= 0)
            cce_safetensors_load_as_tensor(gpt, cce_safetensors_find(gpt, buf), &m->ln1_b[l]);

        supra_tname(buf, sizeof(buf), sc->ln2_fmt, l, ".weight");
        if (cce_safetensors_find(gpt, buf) >= 0)
            cce_safetensors_load_as_tensor(gpt, cce_safetensors_find(gpt, buf), &m->ln2_w[l]);
        supra_tname(buf, sizeof(buf), sc->ln2_fmt, l, ".bias");
        if (cce_safetensors_find(gpt, buf) >= 0)
            cce_safetensors_load_as_tensor(gpt, cce_safetensors_find(gpt, buf), &m->ln2_b[l]);
    }
    {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s.weight", sc->ln_f);
        if (cce_safetensors_find(gpt, buf) >= 0)
            cce_safetensors_load_as_tensor(gpt, cce_safetensors_find(gpt, buf), &m->ln_f_w);
        snprintf(buf, sizeof(buf), "%s.bias", sc->ln_f);
        if (cce_safetensors_find(gpt, buf) >= 0)
            cce_safetensors_load_as_tensor(gpt, cce_safetensors_find(gpt, buf), &m->ln_f_b);
    }

    /* Decompose all linear projections into independent CNet cascades. Branch
       names are the loader's own stable vocabulary ("gpt.block%d.*"), the same
       for every input schema — the forward and the QAT trainer key on them. */
    for (int l = 0; l < m->n_layer; l++) {
        char tname[CCE_ST_MAX_NAME], bname[64];
        int ok = 1;
        supra_tname(tname, sizeof(tname), sc->qkv_fmt, l, ".weight");
        snprintf(bname, sizeof(bname), "gpt.block%d.qkv", l);
        ok = ok && supra_add_checked(m->forest, gpt, tname, bname, 0.02f, transpose);

        supra_tname(tname, sizeof(tname), sc->attn_proj_fmt, l, ".weight");
        snprintf(bname, sizeof(bname), "gpt.block%d.attn_proj", l);
        ok = ok && supra_add_checked(m->forest, gpt, tname, bname, 0.02f, transpose);

        supra_tname(tname, sizeof(tname), sc->mlp_up_fmt, l, ".weight");
        snprintf(bname, sizeof(bname), "gpt.block%d.mlp_up", l);
        ok = ok && supra_add_checked(m->forest, gpt, tname, bname, 0.02f, transpose);

        supra_tname(tname, sizeof(tname), sc->mlp_down_fmt, l, ".weight");
        snprintf(bname, sizeof(bname), "gpt.block%d.mlp_down", l);
        ok = ok && supra_add_checked(m->forest, gpt, tname, bname, 0.02f, transpose);

        /* Any dropped specialist = wrong model: fail closed, free everything. */
        if (!ok) {
            cce_safetensors_free(gpt);
            cce_supra_free_decomposed(m);
            return CCE_ERR_UNSUPPORTED;
        }
    }

    /* Logits head: explicit nn.Linear [vocab, n_embd] when present; otherwise
       GPT-2-style weight tying — the embedding table [vocab, n_embd] IS the
       [out,in] head matrix, so the same transposing add builds "gpt.head". */
    if (cce_safetensors_find(gpt, sc->head) >= 0) {
        if (!supra_add_checked(m->forest, gpt, sc->head, "gpt.head", 0.02f, 1)) {
            cce_safetensors_free(gpt);
            cce_supra_free_decomposed(m);
            return CCE_ERR_UNSUPPORTED;
        }
    } else {
        if (!supra_add_checked(m->forest, gpt, sc->tok_emb, "gpt.head", 0.02f, 1)) {
            cce_safetensors_free(gpt);
            cce_supra_free_decomposed(m);
            return CCE_ERR_UNSUPPORTED;
        }
        m->head_tied = 1;
    }

    cce_safetensors_free(gpt);

    /* --- VQ-VAE full weights (4D convs for patch + linear style mapping) ---
       Supra-only sidecar: other schemas (GPT-2 style) have no VQ file, so do
       not attempt the download-once fetch for them. */
    cce_safetensors* vq = NULL;
    rc = (strcmp(sc->name, "supra") == 0)
             ? supra_open_repo_st(cdir, "vqvae.safetensors", revision, &vq)
             : CCE_ERR_NOT_FOUND;
    if (rc == CCE_OK && vq) {
        /* The sidecar FILE is optional; once present, its codebook add is not. */
        if (!supra_add_checked(m->forest, vq, "vq.embedding.weight", "vqvae.codebook", 0.0f, 1)) {
            cce_safetensors_free(vq);
            cce_supra_free_decomposed(m);
            return CCE_ERR_UNSUPPORTED;
        }
        if (cce_safetensors_find(vq, "vq.embedding.weight") >= 0) {
            cce_safetensors_load_as_tensor(vq, cce_safetensors_find(vq, "vq.embedding.weight"), &m->vq_codebook);
            m->vq_codebook_size = m->vq_codebook.shape[0];
            m->vq_code_dim      = m->vq_codebook.shape[1];
        }

        // load all 3 encoder and decoder layers (4D tensors)
        const char* enc_names[] = {"enc.0", "enc.1", "enc.2"};
        const char* dec_names[] = {"dec.0", "dec.1", "dec.2"};
        for (int i=0; i<3; i++) {
            char wname[64], bname[64];
            snprintf(wname, sizeof(wname), "%s.weight", enc_names[i]);
            snprintf(bname, sizeof(bname), "%s.bias", enc_names[i]);
            if (cce_safetensors_find(vq, wname) >= 0)
                cce_safetensors_load_as_tensor(vq, cce_safetensors_find(vq, wname), &m->vq_enc_w[i]);
            if (cce_safetensors_find(vq, bname) >= 0)
                cce_safetensors_load_as_tensor(vq, cce_safetensors_find(vq, bname), &m->vq_enc_b[i]);

            snprintf(wname, sizeof(wname), "%s.weight", dec_names[i]);
            snprintf(bname, sizeof(bname), "%s.bias", dec_names[i]);
            if (cce_safetensors_find(vq, wname) >= 0)
                cce_safetensors_load_as_tensor(vq, cce_safetensors_find(vq, wname), &m->vq_dec_w[i]);
            if (cce_safetensors_find(vq, bname) >= 0)
                cce_safetensors_load_as_tensor(vq, cce_safetensors_find(vq, bname), &m->vq_dec_b[i]);
        }

        cce_safetensors_free(vq);
    }

    /* Fill remaining hyperparams if not set */
    if (m->n_embd == 0) m->n_embd = 256;
    if (m->block_size == 0) m->block_size = 384;
    if (m->vocab_size == 0) m->vocab_size = 50520; /* approx from doc */
    m->context_routing_mode = CCE_CONTEXT_ROUTING_FULL_KV;
    cce_specialist_kv_budget_default(&m->kv_budget, m->block_size);

    *out = m;
    return CCE_OK;
}

void cce_supra_free_decomposed(cce_supra_decomposed* m) {
    if (!m) return;
    if (m->forest) cce_forest_close(m->forest);
    cce_tensor_free(&m->tok_emb);
    cce_tensor_free(&m->pos_emb);
    for (int i = 0; i < m->n_layer; i++) {
        if (m->ln1_w) cce_tensor_free(&m->ln1_w[i]);
        if (m->ln1_b) cce_tensor_free(&m->ln1_b[i]);
        if (m->ln2_w) cce_tensor_free(&m->ln2_w[i]);
        if (m->ln2_b) cce_tensor_free(&m->ln2_b[i]);
    }
    free(m->ln1_w); free(m->ln1_b); free(m->ln2_w); free(m->ln2_b);
    cce_tensor_free(&m->ln_f_w);
    cce_tensor_free(&m->ln_f_b);
    for (int i=0; i<3; i++) {
        cce_tensor_free(&m->vq_enc_w[i]); cce_tensor_free(&m->vq_enc_b[i]);
        cce_tensor_free(&m->vq_dec_w[i]); cce_tensor_free(&m->vq_dec_b[i]);
    }
    cce_tensor_free(&m->vq_codebook);
    free(m->tok_emb_trit);
    free(m->tok_emb_scale);
    cce_compression_grads_free(&m->compression_grads);
    free(m);
}

cce_result cce_supra_set_context_routing(cce_supra_decomposed* m,
                                         cce_context_routing_mode mode,
                                         const cce_specialist_kv_budget* budget) {
    if (!m) return CCE_ERR_INVALID_ARG;
    if (mode != CCE_CONTEXT_ROUTING_FULL_KV && mode != CCE_CONTEXT_ROUTING_SPARSE_ROUTING)
        return CCE_ERR_INVALID_ARG;
    m->context_routing_mode = mode;
    if (budget) {
        m->kv_budget = *budget;
    } else {
        cce_specialist_kv_budget_default(&m->kv_budget, m->block_size);
    }
    return CCE_OK;
}

cce_result cce_supra_enable_gradient_accumulation(cce_supra_decomposed* m,
                                                  size_t grad_count) {
    if (!m || grad_count == 0) return CCE_ERR_INVALID_ARG;
    cce_compression_grads_free(&m->compression_grads);
    return cce_compression_grads_init(&m->compression_grads, grad_count);
}

void cce_supra_clear_gradient_accumulation(cce_supra_decomposed* m) {
    if (!m) return;
    cce_compression_grads_free(&m->compression_grads);
}

cce_result cce_supra_accumulate_compression_gradient(cce_supra_decomposed* m,
                                                     const float* identity_grad,
                                                     const float* residual_grad,
                                                     size_t grad_count,
                                                     float identity_scale,
                                                     float residual_scale) {
    if (!m) return CCE_ERR_INVALID_ARG;
    if (!m->compression_grads.values) {
        cce_result rc = cce_supra_enable_gradient_accumulation(m, grad_count);
        if (rc != CCE_OK) return rc;
    }
    return cce_compression_grads_accumulate(&m->compression_grads,
                                            identity_grad,
                                            residual_grad,
                                            grad_count,
                                            identity_scale,
                                            residual_scale);
}

/* int8 weight-only PTQ over every linear specialist in the forest (qkv/proj/mlp/
   head). Returns the number of blocks quantized, or -1 on bad arg. Embedding
   tables stay FP32 (they're lookups, not matmuls). */
int cce_supra_quantize_int8(cce_supra_decomposed* m) {
    if (!m || !m->forest) return -1;
    int n = 0;
    for (int b = 0; b < m->forest->num_branches; ++b) {
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; ++j)
            if (cce_block_quantize_int8(&cas->blocks[j]) == CCE_OK) n++;
    }
    return n;
}

/* BitNet b1.58 ternary PTQ over all linear specialists + the tok_emb lookup
   (ternarized on the fly per row). Post-hoc only. */
int cce_supra_quantize_ternary(cce_supra_decomposed* m) {
    if (!m || !m->forest) return -1;
    int n = 0;
    for (int b = 0; b < m->forest->num_branches; ++b) {
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; ++j)
            if (cce_block_quantize_ternary(&cas->blocks[j]) == CCE_OK) n++;
    }
    m->tok_emb_ternary = 1;   /* embedding lookup uses ternary too (debug ref) */
    return n;
}

/* Trit-pack the tok_emb table (per-row absmean ternary, 5/byte) and FREE the FP
   table. Returns packed bytes (trits + scales). Bit-exact with the ternary lookup. */
static long supra_pack_tok_emb(cce_supra_decomposed* m) {
    if (m->tok_emb_packed) return 0;
    if (!m->tok_emb.data) return 0;
    int V = m->tok_emb.shape[0], D = m->tok_emb.shape[1];
    int bpr = (D + 4) / 5;
    uint8_t* trit = (uint8_t*)malloc((size_t)V * bpr);
    float*   scl  = (float*)malloc((size_t)V * sizeof(float));
    if (!trit || !scl) { free(trit); free(scl); return 0; }
    for (int r = 0; r < V; ++r) {
        const float* row = m->tok_emb.data + (size_t)r * D;
        float s = 0; for (int i = 0; i < D; ++i) s += fabsf(row[i]);
        float g = D ? s / (float)D : 0.0f; scl[r] = g;
        uint8_t* tr = trit + (size_t)r * bpr;
        int o = 0;
        while (o < D) {
            int byte = 0, mul = 1;
            for (int k = 0; k < 5; ++k) {
                int code = 0;
                if (o < D) { if (g > 0.0f) { float rr = roundf(row[o]/g); if (rr>1)rr=1; if (rr<-1)rr=-1; code=(int)rr; } o++; }
                byte += (code + 1) * mul; mul *= 3;
            }
            *tr++ = (uint8_t)byte;
        }
    }
    m->tok_emb_trit = trit; m->tok_emb_scale = scl; m->tok_emb_trit_bpr = bpr;
    m->tok_emb_packed = 1; m->tok_emb_ternary = 0;
    cce_tensor_free(&m->tok_emb);   /* drop the FP table */
    return (long)V * bpr + (long)V * (long)sizeof(float);
}

/* Pack every specialist (incl. the head) to 1.6-bit trits. Ternary-quantizes any
   block not already ternary, then packs + frees the int8 codes. Returns the byte
   size of the packed weights (trits + per-output scales), or -1 on bad arg. */
long cce_supra_pack_trits(cce_supra_decomposed* m) {
    if (!m || !m->forest) return -1;
    long bytes = 0;
    for (int b = 0; b < m->forest->num_branches; ++b) {
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas) continue;
        for (int j = 0; j < cas->num_blocks; ++j) {
            cce_block* blk = &cas->blocks[j];
            if (!blk->w_q && !blk->w_trit) cce_block_quantize_ternary(blk);
            if (cce_block_pack_trits(blk) == CCE_OK && blk->w_trit) {
                int in_dim = blk->weights.shape[0], out_dim = blk->weights.shape[1];
                bytes += (long)in_dim * blk->w_trit_bpr;  /* packed trits */
                bytes += (long)out_dim * (long)sizeof(float);  /* per-output scales */
            }
        }
    }
    bytes += supra_pack_tok_emb(m);   /* pack the embedding table too */
    return bytes;
}

/* =================== Packed-Supra on-disk artifact (export / reload) ===================
   A self-contained 1.6-bit artifact: packed-trit specialists (incl. head) + packed
   tok_emb + FP pos_emb/LayerNorm/biases. The reloaded model has NO FP big tensors
   and runs through the SAME verified cce_supra_gpt_forward. */
#define SUPRA_PACK_MAGIC 0x4B505553u  /* 'S','U','P','K' */

static void st_write_tensor(FILE* f, const cce_tensor* t) {
    int nd = t->ndim; fwrite(&nd, sizeof(int), 1, f);
    fwrite(t->shape, sizeof(int), (size_t)nd, f);
    fwrite(t->data, sizeof(float), t->numel, f);
}
static cce_result st_read_tensor(FILE* f, cce_tensor* t) {
    int nd = 0; if (fread(&nd, sizeof(int), 1, f) != 1 || nd < 1 || nd > CCE_MAX_DIMS) return CCE_ERR_IO;
    int sh[CCE_MAX_DIMS]; if (fread(sh, sizeof(int), (size_t)nd, f) != (size_t)nd) return CCE_ERR_IO;
    if (cce_tensor_alloc(t, sh, nd) != CCE_OK) return CCE_ERR_OOM;
    if (fread(t->data, sizeof(float), t->numel, f) != t->numel) return CCE_ERR_IO;
    return CCE_OK;
}

/* Build a HOT forest branch holding a single packed-trit block (no FP weights). */
static cce_result add_packed_branch(cce_forest* fr, const char* name, int in, int out, int bpr,
                                    const uint8_t* trit, const float* scale, const float* bias) {
    if (fr->num_branches >= fr->max_branches) return CCE_ERR_INVALID_ARG;
    cce_cascade* cas = NULL;
    if (cce_cascade_create(&cas, 1) != CCE_OK) return CCE_ERR_OOM;
    cas->num_blocks = 1;
    cce_block* blk = &cas->blocks[0];
    memset(blk, 0, sizeof(*blk));
    blk->type = CCE_BLOCK_LINEAR_HEAD;
    blk->weights.ndim = 2; blk->weights.shape[0] = in; blk->weights.shape[1] = out;
    blk->weights.numel = (size_t)in * out; blk->weights.data = NULL; blk->weights.owns_memory = 0;
    int bsh[1] = { out };
    cce_tensor_alloc(&blk->bias, bsh, 1); memcpy(blk->bias.data, bias, (size_t)out * sizeof(float));
    blk->w_trit = (uint8_t*)malloc((size_t)in * bpr); memcpy(blk->w_trit, trit, (size_t)in * bpr);
    blk->w_trit_bpr = bpr;
    blk->w_scale = (float*)malloc((size_t)out * sizeof(float)); memcpy(blk->w_scale, scale, (size_t)out * sizeof(float));
    int idx = fr->num_branches++;
    fr->branches[idx].cascade = cas;
    snprintf(fr->branches[idx].name, sizeof(fr->branches[idx].name), "%s", name);
    fr->branches[idx].tier = CCE_TIER_HOT; fr->branches[idx].is_view = 0; fr->branches[idx].persisted = 0;
    return CCE_OK;
}

cce_result cce_supra_export_packed(cce_supra_decomposed* m, const char* path) {
    if (!m || !path || !m->tok_emb_packed || !m->forest) return CCE_ERR_INVALID_ARG;
    FILE* f = fopen(path, "wb"); if (!f) return CCE_ERR_IO;
    uint32_t magic = SUPRA_PACK_MAGIC, ver = 1;
    fwrite(&magic, 4, 1, f); fwrite(&ver, 4, 1, f);
    int hp[5] = { m->n_layer, m->n_embd, m->block_size, m->n_head, m->vocab_size };
    fwrite(hp, sizeof(int), 5, f);
    int te[3] = { m->vocab_size, m->n_embd, m->tok_emb_trit_bpr };
    fwrite(te, sizeof(int), 3, f);
    fwrite(m->tok_emb_trit, 1, (size_t)m->vocab_size * m->tok_emb_trit_bpr, f);
    fwrite(m->tok_emb_scale, sizeof(float), (size_t)m->vocab_size, f);
    st_write_tensor(f, &m->pos_emb);
    /* n_layer LN pairs (n_layer is in the header above; legacy artifacts hold 4) */
    for (int l = 0; l < m->n_layer; l++) { st_write_tensor(f, &m->ln1_w[l]); st_write_tensor(f, &m->ln1_b[l]); st_write_tensor(f, &m->ln2_w[l]); st_write_tensor(f, &m->ln2_b[l]); }
    st_write_tensor(f, &m->ln_f_w); st_write_tensor(f, &m->ln_f_b);
    int ns = m->forest->num_branches; fwrite(&ns, sizeof(int), 1, f);
    for (int b = 0; b < ns; b++) {
        cce_cascade* cas = m->forest->branches[b].cascade;
        if (!cas || cas->num_blocks < 1 || !cas->blocks[0].w_trit) { int z=0; fwrite(&z,sizeof(int),1,f); continue; }
        cce_block* blk = &cas->blocks[0];
        int in = blk->weights.shape[0], out = blk->weights.shape[1], bpr = blk->w_trit_bpr;
        int valid = 1; fwrite(&valid, sizeof(int), 1, f);
        char name[64]; memset(name, 0, 64); snprintf(name, sizeof(name), "%s", m->forest->branches[b].name);
        fwrite(name, 1, 64, f);
        int dims[3] = { in, out, bpr }; fwrite(dims, sizeof(int), 3, f);
        fwrite(blk->w_trit, 1, (size_t)in * bpr, f);
        fwrite(blk->w_scale, sizeof(float), (size_t)out, f);
        fwrite(blk->bias.data, sizeof(float), (size_t)out, f);
    }
    fclose(f);
    return CCE_OK;
}

cce_result cce_supra_load_packed(cce_supra_decomposed** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    FILE* f = fopen(path, "rb"); if (!f) return CCE_ERR_IO;
    uint32_t magic = 0, ver = 0;
    if (fread(&magic, 4, 1, f) != 1 || magic != SUPRA_PACK_MAGIC) { fclose(f); return CCE_ERR_UNSUPPORTED; }
    if (fread(&ver, 4, 1, f) != 1) { fclose(f); return CCE_ERR_IO; }
    cce_supra_decomposed* m = (cce_supra_decomposed*)calloc(1, sizeof(*m));
    if (!m) { fclose(f); return CCE_ERR_OOM; }
    int hp[5]; if (fread(hp, sizeof(int), 5, f) != 5) { free(m); fclose(f); return CCE_ERR_IO; }
    m->n_layer = hp[0]; m->n_embd = hp[1]; m->block_size = hp[2]; m->n_head = hp[3]; m->vocab_size = hp[4];
    if (m->n_layer < 1 || m->n_layer > CCE_ST_MAX_TENSORS) { free(m); fclose(f); return CCE_ERR_UNSUPPORTED; }
    if (supra_alloc_ln_arrays(m, m->n_layer) != CCE_OK) { free(m); fclose(f); return CCE_ERR_OOM; }
    m->context_routing_mode = CCE_CONTEXT_ROUTING_FULL_KV;
    cce_specialist_kv_budget_default(&m->kv_budget, m->block_size);
    /* From here on the LN arrays are live: every error exit must tear down via
       cce_supra_free_decomposed (a bare free(m) leaks ln1_w/ln1_b/ln2_w/ln2_b). */
    int te[3]; if (fread(te, sizeof(int), 3, f) != 3) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO; }
    if (te[0] <= 0 || te[2] <= 0 || (size_t)te[0] > SIZE_MAX / (size_t)te[2]) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_UNSUPPORTED; }
    size_t tok_emb_bytes = (size_t)te[0] * (size_t)te[2];
    m->tok_emb_trit_bpr = te[2]; m->tok_emb_packed = 1;
    m->tok_emb_trit  = (uint8_t*)malloc(tok_emb_bytes);
    m->tok_emb_scale = (float*)malloc((size_t)te[0] * sizeof(float));
    if (!m->tok_emb_trit || !m->tok_emb_scale) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_OOM; }
    if (fread(m->tok_emb_trit, 1, tok_emb_bytes, f) != tok_emb_bytes) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO; }
    if (fread(m->tok_emb_scale, sizeof(float), (size_t)te[0], f) != (size_t)te[0]) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO; }
    st_read_tensor(f, &m->pos_emb);
    for (int l = 0; l < m->n_layer; l++) { st_read_tensor(f, &m->ln1_w[l]); st_read_tensor(f, &m->ln1_b[l]); st_read_tensor(f, &m->ln2_w[l]); st_read_tensor(f, &m->ln2_b[l]); }
    st_read_tensor(f, &m->ln_f_w); st_read_tensor(f, &m->ln_f_b);
    int ns = 0; if (fread(&ns, sizeof(int), 1, f) != 1) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO; }
    char arch[256]; snprintf(arch, sizeof(arch), "supra_packed_reload.cce"); remove(arch);
    if (cce_forest_open(&m->forest, arch, ns + 4) != CCE_OK) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO; }
    for (int b = 0; b < ns; b++) {
        int valid = 0;
        if (fread(&valid, sizeof(int), 1, f) != 1) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO; }
        if (!valid) continue;
        char name[64];
        if (fread(name, 1, 64, f) != 64) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO; }
        int dims[3];
        if (fread(dims, sizeof(int), 3, f) != 3) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO; }
        int in = dims[0], out = dims[1], bpr = dims[2];
        if (in <= 0 || out <= 0 || bpr <= 0 || (size_t)in > SIZE_MAX / (size_t)bpr) { cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_UNSUPPORTED; }
        size_t trit_bytes = (size_t)in * (size_t)bpr;
        uint8_t* trit = (uint8_t*)malloc(trit_bytes);
        float* scale = (float*)malloc((size_t)out * sizeof(float));
        float* bias  = (float*)malloc((size_t)out * sizeof(float));
        if (!trit || !scale || !bias) { free(trit); free(scale); free(bias); cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_OOM; }
        if (fread(trit, 1, trit_bytes, f) != trit_bytes
            || fread(scale, sizeof(float), (size_t)out, f) != (size_t)out
            || fread(bias, sizeof(float), (size_t)out, f) != (size_t)out) {
            free(trit); free(scale); free(bias); cce_supra_free_decomposed(m); fclose(f); return CCE_ERR_IO;
        }
        cce_result add_result = add_packed_branch(m->forest, name, in, out, bpr, trit, scale, bias);
        free(trit); free(scale); free(bias);
        if (add_result != CCE_OK) { cce_supra_free_decomposed(m); fclose(f); return add_result; }
    }
    fclose(f);
    *out = m;
    return CCE_OK;
}

/* ------------------- Full forward glue (fixes the "needs glue" limitation) ------------------- */

/* Apply a single-vector linear specialist cascade across every row of a
   [T, in_dim] matrix, writing [T, out_dim]. The CCE cascade forward operates on
   one vector at a time (and reassigns its output tensor), so we drive it per row
   and copy results into the destination. `in` and `out` are pre-allocated 2-D. */
static cce_result apply_linear_rows(cce_cascade* cas, const cce_tensor* in, cce_tensor* out) {
    if (!cas || !in || !out || in->ndim != 2 || out->ndim != 2) return CCE_ERR_INVALID_ARG;
    int T    = in->shape[0];
    int din  = in->shape[1];
    int dout = out->shape[1];
    if (out->shape[0] != T) return CCE_ERR_INVALID_ARG;

    cce_tensor row_in = {0};
    int ish[1] = { din };
    if (cce_tensor_alloc(&row_in, ish, 1) != CCE_OK) return CCE_ERR_OOM;

    for (int t = 0; t < T; t++) {
        memcpy(row_in.data, in->data + (size_t)t * din, (size_t)din * sizeof(float));
        cce_tensor row_out = {0};                  /* cascade_forward allocates this */
        cce_result r = cce_cascade_forward(cas, &row_in, &row_out);
        if (r != CCE_OK) { cce_tensor_free(&row_in); return r; }
        if (row_out.numel != (size_t)dout) { cce_tensor_free(&row_out); cce_tensor_free(&row_in); return CCE_ERR_UNSUPPORTED; }
        memcpy(out->data + (size_t)t * dout, row_out.data, (size_t)dout * sizeof(float));
        cce_tensor_free(&row_out);
    }
    cce_tensor_free(&row_in);
    return CCE_OK;
}

/* Perform one transformer block using the decomposed specialists.
   This version properly splits the fused QKV and does multi-head attention
   using only cce_tensor ops + the specialist cascades for the linears. */
static cce_result supra_block_forward(cce_supra_decomposed* m, int layer,
                                      cce_tensor* x /* in/out [T, n_embd] */) {
    char name[128];
    int n_embd   = m->n_embd;
    int n_head   = m->n_head;
    int head_dim = n_embd / n_head;
    int T        = x->shape[0];

    /* Resolve all specialist projections up front. */
    snprintf(name, sizeof(name), "gpt.block%d.qkv", layer);
    cce_cascade* qkv_cas  = get_cascade_by_name(m->forest, name);
    snprintf(name, sizeof(name), "gpt.block%d.attn_proj", layer);
    cce_cascade* proj_cas = get_cascade_by_name(m->forest, name);
    snprintf(name, sizeof(name), "gpt.block%d.mlp_up", layer);
    cce_cascade* up_cas   = get_cascade_by_name(m->forest, name);
    snprintf(name, sizeof(name), "gpt.block%d.mlp_down", layer);
    cce_cascade* down_cas = get_cascade_by_name(m->forest, name);
    if (!qkv_cas || !proj_cas || !up_cas || !down_cas) return CCE_ERR_NOT_FOUND;

    /* MLP hidden width = up-projection output dim (e.g. 4*n_embd). */
    int mlp_hidden = up_cas->blocks[up_cas->num_blocks - 1].weights.shape[1];

    cce_tensor ln_out = {0}, qkv = {0}, attn_proj_in = {0}, after_attn = {0},
               mlp_in = {0}, mlp_mid = {0}, mlp_out = {0};
    int sh2[2]    = {T, n_embd};
    int qkv_sh[2] = {T, 3 * n_embd};
    int mid_sh[2] = {T, mlp_hidden};
    cce_tensor_alloc(&ln_out, sh2, 2);
    cce_tensor_alloc(&qkv, qkv_sh, 2);
    cce_tensor_alloc(&attn_proj_in, sh2, 2);
    cce_tensor_alloc(&after_attn, sh2, 2);
    cce_tensor_alloc(&mlp_in, sh2, 2);
    cce_tensor_alloc(&mlp_mid, mid_sh, 2);
    cce_tensor_alloc(&mlp_out, sh2, 2);

    cce_result rc;

    /* LN1 + fused QKV projection (pure linear, applied per token row). */
    cce_tensor_layer_norm(x, &m->ln1_w[layer], &m->ln1_b[layer], 1e-5f, &ln_out);
    rc = apply_linear_rows(qkv_cas, &ln_out, &qkv);
    if (rc != CCE_OK) goto done;

    /* Multi-head attention */
    cce_tensor qh, kh, vh, scores, outh;
    int hd_sh[2] = {T, head_dim};
    int sc_sh[2] = {T, T};
    cce_tensor_alloc(&qh, hd_sh, 2);
    cce_tensor_alloc(&kh, hd_sh, 2);
    cce_tensor_alloc(&vh, hd_sh, 2);
    cce_tensor_alloc(&scores, sc_sh, 2);
    cce_tensor_alloc(&outh, hd_sh, 2);

    float scale = 1.0f / sqrtf((float)head_dim);

    /* For each head: extract, score, mask, softmax, value */
    for (int h = 0; h < n_head; h++) {
        /* Extract q/k/v for this head from the fused [T, 3D] */
        for (int t = 0; t < T; t++) {
            float* qbase = qkv.data + t * (3 * n_embd) + h * head_dim;
            float* kbase = qbase + n_embd;
            float* vbase = kbase + n_embd;
            memcpy(qh.data + t * head_dim, qbase, head_dim * sizeof(float));
            memcpy(kh.data + t * head_dim, kbase, head_dim * sizeof(float));
            memcpy(vh.data + t * head_dim, vbase, head_dim * sizeof(float));
        }

        /* scores = qh @ kh^T : kht must be shaped [head_dim, T] so the matmul
           dims line up (qh[T,head_dim] @ kht[head_dim,T] -> [T,T]). Allocating it
           as [T,head_dim] makes cce_tensor_matmul reject the call (b->shape[0]!=k)
           and leave `scores` zeroed -> uniform attention for every t>0. */
        cce_tensor kht;
        int kht_sh[2] = {head_dim, T};
        cce_tensor_alloc(&kht, kht_sh, 2);
        for (int i = 0; i < T; i++)
            for (int j = 0; j < head_dim; j++)
                kht.data[j * T + i] = kh.data[i * head_dim + j];

        cce_result mrc = cce_tensor_matmul(&qh, &kht, &scores);
        if (mrc != CCE_OK) { cce_tensor_free(&kht); rc = mrc; break; }
        for (size_t i = 0; i < scores.numel; i++) scores.data[i] *= scale;

        /* causal mask */
        for (int i = 0; i < T; i++)
            for (int j = i + 1; j < T; j++)
                scores.data[i * T + j] = -1e9f;

        cce_tensor_softmax(&scores, &scores);

        /* outh = scores @ vh */
        cce_tensor_matmul(&scores, &vh, &outh);

        /* write back to attn_proj_in (concat heads) */
        for (int t = 0; t < T; t++)
            memcpy(attn_proj_in.data + t * n_embd + h * head_dim,
                   outh.data + t * head_dim, head_dim * sizeof(float));

        cce_tensor_free(&kht);
    }

    cce_tensor_free(&qh); cce_tensor_free(&kh); cce_tensor_free(&vh);
    cce_tensor_free(&scores); cce_tensor_free(&outh);
    if (rc != CCE_OK) goto done;   /* a head matmul failed */

    /* Attention output projection (pure linear) + residual. */
    rc = apply_linear_rows(proj_cas, &attn_proj_in, &after_attn);
    if (rc != CCE_OK) goto done;
    cce_tensor_add(x, &after_attn, &after_attn);   /* after_attn = x + attn_out */

    /* LN2 + MLP (up -> GELU -> down) + residual. */
    cce_tensor_layer_norm(&after_attn, &m->ln2_w[layer], &m->ln2_b[layer], 1e-5f, &mlp_in);
    rc = apply_linear_rows(up_cas, &mlp_in, &mlp_mid);
    if (rc != CCE_OK) goto done;
    cce_tensor_gelu(&mlp_mid, &mlp_mid);
    rc = apply_linear_rows(down_cas, &mlp_mid, &mlp_out);
    if (rc != CCE_OK) goto done;
    cce_tensor_add(&after_attn, &mlp_out, x);       /* x = after_attn + mlp_out */

done:
    cce_tensor_free(&ln_out);
    cce_tensor_free(&qkv);
    cce_tensor_free(&attn_proj_in);
    cce_tensor_free(&after_attn);
    cce_tensor_free(&mlp_in);
    cce_tensor_free(&mlp_mid);
    cce_tensor_free(&mlp_out);
    return rc;
}

/* Fill one token's embedding into dst[n_embd]: packed-trit, on-the-fly ternary,
   or FP, matching BitNet b1.58 (effective = gamma * {-1,0,+1}). */
static void supra_embed_row(const cce_supra_decomposed* m, int tok, float* dst) {
    int D = m->n_embd;
    if (tok < 0 || tok >= m->vocab_size) { memset(dst, 0, (size_t)D * sizeof(float)); return; }
    if (m->tok_emb_packed) {
        const uint8_t* p = m->tok_emb_trit + (size_t)tok * m->tok_emb_trit_bpr;
        float g = m->tok_emb_scale[tok];
        int i = 0;
        while (i < D) { unsigned b = *p++; for (int k = 0; k < 5 && i < D; ++k, ++i) { int code = (int)(b % 3) - 1; b /= 3; dst[i] = g * (float)code; } }
    } else if (m->tok_emb_ternary) {
        const float* row = m->tok_emb.data + (size_t)tok * D;
        float s = 0; for (int i = 0; i < D; ++i) s += fabsf(row[i]);
        float g = D ? s / (float)D : 0.0f;
        for (int i = 0; i < D; ++i) {
            if (g <= 0.0f) { dst[i] = 0.0f; continue; }
            float r = roundf(row[i] / g); if (r > 1) r = 1; if (r < -1) r = -1; dst[i] = g * r;
        }
    } else {
        memcpy(dst, m->tok_emb.data + (size_t)tok * D, (size_t)D * sizeof(float));
    }
}

/* High level: embed + N blocks + final ln + head (logits for the last token). */
cce_result cce_supra_gpt_forward(cce_supra_decomposed* m, const int* token_ids, int n_tokens,
                                 float* logits_out, int logits_cap) {
    if (!m || !token_ids || n_tokens <= 0 || !logits_out) return CCE_ERR_INVALID_ARG;

    int T = n_tokens;
    int D = m->n_embd;

    cce_tensor x = {0};
    int xsh[2] = {T, D};
    if (cce_tensor_alloc(&x, xsh, 2) != CCE_OK) return CCE_ERR_OOM;

    /* token embedding (packed-trit / ternary / FP) + positional embedding */
    for (int t = 0; t < T; t++) {
        supra_embed_row(m, token_ids[t], x.data + (size_t)t*D);
        for (int d = 0; d < D; d++)
            x.data[(size_t)t*D + d] += m->pos_emb.data[(size_t)t * D + d];
    }

    cce_result rc = CCE_OK;
    for (int l = 0; l < m->n_layer; l++) {
        rc = supra_block_forward(m, l, &x);
        if (rc != CCE_OK) { cce_tensor_free(&x); return rc; }
    }

    /* final layer norm */
    cce_tensor lnf = {0};
    cce_tensor_alloc(&lnf, xsh, 2);
    cce_tensor_layer_norm(&x, &m->ln_f_w, &m->ln_f_b, 1e-5f, &lnf);

    /* logits head — only the LAST position is needed for next-token generation,
       so we run the (vocab-sized) head on a single row to avoid a [T, vocab] blowup. */
    cce_cascade* head_cas = get_cascade_by_name(m->forest, "gpt.head");
    if (!head_cas) { cce_tensor_free(&x); cce_tensor_free(&lnf); return CCE_ERR_NOT_FOUND; }

    cce_tensor last_in = {0}, logit_row = {0};
    int ish[1] = { D };
    cce_tensor_alloc(&last_in, ish, 1);
    memcpy(last_in.data, lnf.data + (size_t)(T - 1) * D, (size_t)D * sizeof(float));

    rc = cce_cascade_forward(head_cas, &last_in, &logit_row);   /* [vocab] */
    if (rc == CCE_OK) {
        int vocab = (int)logit_row.numel;
        int copy  = (vocab < logits_cap) ? vocab : logits_cap;
        memcpy(logits_out, logit_row.data, (size_t)copy * sizeof(float));
    }

    cce_tensor_free(&last_in);
    cce_tensor_free(&logit_row);
    cce_tensor_free(&lnf);
    cce_tensor_free(&x);
    return rc;
}

/* Head-only QAT support: run the frozen forward through ln_f and copy the
   final-position hidden vector (n_embd values) into h_out, WITHOUT running the
   vocab-sized head. This is the (h, target) cache the head-only QAT smoke uses. */
cce_result cce_supra_hidden_last(cce_supra_decomposed* m, const int* token_ids,
                                 int n_tokens, float* h_out, int cap) {
    if (!m || !token_ids || n_tokens <= 0 || !h_out) return CCE_ERR_INVALID_ARG;
    int T = n_tokens, D = m->n_embd;
    if (cap < D || T > m->block_size) return CCE_ERR_INVALID_ARG;

    cce_tensor x = {0};
    int xsh[2] = {T, D};
    if (cce_tensor_alloc(&x, xsh, 2) != CCE_OK) return CCE_ERR_OOM;
    for (int t = 0; t < T; t++) {
        supra_embed_row(m, token_ids[t], x.data + (size_t)t * D);
        for (int d = 0; d < D; d++)
            x.data[(size_t)t * D + d] += m->pos_emb.data[(size_t)t * D + d];
    }
    cce_result rc = CCE_OK;
    for (int l = 0; l < m->n_layer; l++) {
        rc = supra_block_forward(m, l, &x);
        if (rc != CCE_OK) { cce_tensor_free(&x); return rc; }
    }
    cce_tensor lnf = {0};
    cce_tensor_alloc(&lnf, xsh, 2);
    cce_tensor_layer_norm(&x, &m->ln_f_w, &m->ln_f_b, 1e-5f, &lnf);
    memcpy(h_out, lnf.data + (size_t)(T - 1) * D, (size_t)D * sizeof(float));
    cce_tensor_free(&lnf);
    cce_tensor_free(&x);
    return CCE_OK;
}

/* Like cce_supra_hidden_last but copies the hidden vector at EVERY position
   (the full [n_tokens, n_embd] post-ln_f matrix into h_out, row-major). One
   forward yields n_tokens-1 next-token (hidden, target) pairs -- the cheap way
   to cache thousands of pairs from a real corpus. cap >= n_tokens*n_embd. */
cce_result cce_supra_hidden_all(cce_supra_decomposed* m, const int* token_ids,
                                int n_tokens, float* h_out, int cap) {
    if (!m || !token_ids || n_tokens <= 0 || !h_out) return CCE_ERR_INVALID_ARG;
    int T = n_tokens, D = m->n_embd;
    if (T > m->block_size || cap < T * D) return CCE_ERR_INVALID_ARG;

    cce_tensor x = {0};
    int xsh[2] = {T, D};
    if (cce_tensor_alloc(&x, xsh, 2) != CCE_OK) return CCE_ERR_OOM;
    for (int t = 0; t < T; t++) {
        supra_embed_row(m, token_ids[t], x.data + (size_t)t * D);
        for (int d = 0; d < D; d++)
            x.data[(size_t)t * D + d] += m->pos_emb.data[(size_t)t * D + d];
    }
    cce_result rc = CCE_OK;
    for (int l = 0; l < m->n_layer; l++) {
        rc = supra_block_forward(m, l, &x);
        if (rc != CCE_OK) { cce_tensor_free(&x); return rc; }
    }
    cce_tensor lnf = {0};
    cce_tensor_alloc(&lnf, xsh, 2);
    cce_tensor_layer_norm(&x, &m->ln_f_w, &m->ln_f_b, 1e-5f, &lnf);
    memcpy(h_out, lnf.data, (size_t)T * D * sizeof(float));
    cce_tensor_free(&lnf);
    cce_tensor_free(&x);
    return CCE_OK;
}

/* Borrow the FP head ("gpt.head") weight matrix and bias. Layout is [in,out]
   row-major: logit[o] = bias[o] + sum_i in[i]*weights[i*out_dim + o]. Returns
   CCE_ERR_INVALID_ARG if the head has no FP weights (already int8/trit). */
cce_result cce_supra_head_fp(cce_supra_decomposed* m, const float** weights,
                             const float** bias, int* in_dim, int* out_dim) {
    if (!m || !m->forest) return CCE_ERR_INVALID_ARG;
    cce_cascade* head = get_cascade_by_name(m->forest, "gpt.head");
    if (!head || head->num_blocks < 1) return CCE_ERR_NOT_FOUND;
    cce_block* blk = &head->blocks[head->num_blocks - 1];
    if (!blk->weights.data || blk->weights.ndim != 2) return CCE_ERR_INVALID_ARG;
    if (weights) *weights = blk->weights.data;
    if (bias)    *bias    = blk->bias.data;
    if (in_dim)  *in_dim  = blk->weights.shape[0];
    if (out_dim) *out_dim = blk->weights.shape[1];
    return CCE_OK;
}

/* Top-k + temperature sampling.
   NOTE: the top-k filter must NOT permute `logits`, because the sampler returns
   the array index as the token id. The previous version selection-sorted logits
   in place, which moved the largest logit to index 0 -> every "top" token was
   reported as id 0 ('!'). Here we find the top-k threshold on a copy and only
   mask `logits` below it, preserving token identity. */
static int sample_next_token(float* logits, int vocab_size, float temperature, int top_k, unsigned int* rng_state) {
    if (temperature <= 0) temperature = 1e-6f;
    for (int i = 0; i < vocab_size; i++) logits[i] /= temperature;

    if (top_k > 0 && top_k < vocab_size) {
        float* tmp = (float*)malloc((size_t)vocab_size * sizeof(float));
        if (tmp) {
            memcpy(tmp, logits, (size_t)vocab_size * sizeof(float));
            /* partial selection: move the k largest to the front of the copy */
            for (int i = 0; i < top_k; i++) {
                int mj = i;
                for (int j = i + 1; j < vocab_size; j++) if (tmp[j] > tmp[mj]) mj = j;
                if (mj != i) { float t = tmp[i]; tmp[i] = tmp[mj]; tmp[mj] = t; }
            }
            float thresh = tmp[top_k - 1];
            free(tmp);
            for (int i = 0; i < vocab_size; i++) if (logits[i] < thresh) logits[i] = -1e30f;
        }
    }

    /* softmax over surviving logits, then sample by ORIGINAL index */
    float maxl = -1e30f;
    for (int i=0; i<vocab_size; i++) if (logits[i] > maxl) maxl = logits[i];
    float sum = 0.0f;
    for (int i=0; i<vocab_size; i++) {
        logits[i] = expf(logits[i] - maxl);
        sum += logits[i];
    }
    if (sum <= 0) sum = 1.0f;
    float r = (float)((*rng_state = *rng_state * 1103515245U + 12345U) & 0x7fffffffU) / 2147483648.0f;
    float cum = 0.0f;
    for (int i=0; i<vocab_size; i++) {
        cum += logits[i] / sum;
        if (r <= cum) return i;
    }
    return vocab_size - 1;
}

/* ---- KV cache: turns per-step generation from O(prefix) into O(1) linears +
   O(prefix) cheap attention dot-products, so a length-T generation is ~O(T)
   instead of O(T^2). Only the NEW token is pushed through the projections each
   step; past keys/values are reused from the cache. Numerically equivalent to the
   batched forward (causal attention depends only on positions <= t). ---------- */
typedef struct {
    int    n_embd, block_size, n_head, head_dim, n_layer;
    float* K;   /* [n_layer][block_size][n_embd] */
    float* V;   /* [n_layer][block_size][n_embd] */
    int    len; /* cached positions so far */
} supra_kv_cache;

static supra_kv_cache* kv_alloc(const cce_supra_decomposed* m) {
    supra_kv_cache* c = (supra_kv_cache*)calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->n_embd=m->n_embd; c->block_size=m->block_size; c->n_head=m->n_head;
    c->head_dim=m->n_embd/m->n_head; c->n_layer=m->n_layer; c->len=0;
    size_t n = (size_t)c->n_layer * c->block_size * c->n_embd;
    c->K=(float*)calloc(n,sizeof(float)); c->V=(float*)calloc(n,sizeof(float));
    if (!c->K || !c->V) { free(c->K); free(c->V); free(c); return NULL; }
    return c;
}
static void kv_free(supra_kv_cache* c){ if(c){ free(c->K); free(c->V); free(c); } }

/* One incremental step: token at position `pos`, updates the cache, writes the
   next-token logits. Returns CCE_OK. `scratch_scores` must hold >= block_size floats. */
static cce_result kv_step(cce_supra_decomposed* m, supra_kv_cache* kv,
                          int token, int pos, float* scratch_scores,
                          int* selected_indices,
                          float* logits_out, int cap) {
    int D=m->n_embd, H=m->n_head, hd=kv->head_dim, BS=kv->block_size;
    float scale = 1.0f / sqrtf((float)hd);
    int sh[1] = { D };

    cce_tensor x={0}, ln={0}, attn_in={0};
    cce_tensor_alloc(&x, sh, 1);
    cce_tensor_alloc(&ln, sh, 1);
    cce_tensor_alloc(&attn_in, sh, 1);

    /* token + positional embedding */
    for (int d=0; d<D; d++)
        x.data[d] = m->tok_emb.data[(size_t)token*D + d] + m->pos_emb.data[(size_t)pos*D + d];

    char name[128];
    for (int l=0; l<m->n_layer; l++) {
        /* LN1 -> fused QKV (single vector through the specialist) */
        cce_tensor_layer_norm(&x, &m->ln1_w[l], &m->ln1_b[l], 1e-5f, &ln);
        snprintf(name,sizeof(name),"gpt.block%d.qkv",l);
        cce_tensor qkv={0};
        cce_cascade_forward(get_cascade_by_name(m->forest,name), &ln, &qkv);  /* [3D] */

        /* append this token's k,v to the cache */
        float* Kl = kv->K + ((size_t)l*BS + pos)*D;
        float* Vl = kv->V + ((size_t)l*BS + pos)*D;
        memcpy(Kl, qkv.data + D,     (size_t)D*sizeof(float));
        memcpy(Vl, qkv.data + 2*D,   (size_t)D*sizeof(float));

        /* per-head attention of q_t over cached k/v (positions 0..pos) */
        for (int h=0; h<H; h++) {
            const float* q = qkv.data + h*hd;          /* q of head h */
            for (int j=0; j<=pos; j++) {
                const float* kj = kv->K + ((size_t)l*BS + j)*D + h*hd;
                float s=0; for (int d=0; d<hd; d++) s += q[d]*kj[d];
                scratch_scores[j]=s * scale;
            }

            int selected_count = pos + 1;
            if (m->context_routing_mode == CCE_CONTEXT_ROUTING_SPARSE_ROUTING) {
                cce_result src = cce_specialist_select_kv_tokens(
                    scratch_scores, pos + 1, &m->kv_budget,
                    selected_indices, BS, &selected_count);
                if (src != CCE_OK || selected_count <= 0) {
                    selected_indices[0] = pos;
                    selected_count = 1;
                }
            } else {
                for (int j=0; j<=pos; j++) selected_indices[j]=j;
            }

            float maxv=-1e30f;
            for (int si=0; si<selected_count; si++) {
                int j = selected_indices[si];
                if (scratch_scores[j] > maxv) maxv = scratch_scores[j];
            }
            float sum=0; for (int si=0; si<selected_count; si++){ int j=selected_indices[si]; scratch_scores[j]=expf(scratch_scores[j]-maxv); sum+=scratch_scores[j]; }
            float inv = (sum>0)?1.0f/sum:1.0f;
            float* o = attn_in.data + h*hd;
            for (int d=0; d<hd; d++) o[d]=0.0f;
            for (int si=0; si<selected_count; si++) {
                int j = selected_indices[si];
                const float* vj = kv->V + ((size_t)l*BS + j)*D + h*hd;
                float w = scratch_scores[j]*inv;
                for (int d=0; d<hd; d++) o[d] += w*vj[d];
            }
        }
        cce_tensor_free(&qkv);

        /* attention output projection + residual */
        snprintf(name,sizeof(name),"gpt.block%d.attn_proj",l);
        cce_tensor proj={0};
        cce_cascade_forward(get_cascade_by_name(m->forest,name), &attn_in, &proj);
        for (int d=0; d<D; d++) x.data[d] += proj.data[d];   /* after_attn = x + proj */
        cce_tensor_free(&proj);

        /* LN2 -> MLP (up -> gelu -> down) + residual */
        cce_tensor_layer_norm(&x, &m->ln2_w[l], &m->ln2_b[l], 1e-5f, &ln);
        snprintf(name,sizeof(name),"gpt.block%d.mlp_up",l);
        cce_tensor mid={0};
        cce_cascade_forward(get_cascade_by_name(m->forest,name), &ln, &mid);
        cce_tensor_gelu(&mid, &mid);
        snprintf(name,sizeof(name),"gpt.block%d.mlp_down",l);
        cce_tensor mo={0};
        cce_cascade_forward(get_cascade_by_name(m->forest,name), &mid, &mo);
        for (int d=0; d<D; d++) x.data[d] += mo.data[d];     /* x = after_attn + mlp_out */
        cce_tensor_free(&mid); cce_tensor_free(&mo);
    }

    /* final LN + head (logits for this position) */
    cce_tensor_layer_norm(&x, &m->ln_f_w, &m->ln_f_b, 1e-5f, &ln);
    cce_tensor logit={0};
    cce_cascade_forward(get_cascade_by_name(m->forest,"gpt.head"), &ln, &logit);
    int copy = ((int)logit.numel < cap) ? (int)logit.numel : cap;
    memcpy(logits_out, logit.data, (size_t)copy*sizeof(float));
    cce_tensor_free(&logit);

    cce_tensor_free(&x); cce_tensor_free(&ln); cce_tensor_free(&attn_in);
    if (pos+1 > kv->len) kv->len = pos+1;
    return CCE_OK;
}

/* Autoregressive generate with a KV cache (O(T) linears) + top-k/temperature.
   Context is capped at block_size (positional table limit); generation stops there. */
int cce_supra_generate_text(cce_supra_decomposed* m, const int* prompt, int prompt_len,
                            int* out_ids, int max_new, float temperature, int top_k) {
    if (!m || !prompt || !out_ids || max_new <= 0 || prompt_len <= 0) return 0;

    supra_kv_cache* kv = kv_alloc(m);
    if (!kv) return 0;
    float* logits  = (float*)malloc((size_t)m->vocab_size * sizeof(float));
    float* scores  = (float*)malloc((size_t)m->block_size * sizeof(float));
    int* selected_indices = (int*)malloc((size_t)m->block_size * sizeof(int));
    if (!logits || !scores || !selected_indices) { free(logits); free(scores); free(selected_indices); kv_free(kv); return 0; }
    unsigned int rng = 123456789U;

    int pos = 0;
    /* prefill the prompt; `logits` ends holding the next-token distribution */
    for (int i=0; i<prompt_len && pos<m->block_size; i++, pos++)
        kv_step(m, kv, prompt[i], pos, scores, selected_indices, logits, m->vocab_size);

    int produced = 0;
    for (int step=0; step<max_new && pos<m->block_size; step++) {
        int next = sample_next_token(logits, m->vocab_size, temperature, top_k, &rng);
        out_ids[produced++] = next;
        if (next == 50256) break;          /* <|endoftext|> */
        kv_step(m, kv, next, pos, scores, selected_indices, logits, m->vocab_size);
        pos++;
    }

    free(logits); free(scores); free(selected_indices); kv_free(kv);
    return produced;
}

/* Full VQ using 4D weights. First layer can conceptually use patch, here direct small conv for correctness. */
static void naive_conv2d(const float* w, const float* b, int out_c, int in_c, int k,
                         const float* in, int in_h, int in_w, float* out, int stride, int pad) {
    int out_h = (in_h + 2*pad - k)/stride + 1;
    int out_w = (in_w + 2*pad - k)/stride + 1;
    for (int oc = 0; oc < out_c; oc++) {
        for (int oh = 0; oh < out_h; oh++) for (int ow = 0; ow < out_w; ow++) {
            float s = b ? b[oc] : 0;
            for (int ic=0; ic<in_c; ic++) for (int kh=0; kh<k; kh++) for (int kw=0; kw<k; kw++) {
                int ih = oh*stride + kh - pad, iw = ow*stride + kw - pad;
                if (ih>=0 && ih<in_h && iw>=0 && iw<in_w) {
                    int widx = ((oc*in_c + ic)*k + kh)*k + kw;
                    s += in[(ic*in_h + ih)*in_w + iw] * w[widx];
                }
            }
            out[(oc*out_h + oh)*out_w + ow] = s;
        }
    }
}

cce_result cce_supra_vq_encode(cce_supra_decomposed* m, const float* rgb, int h, int w, int* codes) {
    if (!m || h%8 !=0 || w%8 !=0 || m->vq_code_dim <= 0) return CCE_ERR_INVALID_ARG;
    /* heap-allocated feature maps: too large for the default ~1 MB stack */
    float* f1 = (float*)malloc((size_t)32           * (h/2) * (w/2) * sizeof(float));
    float* f2 = (float*)malloc((size_t)64           * (h/4) * (w/4) * sizeof(float));
    float* f3 = (float*)malloc((size_t)m->vq_code_dim * (h/8) * (w/8) * sizeof(float));
    if (!f1 || !f2 || !f3) { free(f1); free(f2); free(f3); return CCE_ERR_OOM; }

    naive_conv2d(m->vq_enc_w[0].data, m->vq_enc_b[0].data,
                 32, 3, 4, rgb, h, w, f1, 2, 1);
    naive_conv2d(m->vq_enc_w[1].data, m->vq_enc_b[1].data,
                 64, 32, 4, f1, h/2, w/2, f2, 2, 1);
    naive_conv2d(m->vq_enc_w[2].data, m->vq_enc_b[2].data,
                 m->vq_code_dim, 64, 4, f2, h/4, w/4, f3, 2, 1);

    int ch = h/8, cw = w/8;
    for (int i=0; i<ch*cw; i++) {
        float best = 1e30f; int bc=0;
        for (int c=0; c<m->vq_codebook_size; c++) {
            float dist=0;  /* was shadowed by the inner loop var 'd' -> always 0 */
            for (int k=0; k<m->vq_code_dim; k++) {
                float dd = f3[i*m->vq_code_dim + k] - m->vq_codebook.data[c * m->vq_code_dim + k];
                dist += dd*dd;
            }
            if (dist < best) { best=dist; bc = c; }
        }
        codes[i] = bc;
    }
    free(f1); free(f2); free(f3);
    return CCE_OK;
}

cce_result cce_supra_vq_decode(cce_supra_decomposed* m, const int* codes, int ch, int cw, float* out_rgb) {
    if (!m || m->vq_code_dim <= 0) return CCE_ERR_INVALID_ARG;
    float* zq = (float*)malloc((size_t)ch * cw * m->vq_code_dim * sizeof(float)); /* heap, not 1 MB stack */
    if (!zq) return CCE_ERR_OOM;
    for (int i=0; i<ch*cw; i++) {
        int c = codes[i];
        memcpy(zq + (size_t)i*m->vq_code_dim,
               m->vq_codebook.data + (size_t)c*m->vq_code_dim, m->vq_code_dim*sizeof(float));
    }
    // simple upsample + apply dec (full convT would be better, here placeholder upscale)
    int oh = ch*8, ow = cw*8;
    for (int i=0; i<oh*ow*3; i++) out_rgb[i] = 0.5f;
    free(zq);
    return CCE_OK;
}

/* ================== Byte-level BPE tokenizer (GPT-2 style) + A2A handle ================== */

#define SUPRA_MAX_SPECIAL 32

typedef struct { char s[3]; int len; } supra_byte_enc;

struct cce_supra_tokenizer {
    /* control-token ids (resolved from added_tokens) */
    int eot_id, text_start_id, text_end_id, image_start_id, image_end_id,
        video_start_id, video_end_id, frame_id;

    /* vocab map: token string (byte-level unicode, UTF-8) -> id (open addressing) */
    char** vtok;
    int*   vid;
    int    vcap;

    /* id -> token string (for decode) */
    char** id_to_token;
    int    id_cap;
    unsigned char* is_special;   /* 1 if id is an added/control token */

    /* merge ranks: key "A\x1fB" -> rank (open addressing) */
    char** mkey;
    int*   mrank;
    int    mcap;

    /* byte <-> unicode tables */
    supra_byte_enc byte_enc[256];    /* byte -> its mapped unicode char (UTF-8) */
    int            cp_to_byte[1024]; /* unicode codepoint -> byte (-1 if none) */

    /* special-token literals for encode-time matching */
    char* special_str[SUPRA_MAX_SPECIAL];
    int   special_id[SUPRA_MAX_SPECIAL];
    int   nspecial;

    int vocab_size;  /* model logits width hint */
};

static char* supra_strdup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- hashing + open-addressing maps ---- */
static unsigned long long supra_fnv1a(const char* s) {
    unsigned long long h = 1469598103934665603ULL;  /* 64-bit (unsigned long is 32-bit on Windows) */
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}
static void supra_vocab_put(cce_supra_tokenizer* t, char* key, int id) { /* takes ownership of key */
    unsigned long long h = supra_fnv1a(key) % (unsigned long long)t->vcap;
    while (t->vtok[h]) {
        if (strcmp(t->vtok[h], key) == 0) { t->vid[h] = id; free(key); return; }
        h = (h + 1) % (unsigned long long)t->vcap;
    }
    t->vtok[h] = key; t->vid[h] = id;
}
static int supra_vocab_get(const cce_supra_tokenizer* t, const char* key) {
    unsigned long long h = supra_fnv1a(key) % (unsigned long long)t->vcap;
    while (t->vtok[h]) {
        if (strcmp(t->vtok[h], key) == 0) return t->vid[h];
        h = (h + 1) % (unsigned long long)t->vcap;
    }
    return -1;
}
static void supra_merges_put(cce_supra_tokenizer* t, char* key, int rank) { /* takes ownership of key */
    unsigned long long h = supra_fnv1a(key) % (unsigned long long)t->mcap;
    while (t->mkey[h]) {
        if (strcmp(t->mkey[h], key) == 0) { t->mrank[h] = rank; free(key); return; }
        h = (h + 1) % (unsigned long long)t->mcap;
    }
    t->mkey[h] = key; t->mrank[h] = rank;
}
static int supra_merges_get(const cce_supra_tokenizer* t, const char* key) {
    unsigned long long h = supra_fnv1a(key) % (unsigned long long)t->mcap;
    while (t->mkey[h]) {
        if (strcmp(t->mkey[h], key) == 0) return t->mrank[h];
        h = (h + 1) % (unsigned long long)t->mcap;
    }
    return -1;
}

/* GPT-2 bytes_to_unicode: every byte maps to a printable unicode codepoint
   (<= 0x143 here -> 1-2 UTF-8 bytes). Builds both directions. */
static void supra_build_byte_tables(cce_supra_tokenizer* t) {
    int in_set[256]; memset(in_set, 0, sizeof(in_set));
    for (int b = 33;  b <= 126; b++) in_set[b] = 1;
    for (int b = 161; b <= 172; b++) in_set[b] = 1;
    for (int b = 174; b <= 255; b++) in_set[b] = 1;
    int cp[256], n = 0;
    for (int b = 0; b < 256; b++) cp[b] = in_set[b] ? b : (256 + n++);
    for (int c = 0; c < 1024; c++) t->cp_to_byte[c] = -1;
    for (int b = 0; b < 256; b++) {
        int c = cp[b];
        supra_byte_enc* e = &t->byte_enc[b];
        if (c < 0x80) { e->s[0] = (char)c; e->len = 1; }
        else { e->s[0] = (char)(0xC0 | (c >> 6)); e->s[1] = (char)(0x80 | (c & 0x3F)); e->len = 2; }
        e->s[e->len] = 0;
        if (c < 1024) t->cp_to_byte[c] = b;
    }
}

/* Read a JSON string starting at s[*i]=='"'; returns malloc'd UTF-8 (caller frees).
   Token strings are stored as raw UTF-8; standard escapes incl. \uXXXX are decoded. */
static char* supra_json_str(const char* s, int* i, int n) {
    if (*i >= n || s[*i] != '"') return NULL;
    (*i)++;
    size_t cap = 16, len = 0;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    while (*i < n) {
        unsigned char c = (unsigned char)s[*i];
        if (c == '"') { (*i)++; out[len] = 0; return out; }
        char tmp[4]; int tl = 0;
        if (c == '\\') {
            (*i)++; if (*i >= n) break;
            char e = s[*i];
            if (e=='"'||e=='\\'||e=='/') tmp[tl++]=e;
            else if (e=='n') tmp[tl++]='\n';
            else if (e=='t') tmp[tl++]='\t';
            else if (e=='r') tmp[tl++]='\r';
            else if (e=='b') tmp[tl++]='\b';
            else if (e=='f') tmp[tl++]='\f';
            else if (e=='u') {
                if (*i + 4 >= n) break;
                int u = 0;
                for (int k = 1; k <= 4; k++) {
                    char hc = s[*i + k]; u <<= 4;
                    if (hc>='0'&&hc<='9') u |= hc-'0';
                    else if (hc>='a'&&hc<='f') u |= hc-'a'+10;
                    else if (hc>='A'&&hc<='F') u |= hc-'A'+10;
                }
                (*i) += 4;
                if (u < 0x80) tmp[tl++] = (char)u;
                else if (u < 0x800) { tmp[tl++]=(char)(0xC0|(u>>6)); tmp[tl++]=(char)(0x80|(u&0x3F)); }
                else { tmp[tl++]=(char)(0xE0|(u>>12)); tmp[tl++]=(char)(0x80|((u>>6)&0x3F)); tmp[tl++]=(char)(0x80|(u&0x3F)); }
            } else tmp[tl++] = e;
            (*i)++;
        } else {
            tmp[tl++] = (char)c;
            (*i)++;
        }
        if (len + (size_t)tl + 1 > cap) {
            while (len + (size_t)tl + 1 > cap) cap *= 2;
            char* nb = (char*)realloc(out, cap);
            if (!nb) { free(out); return NULL; }
            out = nb;
        }
        for (int k = 0; k < tl; k++) out[len++] = tmp[k];
    }
    free(out);
    return NULL;
}

static int supra_special_lookup(const cce_supra_tokenizer* t, const char* lit) {
    for (int k = 0; k < t->nspecial; k++)
        if (t->special_str[k] && strcmp(t->special_str[k], lit) == 0) return t->special_id[k];
    return -1;
}

static void supra_parse_added_tokens(cce_supra_tokenizer* t, const char* s, int n) {
    const char* p = strstr(s, "\"added_tokens\"");
    if (!p) return;
    int i = (int)(p - s);
    while (i < n && s[i] != '[') i++;
    if (i >= n) return;
    i++;
    while (i < n) {
        skip_ws(s, &i, n);
        if (i < n && s[i] == ']') break;
        if (i < n && s[i] == '{') {
            i++;
            int id = -1; char* content = NULL;
            while (i < n && s[i] != '}') {
                skip_ws(s, &i, n);
                if (i < n && s[i] == '"') {
                    char* key = supra_json_str(s, &i, n);
                    skip_ws(s, &i, n);
                    if (i < n && s[i] == ':') i++;
                    skip_ws(s, &i, n);
                    if (key && strcmp(key, "id") == 0) {
                        int v = 0; while (i < n && isdigit((unsigned char)s[i])) v = v*10 + (s[i++]-'0');
                        id = v;
                    } else if (key && strcmp(key, "content") == 0) {
                        content = supra_json_str(s, &i, n);
                    } else {
                        skip_json_value(s, &i, n);
                    }
                    free(key);
                } else i++;
                skip_ws(s, &i, n);
                if (i < n && s[i] == ',') i++;
            }
            if (i < n && s[i] == '}') i++;
            if (id >= 0 && content) {
                if (id < t->id_cap) {
                    free(t->id_to_token[id]);
                    t->id_to_token[id] = supra_strdup(content);
                    t->is_special[id] = 1;
                }
                if (t->nspecial < SUPRA_MAX_SPECIAL) {
                    t->special_str[t->nspecial] = supra_strdup(content);
                    t->special_id[t->nspecial] = id;
                    t->nspecial++;
                }
            }
            free(content);
        } else i++;
        skip_ws(s, &i, n);
        if (i < n && s[i] == ',') i++;
    }
}

static void supra_parse_vocab(cce_supra_tokenizer* t, const char* s, int n) {
    const char* p = strstr(s, "\"vocab\"");
    if (!p) return;
    int i = (int)(p - s) + 7;
    while (i < n && s[i] != '{') i++;
    if (i >= n) return;
    i++;
    while (i < n) {
        skip_ws(s, &i, n);
        if (i < n && s[i] == '}') break;
        if (i >= n || s[i] != '"') { i++; continue; }
        char* key = supra_json_str(s, &i, n);
        skip_ws(s, &i, n);
        if (i < n && s[i] == ':') i++;
        skip_ws(s, &i, n);
        int v = 0, sign = 1;
        if (i < n && s[i] == '-') { sign = -1; i++; }
        while (i < n && isdigit((unsigned char)s[i])) v = v*10 + (s[i++]-'0');
        v *= sign;
        if (key) {
            if (v >= 0 && v < t->id_cap && !t->id_to_token[v]) t->id_to_token[v] = supra_strdup(key);
            supra_vocab_put(t, key, v);  /* takes ownership of key */
        }
        skip_ws(s, &i, n);
        if (i < n && s[i] == ',') i++;
    }
}

static void supra_add_merge(cce_supra_tokenizer* t, const char* a, const char* b, int rank) {
    size_t la = strlen(a), lb = strlen(b);
    char* key = (char*)malloc(la + lb + 2);
    if (!key) return;
    memcpy(key, a, la); key[la] = '\x1f'; memcpy(key + la + 1, b, lb); key[la + 1 + lb] = 0;
    supra_merges_put(t, key, rank);
}

static void supra_parse_merges(cce_supra_tokenizer* t, const char* s, int n) {
    const char* p = strstr(s, "\"merges\"");
    if (!p) return;
    int i = (int)(p - s) + 8;
    while (i < n && s[i] != '[') i++;
    if (i >= n) return;
    i++; /* outer [ */
    int rank = 0;
    while (i < n) {
        skip_ws(s, &i, n);
        if (i < n && s[i] == ']') break;
        if (i < n && s[i] == '[') {              /* ["A","B"] form */
            i++;
            skip_ws(s, &i, n);
            char* a = supra_json_str(s, &i, n);
            skip_ws(s, &i, n); if (i < n && s[i] == ',') i++; skip_ws(s, &i, n);
            char* b = supra_json_str(s, &i, n);
            skip_ws(s, &i, n); if (i < n && s[i] == ']') i++;
            if (a && b) supra_add_merge(t, a, b, rank++);
            free(a); free(b);
        } else if (i < n && s[i] == '"') {       /* "A B" form */
            char* m = supra_json_str(s, &i, n);
            if (m) { char* sp = strchr(m, ' '); if (sp) { *sp = 0; supra_add_merge(t, m, sp + 1, rank++); } free(m); }
        } else i++;
        skip_ws(s, &i, n);
        if (i < n && s[i] == ',') i++;
    }
}

cce_result cce_supra_tokenizer_load(cce_supra_tokenizer** out, const char* path) {
    if (!out || !path) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    FILE* f = fopen(path, "rb");
    if (!f) return CCE_ERR_IO;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return CCE_ERR_IO; }
    char* buf = (char*)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return CCE_ERR_OOM; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return CCE_ERR_IO; }
    buf[sz] = 0; fclose(f);
    int n = (int)sz;

    cce_supra_tokenizer* t = (cce_supra_tokenizer*)calloc(1, sizeof(*t));
    if (!t) { free(buf); return CCE_ERR_OOM; }
    t->eot_id = t->text_start_id = t->text_end_id = t->image_start_id = t->image_end_id =
        t->video_start_id = t->video_end_id = t->frame_id = -1;
    t->vocab_size = 50520;
    supra_build_byte_tables(t);

    t->vcap = 1 << 17; t->vtok = (char**)calloc(t->vcap, sizeof(char*)); t->vid = (int*)calloc(t->vcap, sizeof(int));
    t->mcap = 1 << 17; t->mkey = (char**)calloc(t->mcap, sizeof(char*)); t->mrank = (int*)calloc(t->mcap, sizeof(int));
    t->id_cap = 50520; t->id_to_token = (char**)calloc(t->id_cap, sizeof(char*)); t->is_special = (unsigned char*)calloc(t->id_cap, 1);
    if (!t->vtok || !t->vid || !t->mkey || !t->mrank || !t->id_to_token || !t->is_special) {
        free(buf); cce_supra_tokenizer_free(t); return CCE_ERR_OOM;
    }

    supra_parse_added_tokens(t, buf, n);
    supra_parse_vocab(t, buf, n);
    supra_parse_merges(t, buf, n);

    t->eot_id         = supra_special_lookup(t, "<|endoftext|>");
    t->text_start_id  = supra_special_lookup(t, "<TEXT>");
    t->text_end_id    = supra_special_lookup(t, "</TEXT>");
    t->image_start_id = supra_special_lookup(t, "<IMAGE>");
    t->image_end_id   = supra_special_lookup(t, "</IMAGE>");
    t->video_start_id = supra_special_lookup(t, "<VIDEO>");
    t->video_end_id   = supra_special_lookup(t, "</VIDEO>");
    t->frame_id       = supra_special_lookup(t, "<FRAME>");

    free(buf);
    *out = t;
    return CCE_OK;
}

void cce_supra_tokenizer_free(cce_supra_tokenizer* t) {
    if (!t) return;
    if (t->vtok) { for (int i = 0; i < t->vcap; i++) free(t->vtok[i]); free(t->vtok); }
    free(t->vid);
    if (t->mkey) { for (int i = 0; i < t->mcap; i++) free(t->mkey[i]); free(t->mkey); }
    free(t->mrank);
    if (t->id_to_token) { for (int i = 0; i < t->id_cap; i++) free(t->id_to_token[i]); free(t->id_to_token); }
    free(t->is_special);
    for (int i = 0; i < t->nspecial; i++) free(t->special_str[i]);
    free(t);
}

/* ---- encoding ---- */

static int supra_is_letter(unsigned char c) { return isalpha(c) || c >= 0x80; }

/* BPE-merge one pre-token (raw bytes) into vocab ids, appended at ids[*cnt]. */
static void supra_bpe_word(cce_supra_tokenizer* t, const char* w, int wlen,
                           int* ids, int* cnt, int max) {
    if (wlen <= 0) return;
    char** sym = (char**)malloc(sizeof(char*) * (size_t)wlen);
    if (!sym) return;
    int ns = 0;
    for (int k = 0; k < wlen; k++) {
        supra_byte_enc* e = &t->byte_enc[(unsigned char)w[k]];
        char* p = (char*)malloc((size_t)e->len + 1);
        if (!p) continue;
        memcpy(p, e->s, (size_t)e->len + 1);
        sym[ns++] = p;
    }
    for (;;) {
        int best_rank = INT_MAX, best_i = -1;
        char keybuf[512];
        for (int k = 0; k + 1 < ns; k++) {
            size_t la = strlen(sym[k]), lb = strlen(sym[k+1]);
            if (la + lb + 2 > sizeof(keybuf)) continue;
            memcpy(keybuf, sym[k], la); keybuf[la] = '\x1f';
            memcpy(keybuf + la + 1, sym[k+1], lb); keybuf[la + 1 + lb] = 0;
            int r = supra_merges_get(t, keybuf);
            if (r >= 0 && r < best_rank) { best_rank = r; best_i = k; }
        }
        if (best_i < 0) break;
        size_t la = strlen(sym[best_i]), lb = strlen(sym[best_i+1]);
        char* merged = (char*)malloc(la + lb + 1);
        if (!merged) break;
        memcpy(merged, sym[best_i], la); memcpy(merged + la, sym[best_i+1], lb); merged[la+lb] = 0;
        free(sym[best_i]); free(sym[best_i+1]);
        sym[best_i] = merged;
        for (int k = best_i + 1; k < ns - 1; k++) sym[k] = sym[k+1];
        ns--;
    }
    for (int k = 0; k < ns; k++) {
        int id = supra_vocab_get(t, sym[k]);
        if (id >= 0 && *cnt < max) ids[(*cnt)++] = id;
        free(sym[k]);
    }
    free(sym);
}

/* GPT-2-ish pre-tokenization (approximate; round-trip is exact regardless). */
static void supra_pretok_bpe(cce_supra_tokenizer* t, const char* s, int n,
                             int* ids, int* cnt, int max) {
    int i = 0;
    while (i < n) {
        int start = i;
        unsigned char c = (unsigned char)s[i];
        /* GPT-2 contraction tokens (matched first): 's 't 're 've 'm 'll 'd */
        if (c == '\'') {
            int rem = n - (i + 1); const char* r = s + i + 1; int adv = 0;
            if (rem >= 1 && (r[0]=='s'||r[0]=='t'||r[0]=='m'||r[0]=='d')) adv = 2;
            else if (rem >= 2 && ((r[0]=='r'&&r[1]=='e')||(r[0]=='v'&&r[1]=='e')||(r[0]=='l'&&r[1]=='l'))) adv = 3;
            if (adv) { i += adv; supra_bpe_word(t, s + start, i - start, ids, cnt, max); continue; }
        }
        if (c == ' ') {
            unsigned char nx = (i + 1 < n) ? (unsigned char)s[i+1] : 0;
            if (nx && !isspace(nx)) {
                i++; /* attach the single leading space to the following run */
                if (supra_is_letter(nx))  { while (i < n && supra_is_letter((unsigned char)s[i])) i++; }
                else if (isdigit(nx))     { while (i < n && isdigit((unsigned char)s[i])) i++; }
                else { while (i < n) { unsigned char d=(unsigned char)s[i]; if (isspace(d)||supra_is_letter(d)||isdigit(d)) break; i++; } }
            } else {
                while (i < n && isspace((unsigned char)s[i])) i++;
            }
        } else if (supra_is_letter(c)) {
            while (i < n && supra_is_letter((unsigned char)s[i])) i++;
        } else if (isdigit(c)) {
            while (i < n && isdigit((unsigned char)s[i])) i++;
        } else if (isspace(c)) {
            while (i < n && isspace((unsigned char)s[i])) i++;
        } else {
            while (i < n) { unsigned char d=(unsigned char)s[i]; if (isspace(d)||supra_is_letter(d)||isdigit(d)) break; i++; }
        }
        if (i <= start) i = start + 1;
        supra_bpe_word(t, s + start, i - start, ids, cnt, max);
    }
}

/* Encode a text span, emitting any embedded special-token literals as their ids. */
static void supra_encode_specials(cce_supra_tokenizer* t, const char* text,
                                  int* ids, int* cnt, int max) {
    int n = (int)strlen(text), i = 0, seg = 0;
    while (i < n) {
        int hit_id = -1, hit_len = 0;
        for (int k = 0; k < t->nspecial; k++) {
            const char* lit = t->special_str[k];
            int L = lit ? (int)strlen(lit) : 0;
            if (L > 0 && i + L <= n && memcmp(text + i, lit, (size_t)L) == 0) { hit_id = t->special_id[k]; hit_len = L; break; }
        }
        if (hit_id >= 0) {
            if (i > seg) supra_pretok_bpe(t, text + seg, i - seg, ids, cnt, max);
            if (*cnt < max) ids[(*cnt)++] = hit_id;
            i += hit_len; seg = i;
        } else i++;
    }
    if (n > seg) supra_pretok_bpe(t, text + seg, n - seg, ids, cnt, max);
}

int cce_supra_encode_text(cce_supra_tokenizer* t, const char* text, int* ids, int max) {
    int n = 0;
    if (!t || !text) return 0;
    if (t->text_start_id >= 0 && n < max) ids[n++] = t->text_start_id;
    supra_encode_specials(t, text, ids, &n, max);
    if (t->text_end_id >= 0 && n < max) ids[n++] = t->text_end_id;
    return n;
}

int cce_supra_decode_text(cce_supra_tokenizer* t, const int* ids, int n, char* out, int maxo) {
    int w = 0;
    if (!t || !out || maxo <= 0) { if (out && maxo > 0) out[0] = 0; return 0; }
    for (int k = 0; k < n; k++) {
        int id = ids[k];
        if (id == t->eot_id) break;
        if (id < 0 || id >= t->id_cap || !t->id_to_token[id]) continue;
        if (t->is_special[id]) continue;            /* drop control tokens from output */
        const char* tok = t->id_to_token[id];
        for (int p = 0; tok[p]; ) {
            unsigned char b0 = (unsigned char)tok[p];
            int cp, adv;
            if (b0 < 0x80) { cp = b0; adv = 1; }
            else if ((b0 & 0xE0) == 0xC0 && tok[p+1]) { cp = ((b0 & 0x1F) << 6) | ((unsigned char)tok[p+1] & 0x3F); adv = 2; }
            else if ((b0 & 0xF0) == 0xE0 && tok[p+1] && tok[p+2]) { cp = ((b0 & 0x0F) << 12) | (((unsigned char)tok[p+1] & 0x3F) << 6) | ((unsigned char)tok[p+2] & 0x3F); adv = 3; }
            else { cp = b0; adv = 1; }
            int byte = (cp >= 0 && cp < 1024) ? t->cp_to_byte[cp] : -1;
            if (byte >= 0 && w < maxo - 1) out[w++] = (char)byte;
            p += adv;
        }
    }
    out[w] = 0;
    return w;
}

cce_result cce_supra_a2a_load(cce_supra_a2a** out, const char* dir_or_repo) {
    if (!out) return CCE_ERR_INVALID_ARG;
    *out = NULL;
    cce_supra_a2a* a = (cce_supra_a2a*)calloc(1, sizeof(*a));
    if (!a) return CCE_ERR_OOM;

    char cdir[256];
    cce_supra_cache_dir(dir_or_repo, cdir, sizeof(cdir));

    cce_result r = cce_supra_load_decomposed(&a->model, cdir, NULL);
    if (r != CCE_OK) { free(a); return r; }

    /* tokenizer.json: download once into the same cache dir, then load */
    char tokpath[512];
    if (cce_supra_fetch_file(cdir, "tokenizer.json", NULL, tokpath, sizeof(tokpath)) == CCE_OK)
        cce_supra_tokenizer_load(&a->tokenizer, tokpath);
    *out = a;
    return CCE_OK;
}

void cce_supra_a2a_free(cce_supra_a2a* a) {
    if (!a) return;
    cce_supra_free_decomposed(a->model);
    cce_supra_tokenizer_free(a->tokenizer);
    free(a);
}

int cce_supra_a2a_complete_text(cce_supra_a2a* a, const char* prompt, char* out, int maxo, int maxnew, float temp, int topk) {
    if (out && maxo > 0) out[0] = 0;
    if (!a || !a->model) return 0;
    if (maxnew < 1) maxnew = 1;

    int pids[512]; int np = 0;
    if (a->tokenizer) np = cce_supra_encode_text(a->tokenizer, prompt, pids, 512);
    int* gens = (int*)malloc((size_t)maxnew * sizeof(int));
    if (!gens) return 0;
    int ng = cce_supra_generate_text(a->model, pids, np, gens, maxnew, temp, topk);
    int rc;
    if (a->tokenizer) rc = cce_supra_decode_text(a->tokenizer, gens, ng, out, maxo);
    else rc = snprintf(out, maxo, "[%d tokens generated]", ng);
    free(gens);
    return rc;
}

int cce_supra_a2a_chat_step(cce_supra_a2a* a, const char* user, char* resp, int maxr) {
    /* encode_text already wraps the content in <TEXT>...</TEXT> control ids */
    return cce_supra_a2a_complete_text(a, user, resp, maxr, 80, 0.8f, 40);
}

cce_forest* cce_supra_a2a_get_forest(cce_supra_a2a* a2a) {
    if (!a2a || !a2a->model) return NULL;
    return a2a->model->forest;
}

/* Support direct load of 1.6-bit packed artifact for full CCE stack access (router, perceptual, contracts, composition). */
cce_result cce_supra_a2a_load_packed(cce_supra_a2a** out, const char* packed_path) {
    if (!out || !packed_path) return CCE_ERR_INVALID_ARG;
    *out = NULL;

    cce_supra_a2a* a = (cce_supra_a2a*)calloc(1, sizeof(*a));
    if (!a) return CCE_ERR_OOM;

    cce_result r = cce_supra_load_packed(&a->model, packed_path);
    if (r != CCE_OK) {
        free(a);
        return r;
    }

    /* Tokenizer is still useful for encode/decode if using high-level complete; try cache or skip */
    /* For specialist forest access this is optional. */
    char tokpath[512];
    /* Best effort; many packed scenarios reuse supra_cache or provide alongside. */
    if (cce_supra_fetch_file("supra_cache", "tokenizer.json", NULL, tokpath, sizeof(tokpath)) == CCE_OK) {
        cce_supra_tokenizer_load(&a->tokenizer, tokpath);
    }

    *out = a;
    return CCE_OK;
}

