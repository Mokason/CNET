/* Token-string identity across teachers: dump and translate mining windows.
 *
 * Subcommands:
 *   dump <model.gguf> <window.txt> [-o out.tsv]
 *   translate <src.gguf> <window.txt> <dst.gguf> [-o out.txt]
 *   encode <model.gguf> <text|@file> [-o out] [--no-bos]
 *   mint-context <model> <textfile> <name> [--dir DIR] [--max-ctx N]
 *   verify <model> <ids...>
 */
#include <ctype.h>
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

enum {
    T_U8 = 0, T_I8, T_U16, T_I16, T_U32, T_I32, T_F32, T_BOOL, T_STR, T_ARR,
    T_U64, T_I64, T_F64
};

static const int SCALAR_SIZE[13] = {
    1, 1, 2, 2, 4, 4, 4, 1, -1, -1, 8, 8, 8
};

typedef struct {
    char **tokens;
    int n_tokens;
    char **merges;
    int n_merges;
    int32_t *ttype;
    int n_ttype;
    char tok_model[64];
} Vocab;

static uint32_t ru32(FILE *f) {
    uint32_t v = 0;
    if (fread(&v, 4, 1, f) != 1) return 0;
    return v;
}

static uint64_t ru64(FILE *f) {
    uint64_t v = 0;
    if (fread(&v, 8, 1, f) != 1) return 0;
    return v;
}

static char *rstr(FILE *f) {
    uint64_t n = ru64(f);
    char *s;
    if (n > (1u << 28)) return NULL;
    s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    if (n && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); return NULL; }
    s[n] = 0;
    return s;
}

static int skip_value(FILE *f, uint32_t t) {
    if (t == T_STR) {
        char *s = rstr(f);
        free(s);
        return s ? 0 : -1;
    }
    if (t == T_ARR) {
        uint32_t et = ru32(f);
        uint64_t n = ru64(f), i;
        if (et == T_STR) {
            for (i = 0; i < n; i++) {
                char *s = rstr(f);
                if (!s) return -1;
                free(s);
            }
        } else if (et < 13 && SCALAR_SIZE[et] > 0) {
            if (fseek(f, (long)(n * (uint64_t)SCALAR_SIZE[et]), SEEK_CUR) != 0)
                return -1;
        } else {
            return -1;
        }
        return 0;
    }
    if (t < 13 && SCALAR_SIZE[t] > 0)
        return fseek(f, SCALAR_SIZE[t], SEEK_CUR);
    return -1;
}

static void vocab_free(Vocab *v) {
    int i;
    if (!v) return;
    for (i = 0; i < v->n_tokens; i++) free(v->tokens[i]);
    free(v->tokens);
    for (i = 0; i < v->n_merges; i++) free(v->merges[i]);
    free(v->merges);
    free(v->ttype);
    memset(v, 0, sizeof *v);
}

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int read_gguf_bpe(const char *path, Vocab *v, int want_merges) {
    FILE *f;
    uint32_t version, t;
    uint64_t tensor_count, kv_count, k;
    memset(v, 0, sizeof *v);
    snprintf(v->tok_model, sizeof v->tok_model, "unknown");
    f = fopen(path, "rb");
    if (!f) return -1;
    {
        char magic[4];
        if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "GGUF", 4) != 0) {
            fclose(f); return -1;
        }
    }
    version = ru32(f);
    if (version < 2) { fclose(f); return -1; }
    tensor_count = ru64(f); (void)tensor_count;
    kv_count = ru64(f);
    for (k = 0; k < kv_count; k++) {
        char *key = rstr(f);
        if (!key) { fclose(f); return -1; }
        t = ru32(f);
        if (strcmp(key, "tokenizer.ggml.model") == 0 && t == T_STR) {
            char *s = rstr(f);
            if (!s) { free(key); fclose(f); return -1; }
            snprintf(v->tok_model, sizeof v->tok_model, "%s", s);
            free(s);
        } else if (strcmp(key, "tokenizer.ggml.tokens") == 0 && t == T_ARR) {
            uint32_t et = ru32(f);
            uint64_t n = ru64(f), i;
            if (et != T_STR) { free(key); fclose(f); return -1; }
            v->tokens = (char **)calloc((size_t)n, sizeof(char *));
            v->n_tokens = (int)n;
            for (i = 0; i < n; i++) {
                v->tokens[i] = rstr(f);
                if (!v->tokens[i]) { free(key); fclose(f); return -1; }
            }
        } else if (want_merges && strcmp(key, "tokenizer.ggml.merges") == 0 && t == T_ARR) {
            uint32_t et = ru32(f);
            uint64_t n = ru64(f), i;
            (void)et;
            v->merges = (char **)calloc((size_t)n, sizeof(char *));
            v->n_merges = (int)n;
            for (i = 0; i < n; i++) {
                v->merges[i] = rstr(f);
                if (!v->merges[i]) { free(key); fclose(f); return -1; }
            }
        } else if (want_merges && strcmp(key, "tokenizer.ggml.token_type") == 0 && t == T_ARR) {
            uint32_t et = ru32(f);
            uint64_t n = ru64(f);
            (void)et;
            v->ttype = (int32_t *)malloc((size_t)n * 4);
            v->n_ttype = (int)n;
            if (!v->ttype || fread(v->ttype, 4, (size_t)n, f) != (size_t)n) {
                free(key); fclose(f); return -1;
            }
        } else {
            if (skip_value(f, t) != 0) { free(key); fclose(f); return -1; }
        }
        free(key);
        if (v->tokens && (!want_merges || (v->merges && v->ttype)) &&
            strcmp(v->tok_model, "unknown") != 0 && !want_merges)
            break;
    }
    fclose(f);
    if (!v->tokens) return -1;
    return 0;
}

/* Minimal HF tokenizer.json vocab (model.vocab object). */
static int read_hf_tokens(const char *dir, Vocab *v) {
    char path[1024], *buf = NULL;
    FILE *f;
    long sz;
    const char *p, *vocab_start;
    int max_id = -1;
    memset(v, 0, sizeof *v);
    snprintf(v->tok_model, sizeof v->tok_model, "hf");
    snprintf(path, sizeof path, "%s/tokenizer.json", dir);
    f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
    if (sz <= 0 || sz > 200 * 1024 * 1024) { fclose(f); return -1; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    buf[sz] = 0;
    fclose(f);

    vocab_start = strstr(buf, "\"vocab\"");
    if (!vocab_start) { free(buf); return -1; }
    p = strchr(vocab_start, '{');
    if (!p) { free(buf); return -1; }
    p++;
    /* first pass: max id */
    {
        const char *q = p;
        while (*q && *q != '}') {
            char piece[512];
            int id = 0, o = 0;
            while (*q && *q != '"') q++;
            if (*q != '"') break;
            q++;
            while (*q && *q != '"' && o + 1 < (int)sizeof piece) {
                if (*q == '\\' && q[1]) { piece[o++] = q[1]; q += 2; }
                else piece[o++] = *q++;
            }
            piece[o] = 0;
            if (*q == '"') q++;
            while (*q && *q != ':') q++;
            if (*q == ':') q++;
            while (*q && isspace((unsigned char)*q)) q++;
            id = atoi(q);
            while (*q && *q != ',' && *q != '}') q++;
            if (*q == ',') q++;
            if (id > max_id) max_id = id;
            (void)piece;
        }
    }
    if (max_id < 0) { free(buf); return -1; }
    v->n_tokens = max_id + 1;
    v->tokens = (char **)calloc((size_t)v->n_tokens, sizeof(char *));
    if (!v->tokens) { free(buf); return -1; }
    {
        const char *q = p;
        while (*q && *q != '}') {
            char piece[512];
            int id = 0, o = 0;
            while (*q && *q != '"') q++;
            if (*q != '"') break;
            q++;
            while (*q && *q != '"' && o + 1 < (int)sizeof piece) {
                if (*q == '\\' && q[1]) { piece[o++] = q[1]; q += 2; }
                else piece[o++] = *q++;
            }
            piece[o] = 0;
            if (*q == '"') q++;
            while (*q && *q != ':') q++;
            if (*q == ':') q++;
            while (*q && isspace((unsigned char)*q)) q++;
            id = atoi(q);
            while (*q && *q != ',' && *q != '}') q++;
            if (*q == ',') q++;
            if (id >= 0 && id < v->n_tokens) {
                free(v->tokens[id]);
                v->tokens[id] = (char *)malloc(strlen(piece) + 1);
                if (v->tokens[id]) strcpy(v->tokens[id], piece);
            }
        }
    }
    {
        int i;
        for (i = 0; i < v->n_tokens; i++)
            if (!v->tokens[i]) {
                v->tokens[i] = (char *)malloc(1);
                if (v->tokens[i]) v->tokens[i][0] = 0;
            }
    }
    free(buf);
    return 0;
}

static int read_vocab(const char *path, Vocab *v) {
    if (is_dir(path)) return read_hf_tokens(path, v);
    return read_gguf_bpe(path, v, 0);
}

static void normalize(const char *piece, char *out, size_t out_sz) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)piece;
    while (*p && o + 1 < out_sz) {
        /* UTF-8 ▁ = E2 96 81 */
        if (p[0] == 0xE2 && p[1] == 0x96 && p[2] == 0x81) {
            out[o++] = ' ';
            p += 3;
            continue;
        }
        /* Ġ (C4 A0) */
        if (p[0] == 0xC4 && p[1] == 0xA0) {
            out[o++] = ' ';
            p += 2;
            continue;
        }
        /* Ċ (C4 8A) */
        if (p[0] == 0xC4 && p[1] == 0x8A) {
            out[o++] = '\n';
            p += 2;
            continue;
        }
        out[o++] = (char)*p++;
    }
    out[o] = 0;
}

static int load_window_ids(const char *path, int **out_ids, int *out_n) {
    FILE *f = fopen(path, "r");
    int cap = 256, n = 0;
    int *ids;
    char tok[64];
    if (!f) return -1;
    ids = (int *)malloc((size_t)cap * sizeof(int));
    if (!ids) { fclose(f); return -1; }
    while (fscanf(f, "%63s", tok) == 1) {
        if (n >= cap) {
            cap *= 2;
            ids = (int *)realloc(ids, (size_t)cap * sizeof(int));
            if (!ids) { fclose(f); return -1; }
        }
        ids[n++] = atoi(tok);
    }
    fclose(f);
    *out_ids = ids;
    *out_n = n;
    return 0;
}

static int ensure_parent(const char *path) {
    char tmp[1024];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = 0;
            MKDIR(tmp);
            tmp[i] = c;
        }
    }
    return 0;
}

/* ---- GemmaBpe (core encode/decode) ---- */
typedef struct {
    Vocab v;
    int *piece2id; /* hash-free: linear map via tokens index; use parallel arrays */
    /* rank map: store as parallel a,b string pairs is heavy — use token-id pairs */
    int *rank_a, *rank_b, *rank_r;
    int n_rank;
    int byte_tok[256];
    int bos;
} GemmaBpe;

static int find_piece(const Vocab *v, const char *piece) {
    int i;
    for (i = 0; i < v->n_tokens; i++)
        if (v->tokens[i] && strcmp(v->tokens[i], piece) == 0) return i;
    return -1;
}

static int bpe_init(GemmaBpe *b, const char *model) {
    int i;
    memset(b, 0, sizeof *b);
    for (i = 0; i < 256; i++) b->byte_tok[i] = -1;
    if (read_gguf_bpe(model, &b->v, 1) != 0) return -1;
    b->n_rank = 0;
    b->rank_a = (int *)malloc((size_t)b->v.n_merges * sizeof(int));
    b->rank_b = (int *)malloc((size_t)b->v.n_merges * sizeof(int));
    b->rank_r = (int *)malloc((size_t)b->v.n_merges * sizeof(int));
    if (!b->rank_a) return -1;
    for (i = 0; i < b->v.n_merges; i++) {
        char left[512], right[512];
        const char *m = b->v.merges[i];
        const char *sp = strchr(m, ' ');
        int a, bb;
        if (!sp) continue;
        if ((size_t)(sp - m) >= sizeof left) continue;
        memcpy(left, m, (size_t)(sp - m));
        left[sp - m] = 0;
        snprintf(right, sizeof right, "%s", sp + 1);
        a = find_piece(&b->v, left);
        bb = find_piece(&b->v, right);
        if (a < 0 || bb < 0) continue;
        /* store piece strings via ids — rank lookup uses string concat later */
        b->rank_a[b->n_rank] = a;
        b->rank_b[b->n_rank] = bb;
        b->rank_r[b->n_rank] = i;
        b->n_rank++;
    }
    for (i = 0; i < b->v.n_ttype; i++) {
        const char *p = b->v.tokens[i];
        if (b->v.ttype[i] == 6 && p && strncmp(p, "<0x", 3) == 0) {
            unsigned x = 0;
            if (sscanf(p, "<0x%x>", &x) == 1 && x < 256)
                b->byte_tok[x] = i;
        }
    }
    b->bos = find_piece(&b->v, "<bos>");
    if (b->bos < 0) b->bos = 2;
    return 0;
}

static void bpe_free(GemmaBpe *b) {
    free(b->rank_a); free(b->rank_b); free(b->rank_r);
    vocab_free(&b->v);
}

static int rank_of(GemmaBpe *b, const char *a, const char *bstr) {
    int i;
    for (i = 0; i < b->n_rank; i++) {
        if (strcmp(b->v.tokens[b->rank_a[i]], a) == 0 &&
            strcmp(b->v.tokens[b->rank_b[i]], bstr) == 0)
            return b->rank_r[i];
    }
    return -1;
}

static int bpe_merge(GemmaBpe *b, char ***syms, int *nsym) {
    for (;;) {
        int best = -1, bi = -1, j;
        for (j = 0; j + 1 < *nsym; j++) {
            int r = rank_of(b, (*syms)[j], (*syms)[j + 1]);
            if (r >= 0 && (best < 0 || r < best)) { best = r; bi = j; }
        }
        if (bi < 0) break;
        {
            size_t la = strlen((*syms)[bi]), lb = strlen((*syms)[bi + 1]);
            char *m = (char *)malloc(la + lb + 1);
            int k;
            if (!m) return -1;
            memcpy(m, (*syms)[bi], la);
            memcpy(m + la, (*syms)[bi + 1], lb + 1);
            free((*syms)[bi]);
            free((*syms)[bi + 1]);
            (*syms)[bi] = m;
            for (k = bi + 1; k + 1 < *nsym; k++)
                (*syms)[k] = (*syms)[k + 1];
            (*nsym)--;
        }
    }
    return 0;
}

static void space_to_sp(const char *text, char *out, size_t out_sz) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p && o + 3 < out_sz) {
        if (*p == ' ') {
            out[o++] = (char)0xE2;
            out[o++] = (char)0x96;
            out[o++] = (char)0x81;
            p++;
        } else {
            out[o++] = (char)*p++;
        }
    }
    out[o] = 0;
}

static int bpe_encode(GemmaBpe *b, const char *text, int add_bos, int **out, int *nout) {
    char *sp_text;
    char **syms = NULL;
    int nsym = 0, cap = 0, i;
    int *ids;
    int nid = 0, idcap = 64;
    size_t tlen = strlen(text);

    sp_text = (char *)malloc(tlen * 3 + 4);
    if (!sp_text) return -1;
    space_to_sp(text, sp_text, tlen * 3 + 4);

    /* start as per-char UTF-8 code units (bytes as chars for ASCII/SP) */
    {
        const unsigned char *p = (const unsigned char *)sp_text;
        while (*p) {
            char piece[8];
            int plen = 1;
            if ((p[0] & 0x80) == 0) plen = 1;
            else if ((p[0] & 0xE0) == 0xC0 && p[1]) plen = 2;
            else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) plen = 3;
            else if ((p[0] & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) plen = 4;
            if (nsym >= cap) {
                cap = cap ? cap * 2 : 64;
                syms = (char **)realloc(syms, (size_t)cap * sizeof(char *));
                if (!syms) { free(sp_text); return -1; }
            }
            memcpy(piece, p, (size_t)plen);
            piece[plen] = 0;
            syms[nsym] = (char *)malloc((size_t)plen + 1);
            memcpy(syms[nsym], piece, (size_t)plen + 1);
            nsym++;
            p += plen;
        }
    }
    free(sp_text);
    if (bpe_merge(b, &syms, &nsym) != 0) return -1;

    ids = (int *)malloc((size_t)idcap * sizeof(int));
    if (!ids) return -1;
    if (add_bos) ids[nid++] = b->bos;
    for (i = 0; i < nsym; i++) {
        int tid = find_piece(&b->v, syms[i]);
        if (tid >= 0) {
            if (nid >= idcap) {
                idcap *= 2;
                ids = (int *)realloc(ids, (size_t)idcap * sizeof(int));
            }
            ids[nid++] = tid;
        } else {
            const unsigned char *bp = (const unsigned char *)syms[i];
            while (*bp) {
                int bt = b->byte_tok[*bp];
                if (bt < 0) {
                    fprintf(stderr, "unmappable byte 0x%02x\n", *bp);
                    free(ids);
                    for (i = 0; i < nsym; i++) free(syms[i]);
                    free(syms);
                    return -1;
                }
                if (nid >= idcap) {
                    idcap *= 2;
                    ids = (int *)realloc(ids, (size_t)idcap * sizeof(int));
                }
                ids[nid++] = bt;
                bp++;
            }
        }
        free(syms[i]);
    }
    free(syms);
    *out = ids;
    *nout = nid;
    return 0;
}

static void bpe_decode(GemmaBpe *b, const int *ids, int n, int drop_special,
                       char *out, size_t out_sz) {
    size_t o = 0;
    int i;
    out[0] = 0;
    for (i = 0; i < n; i++) {
        char norm[1024];
        const char *piece;
        if (drop_special && ids[i] == b->bos) continue;
        if (ids[i] < 0 || ids[i] >= b->v.n_tokens) continue;
        piece = b->v.tokens[ids[i]];
        normalize(piece, norm, sizeof norm);
        {
            size_t L = strlen(norm);
            if (o + L + 1 < out_sz) {
                memcpy(out + o, norm, L);
                o += L;
                out[o] = 0;
            }
        }
    }
}

static char *slurp_text(const char *path) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static int cmd_dump(const char *model, const char *window, const char *out_path) {
    Vocab v;
    int *ids = NULL, n = 0, i;
    FILE *out;
    char pathbuf[1024];
    if (read_vocab(model, &v) != 0) {
        fprintf(stderr, "cannot read vocab %s\n", model);
        return 1;
    }
    if (load_window_ids(window, &ids, &n) != 0) {
        vocab_free(&v);
        return 1;
    }
    if (!out_path) {
        snprintf(pathbuf, sizeof pathbuf, "%s.tokens.tsv", window);
        out_path = pathbuf;
    }
    out = fopen(out_path, "w");
    if (!out) { free(ids); vocab_free(&v); return 1; }
    for (i = 0; i < n; i++) {
        int tid = ids[i];
        const char *piece = (tid >= 0 && tid < v.n_tokens) ? v.tokens[tid] : "?";
        char norm[1024];
        normalize(piece, norm, sizeof norm);
        fprintf(out, "%d\t%s\t%s\n", tid, piece, norm);
    }
    fclose(out);
    printf("wrote %s (tokenizer.ggml.model=%s, %d slots)\n",
           out_path, v.tok_model, n);
    free(ids);
    vocab_free(&v);
    return 0;
}

static int cmd_translate(const char *src, const char *window, const char *dst,
                         const char *out_path) {
    Vocab sv, dv;
    int *ids = NULL, n = 0, i, ok = 0, nmiss = 0;
    int *out_ids;
    FILE *out;
    char pathbuf[1024];
    /* surface -> first dst id */
    typedef struct { char *surf; int id; } Surf;
    Surf *map;
    int nmap = 0;

    if (read_vocab(src, &sv) != 0 || read_vocab(dst, &dv) != 0) {
        fprintf(stderr, "cannot read vocab\n");
        return 1;
    }
    if (load_window_ids(window, &ids, &n) != 0) {
        vocab_free(&sv); vocab_free(&dv);
        return 1;
    }
    map = (Surf *)calloc((size_t)dv.n_tokens, sizeof(Surf));
    for (i = 0; i < dv.n_tokens; i++) {
        char norm[1024];
        int j, found = 0;
        normalize(dv.tokens[i], norm, sizeof norm);
        for (j = 0; j < nmap; j++)
            if (strcmp(map[j].surf, norm) == 0) { found = 1; break; }
        if (!found) {
            map[nmap].surf = (char *)malloc(strlen(norm) + 1);
            strcpy(map[nmap].surf, norm);
            map[nmap].id = i;
            nmap++;
        }
    }
    out_ids = (int *)malloc((size_t)n * sizeof(int));
    for (i = 0; i < n; i++) {
        int tid = ids[i], did = -1, j;
        char surface[1024];
        if (tid >= 0 && tid < sv.n_tokens) {
            normalize(sv.tokens[tid], surface, sizeof surface);
            for (j = 0; j < nmap; j++)
                if (strcmp(map[j].surf, surface) == 0) { did = map[j].id; break; }
        }
        out_ids[i] = did;
        if (did < 0) nmiss++;
        else ok++;
    }
    if (!out_path) {
        snprintf(pathbuf, sizeof pathbuf, "%s.xlate.txt", window);
        out_path = pathbuf;
    }
    out = fopen(out_path, "w");
    if (!out) return 1;
    for (i = 0; i < n; i++) fprintf(out, "%d\n", out_ids[i]);
    fclose(out);
    printf("translated %d/%d slots (%s -> %s); untranslatable -> -1\n",
           ok, n, sv.tok_model, dv.tok_model);
    if (nmiss) {
        fprintf(stderr, "untranslatable slots (unit skipped in cross-recert):\n");
        {
            int shown = 0;
            for (i = 0; i < n && shown < 20; i++) {
                if (out_ids[i] < 0) {
                    int tid = ids[i];
                    fprintf(stderr, "  %d\t'%s'\n", tid,
                            (tid >= 0 && tid < sv.n_tokens) ? sv.tokens[tid] : "?");
                    shown++;
                }
            }
            if (nmiss > 20)
                fprintf(stderr, "  ... +%d more\n", nmiss - 20);
        }
    }
    printf("wrote %s\n", out_path);
    for (i = 0; i < nmap; i++) free(map[i].surf);
    free(map); free(out_ids); free(ids);
    vocab_free(&sv); vocab_free(&dv);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s dump|translate|encode|mint-context|verify ...\n",
                argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "dump") == 0) {
        const char *model, *window, *out = NULL;
        int i;
        if (argc < 4) {
            fprintf(stderr, "usage: %s dump <model> <window> [-o out]\n", argv[0]);
            return 2;
        }
        model = argv[2]; window = argv[3];
        for (i = 4; i < argc; i++)
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--out") == 0) && i + 1 < argc)
                out = argv[++i];
        return cmd_dump(model, window, out);
    }
    if (strcmp(argv[1], "translate") == 0) {
        const char *src, *window, *dst, *out = NULL;
        int i;
        if (argc < 5) {
            fprintf(stderr, "usage: %s translate <src> <window> <dst> [-o out]\n", argv[0]);
            return 2;
        }
        src = argv[2]; window = argv[3]; dst = argv[4];
        for (i = 5; i < argc; i++)
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--out") == 0) && i + 1 < argc)
                out = argv[++i];
        return cmd_translate(src, window, dst, out);
    }
    if (strcmp(argv[1], "encode") == 0) {
        GemmaBpe b;
        const char *model, *textarg, *out = NULL;
        int no_bos = 0, i, *ids = NULL, n = 0;
        char *text = NULL;
        int freeme = 0;
        if (argc < 4) {
            fprintf(stderr, "usage: %s encode <model> <text|@file> [-o out] [--no-bos]\n",
                    argv[0]);
            return 2;
        }
        model = argv[2]; textarg = argv[3];
        for (i = 4; i < argc; i++) {
            if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--out") == 0) && i + 1 < argc)
                out = argv[++i];
            else if (strcmp(argv[i], "--no-bos") == 0) no_bos = 1;
        }
        if (textarg[0] == '@') {
            text = slurp_text(textarg + 1);
            freeme = 1;
            if (!text) return 1;
        } else text = (char *)textarg;
        if (bpe_init(&b, model) != 0) { if (freeme) free(text); return 1; }
        if (bpe_encode(&b, text, !no_bos, &ids, &n) != 0) {
            bpe_free(&b); if (freeme) free(text); return 1;
        }
        if (out) {
            FILE *f = fopen(out, "w");
            if (!f) return 1;
            for (i = 0; i < n; i++) fprintf(f, "%d\n", ids[i]);
            fclose(f);
            printf("wrote %s (%d ids)\n", out, n);
        } else {
            for (i = 0; i < n; i++) printf("%d\n", ids[i]);
        }
        free(ids); bpe_free(&b); if (freeme) free(text);
        return 0;
    }
    if (strcmp(argv[1], "mint-context") == 0) {
        GemmaBpe b;
        const char *model, *textfile, *name, *dir = "config/lane_contexts";
        int max_ctx = 511, i, *ids = NULL, n = 0, *ids2 = NULL, n2 = 0;
        char *text, outpath[1024], decoded[8192];
        FILE *f;
        if (argc < 5) {
            fprintf(stderr, "usage: %s mint-context <model> <textfile> <name> [--dir D] [--max-ctx N]\n",
                    argv[0]);
            return 2;
        }
        model = argv[2]; textfile = argv[3]; name = argv[4];
        for (i = 5; i < argc; i++) {
            if (strcmp(argv[i], "--dir") == 0 && i + 1 < argc) dir = argv[++i];
            else if (strcmp(argv[i], "--max-ctx") == 0 && i + 1 < argc) max_ctx = atoi(argv[++i]);
        }
        text = slurp_text(textfile);
        if (!text) return 1;
        if (bpe_init(&b, model) != 0) { free(text); return 1; }
        if (bpe_encode(&b, text, 1, &ids, &n) != 0) { bpe_free(&b); free(text); return 1; }
        if (n >= max_ctx) {
            fprintf(stderr, "refused: %d tokens >= max_ctx %d (shorten the prose)\n", n, max_ctx);
            free(ids); bpe_free(&b); free(text); return 1;
        }
        bpe_decode(&b, ids, n, 1, decoded, sizeof decoded);
        if (bpe_encode(&b, decoded, 1, &ids2, &n2) != 0 || n2 != n ||
            memcmp(ids, ids2, (size_t)n * sizeof(int)) != 0) {
            fprintf(stderr, "refused: context is not round-trip stable\n");
            free(ids); free(ids2); bpe_free(&b); free(text); return 1;
        }
        free(ids2);
        snprintf(outpath, sizeof outpath, "%s/%s.ids", dir, name);
        ensure_parent(outpath);
        f = fopen(outpath, "w");
        if (!f) return 1;
        for (i = 0; i < n; i++) fprintf(f, "%d\n", ids[i]);
        fclose(f);
        printf("wrote %s (%d tokens): '%.70s'...\n", outpath, n, decoded);
        free(ids); bpe_free(&b); free(text);
        return 0;
    }
    if (strcmp(argv[1], "verify") == 0) {
        GemmaBpe b;
        int allok = 1, ai;
        if (argc < 4) {
            fprintf(stderr, "usage: %s verify <model> <ids...>\n", argv[0]);
            return 2;
        }
        if (bpe_init(&b, argv[2]) != 0) return 1;
        for (ai = 3; ai < argc; ai++) {
            int *gold = NULL, ng = 0, *mine = NULL, nm = 0, ok;
            char decoded[65536];
            if (load_window_ids(argv[ai], &gold, &ng) != 0) { allok = 0; continue; }
            bpe_decode(&b, gold, ng, 1, decoded, sizeof decoded);
            if (bpe_encode(&b, decoded, (ng > 0 && gold[0] == b.bos), &mine, &nm) != 0)
                ok = 0;
            else
                ok = (nm == ng && memcmp(gold, mine, (size_t)ng * sizeof(int)) == 0);
            allok &= ok;
            printf("%s %s (%d tokens)%s\n", ok ? "PASS" : "FAIL", argv[ai], ng,
                   ok ? "" : " MISMATCH");
            free(gold); free(mine);
        }
        printf("ENCODER_VERIFY_%s\n", allok ? "PASS" : "FAIL");
        bpe_free(&b);
        return allok ? 0 : 1;
    }
    fprintf(stderr, "unknown subcommand %s\n", argv[1]);
    return 2;
}
