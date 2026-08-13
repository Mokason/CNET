/* Generate a window-screen candidate pool from a model's OWN vocabulary.
 *
 * Usage: gen_window_candidates <model.gguf> <count> -o <out.txt> [--dict PATH]
 */
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- compact SHA-256 ---- */
typedef struct {
    uint32_t h[8];
    uint64_t bits;
    unsigned char block[64];
    size_t used;
} Sha256;

static const uint32_t SHA_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,
    0x923f82a4u,0xab1c5ed5u,0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
    0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,0xe49b69c1u,0xefbe4786u,
    0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,
    0x06ca6351u,0x14292967u,0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
    0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,0xa2bfe8a1u,0xa81a664bu,
    0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,
    0x5b9cca4fu,0x682e6ff3u,0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
    0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32u - n)); }

static void sha_transform(Sha256 *c, const unsigned char b[64]) {
    uint32_t w[64], a, d, e, f, g, h, i0, j, t1, t2;
    for (j = 0; j < 16; ++j)
        w[j] = ((uint32_t)b[j*4] << 24) | ((uint32_t)b[j*4+1] << 16) |
               ((uint32_t)b[j*4+2] << 8) | (uint32_t)b[j*4+3];
    for (; j < 64; ++j) {
        uint32_t s0 = rotr(w[j-15], 7) ^ rotr(w[j-15], 18) ^ (w[j-15] >> 3);
        uint32_t s1 = rotr(w[j-2], 17) ^ rotr(w[j-2], 19) ^ (w[j-2] >> 10);
        w[j] = w[j-16] + s0 + w[j-7] + s1;
    }
    a=c->h[0]; i0=c->h[1]; d=c->h[2]; e=c->h[3];
    f=c->h[4]; g=c->h[5]; h=c->h[6]; j=c->h[7];
    for (uint32_t r = 0; r < 64; ++r) {
        uint32_t s1 = rotr(f,6) ^ rotr(f,11) ^ rotr(f,25);
        uint32_t ch = (f & g) ^ ((~f) & h);
        uint32_t s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t maj = (a & i0) ^ (a & d) ^ (i0 & d);
        t1 = j + s1 + ch + SHA_K[r] + w[r];
        t2 = s0 + maj;
        j=h; h=g; g=f; f=e+t1; e=d; d=i0; i0=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=i0; c->h[2]+=d; c->h[3]+=e;
    c->h[4]+=f; c->h[5]+=g; c->h[6]+=h; c->h[7]+=j;
}

static void sha_init(Sha256 *c) {
    static const uint32_t init[8] = {
        0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
        0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u
    };
    memcpy(c->h, init, sizeof init);
    c->bits = 0; c->used = 0;
}

static void sha_update(Sha256 *c, const unsigned char *p, size_t n) {
    c->bits += (uint64_t)n * 8u;
    while (n > 0) {
        size_t take = 64u - c->used;
        if (take > n) take = n;
        memcpy(c->block + c->used, p, take);
        c->used += take; p += take; n -= take;
        if (c->used == 64u) { sha_transform(c, c->block); c->used = 0; }
    }
}

static void sha_final(Sha256 *c, unsigned char out[32]) {
    uint64_t bits = c->bits;
    unsigned i;
    c->block[c->used++] = 0x80u;
    if (c->used > 56u) {
        memset(c->block + c->used, 0, 64u - c->used);
        sha_transform(c, c->block);
        c->used = 0;
    }
    memset(c->block + c->used, 0, 56u - c->used);
    for (i = 0; i < 8; ++i)
        c->block[63u-i] = (unsigned char)(bits >> (i*8u));
    sha_transform(c, c->block);
    for (i = 0; i < 8; ++i) {
        out[i*4]   = (unsigned char)(c->h[i] >> 24);
        out[i*4+1] = (unsigned char)(c->h[i] >> 16);
        out[i*4+2] = (unsigned char)(c->h[i] >> 8);
        out[i*4+3] = (unsigned char)c->h[i];
    }
}

static void sha_file_hex(const char *path, char hex[65]) {
    static const char hd[] = "0123456789abcdef";
    unsigned char buf[65536], dig[32];
    Sha256 c;
    FILE *f = fopen(path, "rb");
    size_t n;
    int i;
    sha_init(&c);
    if (f) {
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) sha_update(&c, buf, n);
        fclose(f);
    }
    sha_final(&c, dig);
    for (i = 0; i < 32; i++) {
        hex[i*2] = hd[dig[i] >> 4];
        hex[i*2+1] = hd[dig[i] & 15];
    }
    hex[64] = 0;
}

/* ---- GGUF tokens ---- */
enum { T_STR = 8, T_ARR = 9 };
static const int SCALAR_SIZE[13] = {
    1,1,2,2,4,4,4,1,-1,-1,8,8,8
};

static uint32_t ru32(FILE *f) { uint32_t v=0; fread(&v,4,1,f); return v; }
static uint64_t ru64(FILE *f) { uint64_t v=0; fread(&v,8,1,f); return v; }
static char *rstr(FILE *f) {
    uint64_t n = ru64(f);
    char *s = (char *)malloc((size_t)n + 1);
    if (!s) return NULL;
    if (n && fread(s, 1, (size_t)n, f) != (size_t)n) { free(s); return NULL; }
    s[n] = 0;
    return s;
}

static int skip_value(FILE *f, uint32_t t) {
    if (t == T_STR) { char *s = rstr(f); free(s); return s ? 0 : -1; }
    if (t == T_ARR) {
        uint32_t et = ru32(f);
        uint64_t n = ru64(f), i;
        if (et == T_STR) {
            for (i = 0; i < n; i++) { char *s = rstr(f); if (!s) return -1; free(s); }
        } else if (et < 13 && SCALAR_SIZE[et] > 0) {
            fseek(f, (long)(n * (uint64_t)SCALAR_SIZE[et]), SEEK_CUR);
        } else return -1;
        return 0;
    }
    if (t < 13 && SCALAR_SIZE[t] > 0) return fseek(f, SCALAR_SIZE[t], SEEK_CUR);
    return -1;
}

static int read_gguf_tokens(const char *path, char ***out_tok, int *out_n) {
    FILE *f = fopen(path, "rb");
    char magic[4];
    uint64_t kv, k;
    char **tokens = NULL;
    int ntok = 0;
    if (!f) return -1;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "GGUF", 4) != 0) {
        fclose(f); return -1;
    }
    if (ru32(f) < 2) { fclose(f); return -1; }
    (void)ru64(f);
    kv = ru64(f);
    for (k = 0; k < kv; k++) {
        char *key = rstr(f);
        uint32_t t = ru32(f);
        if (!key) { fclose(f); return -1; }
        if (strcmp(key, "tokenizer.ggml.tokens") == 0 && t == T_ARR) {
            uint32_t et = ru32(f);
            uint64_t n = ru64(f), i;
            (void)et;
            tokens = (char **)calloc((size_t)n, sizeof(char *));
            ntok = (int)n;
            for (i = 0; i < n; i++) {
                tokens[i] = rstr(f);
                if (!tokens[i]) { free(key); fclose(f); return -1; }
            }
            free(key);
            break;
        }
        if (skip_value(f, t) != 0) { free(key); fclose(f); return -1; }
        free(key);
    }
    fclose(f);
    if (!tokens) return -1;
    *out_tok = tokens;
    *out_n = ntok;
    return 0;
}

static void normalize(const char *piece, char *out, size_t out_sz) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)piece;
    while (*p && o + 1 < out_sz) {
        if (p[0] == 0xE2 && p[1] == 0x96 && p[2] == 0x81) {
            out[o++] = ' '; p += 3; continue;
        }
        if (p[0] == 0xC4 && p[1] == 0xA0) { out[o++] = ' '; p += 2; continue; }
        if (p[0] == 0xC4 && p[1] == 0x8A) { out[o++] = '\n'; p += 2; continue; }
        out[o++] = (char)*p++;
    }
    out[o] = 0;
}

/* surface match: leading space + 2..14 lowercase ascii letters */
static int word_surface(const char *s, char *word, size_t word_sz) {
    size_t i, n;
    if (s[0] != ' ') return 0;
    n = strlen(s + 1);
    if (n < 2 || n > 14) return 0;
    for (i = 0; i < n; i++)
        if (s[1 + i] < 'a' || s[1 + i] > 'z') return 0;
    if (n + 1 > word_sz) return 0;
    memcpy(word, s + 1, n + 1);
    return 1;
}

/* Simple open-addressing set of lowercase words from dictionary. */
#define DICT_CAP (1 << 20)
static char **dict_slots;
static int dict_count;

static uint64_t hash_str(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static int dict_has(const char *w) {
    uint64_t h = hash_str(w);
    size_t i = (size_t)(h & (DICT_CAP - 1));
    while (dict_slots[i]) {
        if (strcmp(dict_slots[i], w) == 0) return 1;
        i = (i + 1) & (DICT_CAP - 1);
    }
    return 0;
}

static int dict_add(const char *w) {
    uint64_t h = hash_str(w);
    size_t i = (size_t)(h & (DICT_CAP - 1));
    while (dict_slots[i]) {
        if (strcmp(dict_slots[i], w) == 0) return 0;
        i = (i + 1) & (DICT_CAP - 1);
    }
    dict_slots[i] = (char *)malloc(strlen(w) + 1);
    if (!dict_slots[i]) return -1;
    strcpy(dict_slots[i], w);
    dict_count++;
    return 1;
}

static int load_dict(const char *path) {
    FILE *f = fopen(path, "r");
    char line[512];
    dict_slots = (char **)calloc(DICT_CAP, sizeof(char *));
    if (!dict_slots || !f) {
        if (f) fclose(f);
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        int ok = 1, i;
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (!n) continue;
        for (i = 0; line[i]; i++) {
            if (!isalpha((unsigned char)line[i]) || !islower((unsigned char)line[i])) {
                ok = 0; break;
            }
        }
        if (ok) dict_add(line);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv) {
    const char *model = NULL, *out = NULL, *dict = "/usr/share/dict/words";
    int count = 0, ai, ntok = 0, tid;
    char **tokens = NULL;
    int *picked_id;
    char **picked_w;
    int npicked = 0;
    char dict_sha[65], out_sha[65];
    FILE *f;
    char words_path[1024];

    for (ai = 1; ai < argc; ai++) {
        if ((strcmp(argv[ai], "-o") == 0 || strcmp(argv[ai], "--out") == 0) && ai + 1 < argc)
            out = argv[++ai];
        else if (strcmp(argv[ai], "--dict") == 0 && ai + 1 < argc)
            dict = argv[++ai];
        else if (!model) model = argv[ai];
        else if (!count) count = atoi(argv[ai]);
        else {
            fprintf(stderr, "usage: %s <model.gguf> <count> -o <out.txt> [--dict PATH]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!model || count <= 0 || !out) {
        fprintf(stderr, "usage: %s <model.gguf> <count> -o <out.txt> [--dict PATH]\n",
                argv[0]);
        return 2;
    }
    {
        struct stat st;
        if (stat(dict, &st) != 0) {
            fprintf(stderr, "dictionary %s missing — pass --dict (refusing to emit "
                            "unchecked BPE fragments)\n", dict);
            return 1;
        }
    }
    if (load_dict(dict) != 0) {
        fprintf(stderr, "failed to load dict %s\n", dict);
        return 1;
    }
    sha_file_hex(dict, dict_sha);
    if (read_gguf_tokens(model, &tokens, &ntok) != 0) {
        fprintf(stderr, "cannot read tokens from %s\n", model);
        return 1;
    }

    picked_id = (int *)malloc((size_t)count * sizeof(int));
    picked_w = (char **)calloc((size_t)count, sizeof(char *));
    {
        /* seen words — linear scan ok for small candidate counts */
        char **seen = (char **)calloc((size_t)count + 8, sizeof(char *));
        int nseen = 0, si;
        for (tid = 0; tid < ntok && npicked < count; tid++) {
            char norm[1024], word[32];
            int dup = 0;
            normalize(tokens[tid], norm, sizeof norm);
            if (!word_surface(norm, word, sizeof word)) continue;
            if (!dict_has(word)) continue;
            for (si = 0; si < nseen; si++)
                if (strcmp(seen[si], word) == 0) { dup = 1; break; }
            if (dup) continue;
            seen[nseen] = (char *)malloc(strlen(word) + 1);
            strcpy(seen[nseen], word);
            nseen++;
            picked_id[npicked] = tid;
            picked_w[npicked] = (char *)malloc(strlen(word) + 1);
            strcpy(picked_w[npicked], word);
            npicked++;
        }
        for (si = 0; si < nseen; si++) free(seen[si]);
        free(seen);
    }
    if (npicked < count) {
        fprintf(stderr, "only %d qualifying word tokens in %s (wanted %d)\n",
                npicked, model, count);
        return 1;
    }

    f = fopen(out, "w");
    if (!f) return 1;
    for (tid = 0; tid < npicked; tid++) fprintf(f, "%d\n", picked_id[tid]);
    fclose(f);
    sha_file_hex(out, out_sha);

    snprintf(words_path, sizeof words_path, "%s.words.txt", out);
    f = fopen(words_path, "w");
    if (!f) return 1;
    fprintf(f, "# provenance: model=%s\n", model);
    fprintf(f, "# dict=%s sha256=%s\n", dict, dict_sha);
    fprintf(f, "# rule: ascending token id (BPE frequency rank), "
               "surface ' [a-z]{2,14}', dictionary-checked, deduped\n");
    for (tid = 0; tid < npicked; tid++)
        fprintf(f, "%d\t%s\n", picked_id[tid], picked_w[tid]);
    fclose(f);

    printf("wrote %d candidates to %s (sha256 %s)\n", npicked, out, out_sha);
    printf("first:");
    {
        int i;
        for (i = 0; i < npicked && i < 6; i++)
            printf("%s %d=%s", i ? "," : "", picked_id[i], picked_w[i]);
    }
    printf("\n");

    for (tid = 0; tid < ntok; tid++) free(tokens[tid]);
    free(tokens);
    for (tid = 0; tid < npicked; tid++) free(picked_w[tid]);
    free(picked_w); free(picked_id);
    /* leak dict_slots intentionally on exit — short-lived tool */
    return 0;
}
