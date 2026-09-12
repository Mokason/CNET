#include "cnet_vsa_lexicon.h"
#include "cnet_vsa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <dirent.h>

#define FNV_OFFSET_BASIS 14695981039346656037ULL
#define FNV_PRIME        1099511628211ULL
#define DIM              CNET_VSA_TOPICAL_DIM
#define SUBWORD_SEED     0x53554257534545ULL   /* n-gram keys live in their own namespace */
#define PHRASE_SEED      0x50485241534B4559ULL /* phrase keys likewise */

static uint64_t fnv1a_bytes(uint64_t h, const void *data, size_t len) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < len; ++i) { h ^= (uint64_t)p[i]; h *= FNV_PRIME; }
    return h;
}

static uint64_t fnv1a_str(const char *s) {
    return fnv1a_bytes(FNV_OFFSET_BASIS, s, strlen(s));
}

static uint64_t xorshift64(uint64_t *st) {
    uint64_t x = *st;
    if (x == 0) x = 88172645463325252ULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *st = x;
    return x;
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* ---- active lexicon --------------------------------------------------------- */

static const CnetVsaLexicon *g_active = NULL;

void cnet_vsa_lexicon_set_active(const CnetVsaLexicon *lex) { g_active = lex; }
const CnetVsaLexicon *cnet_vsa_lexicon_active(void) { return g_active; }
uint32_t cnet_vsa_lexicon_active_tag(void) {
    return g_active ? (uint32_t)(g_active->hdr.digest & 0xffffffffu) : 0u;
}

static CnetVsaLexicon g_default_lex;
static int g_default_loaded = 0;
static char g_default_path[1200];

int cnet_vsa_lexicon_activate_default(const char *dir) {
    if (g_active) return 0;
    char path[1200];
    const char *cands[2]; int n = 0;
    if (dir && *dir) { snprintf(path, sizeof(path), "%s/registry.lex", dir); cands[n++] = path; }
    const char *env = getenv("CNET_VSA_LEXICON");
    if (env && *env) cands[n++] = env;
    for (int i = 0; i < n; ++i) {
        FILE *f = fopen(cands[i], "rb");
        if (!f) continue;
        fclose(f);
        if (g_default_loaded) { cnet_vsa_lexicon_free(&g_default_lex); g_default_loaded = 0; }
        int rc = cnet_vsa_lexicon_load(&g_default_lex, cands[i]);
        if (rc != 0) return rc;   /* present but invalid: never fall through to another table */
        g_default_loaded = 1;
        snprintf(g_default_path, sizeof(g_default_path), "%s", cands[i]);
        g_active = &g_default_lex;
        return 1;
    }
    return 0;
}

const char *cnet_vsa_lexicon_active_path(void) {
    return (g_active && g_active == &g_default_lex) ? g_default_path : NULL;
}

/* ---- keys ------------------------------------------------------------------- */

static uint64_t word_key_and_stem(const char *token, char *stem, size_t cap) {
    snprintf(stem, cap, "%s", token);
    cnet_vsa_text_stem(stem);
    return fnv1a_str(stem);
}

uint64_t cnet_vsa_lexicon_word_key(const char *token) {
    char key[CNET_VSA_TOKEN_LEN];
    return word_key_and_stem(token, key, sizeof(key));
}

uint64_t cnet_vsa_lexicon_key_hash(const char *key_word) { return fnv1a_str(key_word); }

uint64_t cnet_vsa_lexicon_phrase_key(uint64_t first, uint64_t second) {
    uint64_t h = fnv1a_bytes(PHRASE_SEED, &first, sizeof(first));
    h = fnv1a_bytes(h, &second, sizeof(second));
    return h ? h : 1;
}

int cnet_vsa_lexicon_subword_keys(const char *key_word, uint32_t min_n, uint32_t max_n, uint64_t *out, int cap) {
    if (!key_word || !out || cap <= 0) return 0;
    size_t len = strlen(key_word);
    if (len == 0 || len > CNET_VSA_TOKEN_LEN - 1) return 0;
    char buf[CNET_VSA_TOKEN_LEN + 2];
    buf[0] = '<'; memcpy(buf + 1, key_word, len); buf[len + 1] = '>';
    size_t m = len + 2;
    if (min_n < 2) min_n = 2;
    if (max_n > 8) max_n = 8;
    if (max_n < min_n) return 0;
    int n = 0;
    for (uint32_t g = min_n; g <= max_n && g <= m; ++g) {
        for (size_t i = 0; i + g <= m && n < cap; ++i) out[n++] = fnv1a_bytes(SUBWORD_SEED, buf + i, g);
    }
    return n;
}

/* ---- lookup ----------------------------------------------------------------- */

static size_t key_search(const uint64_t *keys, size_t n, uint64_t key) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint64_t k = keys[mid];
        if (k < key) lo = mid + 1;
        else if (k > key) hi = mid;
        else return mid;
    }
    return (size_t)-1;
}

const CnetVsaLexiconEntry *cnet_vsa_lexicon_find_hash(const CnetVsaLexicon *lex, uint64_t key) {
    if (!lex || !lex->entries || lex->hdr.count == 0) return NULL;
    size_t i;
    if (lex->keys) i = key_search(lex->keys, lex->hdr.count, key);
    else {
        size_t lo = 0, hi = lex->hdr.count; i = (size_t)-1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            uint64_t k = lex->entries[mid].key;
            if (k < key) lo = mid + 1;
            else if (k > key) hi = mid;
            else { i = mid; break; }
        }
    }
    return i == (size_t)-1 ? NULL : &lex->entries[i];
}

static int32_t entry_index(const CnetVsaLexicon *lex, uint64_t key) {
    const CnetVsaLexiconEntry *e = cnet_vsa_lexicon_find_hash(lex, key);
    return e ? (int32_t)(e - lex->entries) : -1;
}

const CnetVsaLexiconEntry *cnet_vsa_lexicon_find(const CnetVsaLexicon *lex, const char *key_word) {
    if (!lex || !key_word) return NULL;
    return cnet_vsa_lexicon_find_hash(lex, fnv1a_str(key_word));
}

const CnetVsaLexiconSubword *cnet_vsa_lexicon_find_subword(const CnetVsaLexicon *lex, uint64_t key) {
    if (!lex || !lex->subwords || lex->hdr.subwords == 0) return NULL;
    size_t i;
    if (lex->skeys) i = key_search(lex->skeys, lex->hdr.subwords, key);
    else {
        size_t lo = 0, hi = lex->hdr.subwords; i = (size_t)-1;
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            uint64_t k = lex->subwords[mid].key;
            if (k < key) lo = mid + 1;
            else if (k > key) hi = mid;
            else { i = mid; break; }
        }
    }
    return i == (size_t)-1 ? NULL : &lex->subwords[i];
}

/* +-mag per sign bit into int16 lanes (byte -> eight lanes lookup) */
static int8_t k_lanes[256][8];
static int k_lanes_ready = 0;
static void lanes_init(void) {
    if (k_lanes_ready) return;
    for (int v = 0; v < 256; ++v) for (int k = 0; k < 8; ++k) k_lanes[v][k] = (int8_t)(((v >> k) & 1) ? 1 : -1);
    k_lanes_ready = 1; /* idempotent: a racing init writes identical values */
}
static void bits_accumulate(int16_t *acc, const uint64_t *bits, int mag) {
    lanes_init();
    for (int w = 0; w < CNET_VSA_TOPICAL_WORDS; ++w) {
        uint64_t b = bits[w];
        int16_t *a = acc + (w << 6);
        for (int j = 0; j < 8; ++j) {
            const int8_t *lanes = k_lanes[(b >> (8 * j)) & 0xffu];
            int16_t *aj = a + (j << 3);
            for (int k = 0; k < 8; ++k) aj[k] = (int16_t)(aj[k] + lanes[k] * mag);
        }
    }
}

int cnet_vsa_lexicon_oov_vector(const CnetVsaLexicon *lex, const char *key_word, int16_t *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(int16_t) * DIM);
    if (!key_word) return 0;
    if (lex && lex->subwords && lex->hdr.subwords) {
        uint64_t keys[128];
        int n = cnet_vsa_lexicon_subword_keys(key_word, lex->hdr.subword_min_n, lex->hdr.subword_max_n, keys, 128);
        int found = 0;
        for (int i = 0; i < n; ++i) {
            const CnetVsaLexiconSubword *sw = cnet_vsa_lexicon_find_subword(lex, keys[i]);
            if (!sw) continue;
            bits_accumulate(out, sw->bits, (int)sw->mag);
            found++;
        }
        if (found) {
            /* composed vector at the norm of an identity-only word times subword_weight */
            double nn = 0.0;
            for (int d = 0; d < DIM; ++d) nn += (double)out[d] * out[d];
            nn = sqrt(nn);
            double target = (double)lex->hdr.fallback_mag * sqrt((double)DIM) * (double)lex->hdr.subword_weight;
            double f = nn > 0.0 ? target / nn : 0.0;
            for (int d = 0; d < DIM; ++d) {
                double x = (double)out[d] * f;
                int q = (int)(x >= 0.0 ? x + 0.5 : x - 0.5);
                out[d] = (int16_t)(q > 32767 ? 32767 : (q < -32767 ? -32767 : q));
            }
            return 1;
        }
    }
    uint64_t bits[CNET_VSA_TOPICAL_WORDS];
    cnet_vsa_text_token_bits(key_word, bits);
    bits_accumulate(out, bits, lex ? (int)lex->hdr.fallback_mag : 1);
    return 0;
}

/* ---- persistence ------------------------------------------------------------ */

static uint64_t lexicon_digest(const CnetVsaLexiconHeader *hdr, const CnetVsaLexiconEntry *entries,
                               const CnetVsaLexiconSubword *subwords) {
    CnetVsaLexiconHeader h = *hdr;
    h.digest = 0;
    uint64_t d = fnv1a_bytes(FNV_OFFSET_BASIS, &h, sizeof(h));
    for (uint32_t i = 0; i < hdr->count; ++i) d = fnv1a_bytes(d, &entries[i], sizeof(entries[i]));
    for (uint32_t i = 0; i < hdr->subwords; ++i) d = fnv1a_bytes(d, &subwords[i], sizeof(subwords[i]));
    return d;
}

static int lexicon_write(const CnetVsaLexiconHeader *hdr, const CnetVsaLexiconEntry *entries,
                         const CnetVsaLexiconSubword *subwords, const char *out_path) {
    int rc = 0;
    char tmp[1200];
    snprintf(tmp, sizeof(tmp), "%s.tmp", out_path);
    FILE *fo = fopen(tmp, "wb");
    if (!fo) return -6;
    if (fwrite(hdr, sizeof(*hdr), 1, fo) != 1) rc = -6;
    if (rc == 0 && hdr->count && fwrite(entries, sizeof(CnetVsaLexiconEntry), hdr->count, fo) != hdr->count) rc = -6;
    if (rc == 0 && hdr->subwords && fwrite(subwords, sizeof(CnetVsaLexiconSubword), hdr->subwords, fo) != hdr->subwords) rc = -6;
    fclose(fo);
    if (rc == 0 && rename(tmp, out_path) != 0) rc = -6;
    if (rc != 0) remove(tmp);
    return rc;
}

int cnet_vsa_lexicon_load(CnetVsaLexicon *lex, const char *path) {
    if (!lex || !path || !*path) return -1;
    memset(lex, 0, sizeof(*lex));
    FILE *fp = fopen(path, "rb");
    if (!fp) return -2;
    if (fread(&lex->hdr, sizeof(lex->hdr), 1, fp) != 1) { fclose(fp); return -3; }
    if (lex->hdr.magic != CNET_VSA_LEX_MAGIC || lex->hdr.version != CNET_VSA_LEX_VERSION ||
        lex->hdr.dim != DIM || lex->hdr.count == 0 || lex->hdr.count > (1u << 22) ||
        lex->hdr.subwords > (1u << 22) || lex->hdr.phrases > lex->hdr.count) {
        fclose(fp); return -4;
    }
    lex->entries = (CnetVsaLexiconEntry *)malloc(sizeof(CnetVsaLexiconEntry) * lex->hdr.count);
    if (!lex->entries) { fclose(fp); return -5; }
    if (fread(lex->entries, sizeof(CnetVsaLexiconEntry), lex->hdr.count, fp) != lex->hdr.count) {
        cnet_vsa_lexicon_free(lex); fclose(fp); return -3;
    }
    if (lex->hdr.subwords) {
        lex->subwords = (CnetVsaLexiconSubword *)malloc(sizeof(CnetVsaLexiconSubword) * lex->hdr.subwords);
        if (!lex->subwords) { cnet_vsa_lexicon_free(lex); fclose(fp); return -5; }
        if (fread(lex->subwords, sizeof(CnetVsaLexiconSubword), lex->hdr.subwords, fp) != lex->hdr.subwords) {
            cnet_vsa_lexicon_free(lex); fclose(fp); return -3;
        }
    }
    int extra = fgetc(fp);
    fclose(fp);
    if (extra != EOF) { cnet_vsa_lexicon_free(lex); return -3; }
    if (lexicon_digest(&lex->hdr, lex->entries, lex->subwords) != lex->hdr.digest) {
        cnet_vsa_lexicon_free(lex); return -6; /* tampered or corrupt */
    }
    for (uint32_t i = 1; i < lex->hdr.count; ++i) {
        if (lex->entries[i - 1].key >= lex->entries[i].key) { cnet_vsa_lexicon_free(lex); return -4; }
    }
    for (uint32_t i = 1; i < lex->hdr.subwords; ++i) {
        if (lex->subwords[i - 1].key >= lex->subwords[i].key) { cnet_vsa_lexicon_free(lex); return -4; }
    }
    if (lex->hdr.subwords && (lex->hdr.subword_min_n < 2 || lex->hdr.subword_max_n > 8 ||
                              lex->hdr.subword_max_n < lex->hdr.subword_min_n)) { cnet_vsa_lexicon_free(lex); return -4; }
    /* packed key mirrors for cache-resident binary search (not persisted) */
    lex->keys = (uint64_t *)malloc(sizeof(uint64_t) * lex->hdr.count);
    if (!lex->keys) { cnet_vsa_lexicon_free(lex); return -5; }
    for (uint32_t i = 0; i < lex->hdr.count; ++i) lex->keys[i] = lex->entries[i].key;
    if (lex->hdr.subwords) {
        lex->skeys = (uint64_t *)malloc(sizeof(uint64_t) * lex->hdr.subwords);
        if (!lex->skeys) { cnet_vsa_lexicon_free(lex); return -5; }
        for (uint32_t i = 0; i < lex->hdr.subwords; ++i) lex->skeys[i] = lex->subwords[i].key;
    }
    return 0;
}

void cnet_vsa_lexicon_free(CnetVsaLexicon *lex) {
    if (!lex) return;
    if (g_active == lex) g_active = NULL;
    free(lex->entries);
    free(lex->subwords);
    free(lex->keys);
    free(lex->skeys);
    lex->entries = NULL;
    lex->subwords = NULL;
    lex->keys = NULL;
    lex->skeys = NULL;
    lex->hdr.count = 0;
    lex->hdr.subwords = 0;
}

/* ---- build ------------------------------------------------------------------ */

void cnet_vsa_lexicon_build_opts_default(CnetVsaLexiconBuildOpts *o) {
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->window = 3;
    o->nnz = 16;
    o->min_count = 2;
    o->max_vocab = 16384;
    o->beta = 0.5f;
    o->reflective = 0;
    o->max_df = 0.30f;
    o->center = 1;
    o->remove_pcs = 0;
    o->distilled = NULL;
    o->distill_alpha = 0.5f;
    o->idf_floor = 0.25f;
    o->idf_power = 1.0f;
    o->max_phrases = 0;
    o->phrase_min_count = 3;
    o->phrase_weight = 1.0f;
    o->max_subwords = 0;
    o->subword_min_n = 3;
    o->subword_max_n = 5;
    o->subword_min_words = 4;
    o->subword_max_words = 0;
    o->subword_weight = 1.0f;
    o->vocab_dump = NULL;
}

void cnet_vsa_lexicon_train_opts_default(CnetVsaLexiconTrainOpts *o) {
    if (!o) return;
    memset(o, 0, sizeof(*o));
    o->epochs = 8;
    o->lr = 0.05f;
    o->margin = 0.10f;
    o->seed = 0x9E3779B97F4A7C15ULL;
    o->sentences_per_corpus = 0;
}

/* open-addressing key -> index map */
typedef struct { uint64_t *keys; int32_t *vals; size_t cap; size_t used; } KeyMap;

static int keymap_init(KeyMap *m, size_t cap_pow2) {
    m->cap = cap_pow2; m->used = 0;
    m->keys = (uint64_t *)calloc(cap_pow2, sizeof(uint64_t));
    m->vals = (int32_t *)malloc(cap_pow2 * sizeof(int32_t));
    if (!m->keys || !m->vals) return -1;
    for (size_t i = 0; i < cap_pow2; ++i) m->vals[i] = -1;
    return 0;
}
static void keymap_free(KeyMap *m) { free(m->keys); free(m->vals); m->keys = NULL; m->vals = NULL; }
static int32_t *keymap_slot(KeyMap *m, uint64_t key, int insert) {
    size_t mask = m->cap - 1;
    size_t i = (size_t)(key * 0x9E3779B97F4A7C15ULL >> 20) & mask;
    for (;;) {
        if (m->vals[i] == -1 || m->keys[i] == key) {
            if (m->vals[i] == -1) {
                if (!insert) return NULL;
                if (m->used * 2 >= m->cap) return NULL; /* caller must size generously */
                m->keys[i] = key; m->used++;
            }
            return &m->vals[i];
        }
        i = (i + 1) & mask;
    }
}
static void keymap_reset(KeyMap *m) {
    for (size_t i = 0; i < m->cap; ++i) m->vals[i] = -1;
    m->used = 0;
}

typedef struct { uint64_t key; uint32_t count; uint32_t df; } WordStat;
typedef struct { uint64_t key; uint64_t a, b; uint32_t count; uint32_t df; } PhraseStat;
typedef struct { uint64_t key; uint32_t words; } SubStat;

static int cmp_stat_count_desc(const void *a, const void *b) {
    const WordStat *x = (const WordStat *)a, *y = (const WordStat *)b;
    if (x->count != y->count) return (x->count < y->count) - (x->count > y->count);
    return (x->key > y->key) - (x->key < y->key);
}
static int cmp_phrase_count_desc(const void *a, const void *b) {
    const PhraseStat *x = (const PhraseStat *)a, *y = (const PhraseStat *)b;
    if (x->count != y->count) return (x->count < y->count) - (x->count > y->count);
    return (x->key > y->key) - (x->key < y->key);
}
static int cmp_sub_words_desc(const void *a, const void *b) {
    const SubStat *x = (const SubStat *)a, *y = (const SubStat *)b;
    if (x->words != y->words) return (x->words < y->words) - (x->words > y->words);
    return (x->key > y->key) - (x->key < y->key);
}
static int cmp_entry_key(const void *a, const void *b) {
    uint64_t x = ((const CnetVsaLexiconEntry *)a)->key, y = ((const CnetVsaLexiconEntry *)b)->key;
    return (x > y) - (x < y);
}
static int cmp_subword_key(const void *a, const void *b) {
    uint64_t x = ((const CnetVsaLexiconSubword *)a)->key, y = ((const CnetVsaLexiconSubword *)b)->key;
    return (x > y) - (x < y);
}

/* sparse ternary index vector of a word: nnz distinct positions with signs */
static void index_vector(uint64_t key, uint32_t nnz, int16_t *pos, int8_t *sign) {
    uint64_t st = key ^ 0xA5A5A5A5A5A5A5A5ULL;
    for (uint32_t k = 0; k < nnz; ++k) {
        int16_t p;
        int dup;
        do {
            p = (int16_t)(xorshift64(&st) % DIM);
            dup = 0;
            for (uint32_t j = 0; j < k; ++j) if (pos[j] == p) { dup = 1; break; }
        } while (dup);
        pos[k] = p;
        sign[k] = (xorshift64(&st) & 1u) ? 1 : -1;
    }
}

/* add one neighbour's index vector, rotated by the signed offset, damped */
static inline void add_context(int32_t *row, int32_t cs, int o, const int16_t *ipos, const int8_t *isgn,
                               const int32_t *damp, uint32_t nnz) {
    int32_t cw = damp[cs];
    if (cw == 0) return;
    const int16_t *cp = ipos + (size_t)cs * nnz;
    const int8_t *cg = isgn + (size_t)cs * nnz;
    int shift = ((o % DIM) + DIM) % DIM;
    for (uint32_t k = 0; k < nnz; ++k) {
        int p = (cp[k] + shift) % DIM;
        row[p] += cg[k] * cw;
    }
}

static void unit_row(float *v) {
    double nn = 0.0;
    for (int d = 0; d < DIM; ++d) nn += (double)v[d] * v[d];
    nn = nn > 0.0 ? sqrt(nn) : 1.0;
    for (int d = 0; d < DIM; ++d) v[d] = (float)(v[d] / nn);
}

static double idf_weight(uint64_t files, uint32_t df, const CnetVsaLexiconBuildOpts *opts) {
    double idf = log((double)(files + 1) / (double)(df + 1)) / log((double)(files + 1));
    if (idf > 1.0) idf = 1.0;
    if (idf < 0.0) idf = 0.0;
    if (opts->idf_power != 1.0f && opts->idf_power > 0.0f) idf = pow(idf, (double)opts->idf_power);
    if (idf < (double)opts->idf_floor) idf = (double)opts->idf_floor;
    return idf;
}

typedef struct {
    uint64_t *stream; uint32_t *doc_of; size_t n_tok;
    KeyMap map, pmap, smap;
    WordStat *stats; PhraseStat *phr; SubStat *subs;
    char (*surf)[CNET_VSA_TOKEN_LEN];
    int16_t *ipos; int8_t *isgn; int32_t *damp; int32_t *acc;
    CnetVsaLexiconEntry *entries; CnetVsaLexiconSubword *subwords;
    float *fvec, *ctxn, *ssum; double *mean;
} BuildMem;

static void build_free(BuildMem *m) {
    free(m->stream); free(m->doc_of); keymap_free(&m->map); keymap_free(&m->pmap); keymap_free(&m->smap);
    free(m->stats); free(m->phr); free(m->subs); free(m->surf); free(m->ipos); free(m->isgn); free(m->damp);
    free(m->acc); free(m->entries); free(m->subwords); free(m->fvec); free(m->ctxn); free(m->ssum); free(m->mean);
}

int cnet_vsa_lexicon_build(const char *corpus_dir, const CnetVsaLexiconBuildOpts *opts_in,
                           const char *out_path, CnetVsaLexiconBuildReport *report) {
    if (!corpus_dir || !out_path) return -1;
    CnetVsaLexiconBuildOpts opts;
    if (opts_in) opts = *opts_in; else cnet_vsa_lexicon_build_opts_default(&opts);
    if (opts.window == 0 || opts.window > 16 || opts.nnz == 0 || opts.nnz > 128 ||
        opts.max_vocab == 0 || opts.max_vocab > (1u << 20) || opts.beta < 0.0f ||
        opts.max_phrases > (1u << 20) || opts.max_subwords > (1u << 20) ||
        (opts.max_subwords && (opts.subword_min_n < 2 || opts.subword_max_n > 8 || opts.subword_max_n < opts.subword_min_n)))
        return -1;
    if (report) memset(report, 0, sizeof(*report));
    BuildMem m; memset(&m, 0, sizeof(m));

    /* pass 1: read every corpus into a key stream (sentence boundaries kept);
     * the stem string of every key is kept once (surface for dumps and n-grams) */
    DIR *d = opendir(corpus_dir);
    if (!d) return -2;
    size_t cap_tok = 1u << 20;
    m.stream = (uint64_t *)malloc(cap_tok * sizeof(uint64_t));   /* 0 = sentence boundary */
    m.doc_of = (uint32_t *)malloc(cap_tok * sizeof(uint32_t));
    size_t cap_surf = 1u << 16, n_surf = 0;
    m.surf = (char (*)[CNET_VSA_TOKEN_LEN])malloc(cap_surf * CNET_VSA_TOKEN_LEN);
    if (!m.stream || !m.doc_of || !m.surf || keymap_init(&m.smap, 1u << 21) != 0) { closedir(d); build_free(&m); return -5; }
    uint64_t files = 0, sentences = 0, corpus_hash = 0;
    struct dirent *de;
    char path[1024], line[2048], stem[CNET_VSA_TOKEN_LEN];
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len < 12 || strcmp(de->d_name + len - 11, "_corpus.txt") != 0) continue;
        snprintf(path, sizeof(path), "%s/%s", corpus_dir, de->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        uint32_t doc = (uint32_t)files++;
        while (fgets(line, sizeof(line), fp)) {
            CnetVsaTokenList tl;
            if (cnet_vsa_text_tokenize(line, &tl) <= 0) continue;
            int any = 0;
            for (size_t i = 0; i < tl.count; ++i) {
                if (cnet_vsa_text_is_stopword(tl.tokens[i].token)) continue;
                uint64_t key = word_key_and_stem(tl.tokens[i].token, stem, sizeof(stem));
                if (key == 0) key = 1;
                if (m.n_tok + 2 >= cap_tok) {
                    cap_tok *= 2;
                    uint64_t *ns = (uint64_t *)realloc(m.stream, cap_tok * sizeof(uint64_t));
                    uint32_t *nd = (uint32_t *)realloc(m.doc_of, cap_tok * sizeof(uint32_t));
                    if (ns) m.stream = ns;
                    if (nd) m.doc_of = nd;
                    if (!ns || !nd) { fclose(fp); closedir(d); build_free(&m); return -5; }
                }
                m.stream[m.n_tok] = key; m.doc_of[m.n_tok] = doc; m.n_tok++;
                corpus_hash += key;
                any = 1;
                int32_t *ss = keymap_slot(&m.smap, key, 1);
                if (ss && *ss < 0) {
                    if (n_surf == cap_surf) {
                        cap_surf *= 2;
                        char (*nsf)[CNET_VSA_TOKEN_LEN] = (char (*)[CNET_VSA_TOKEN_LEN])realloc(m.surf, cap_surf * CNET_VSA_TOKEN_LEN);
                        if (!nsf) { fclose(fp); closedir(d); build_free(&m); return -5; }
                        m.surf = nsf;
                    }
                    snprintf(m.surf[n_surf], CNET_VSA_TOKEN_LEN, "%s", stem);
                    *ss = (int32_t)n_surf++;
                }
            }
            if (any) { m.stream[m.n_tok] = 0; m.doc_of[m.n_tok] = doc; m.n_tok++; sentences++; }
        }
        fclose(fp);
    }
    closedir(d);
    if (m.n_tok < 64 || files == 0) { build_free(&m); return -3; }
    const size_t n_tok = m.n_tok;
    const uint64_t *stream = m.stream;
    const uint32_t *doc_of = m.doc_of;

    /* pass 2: frequencies and document frequencies */
    if (keymap_init(&m.map, 1u << 22) != 0) { build_free(&m); return -5; }
    size_t cap_stat = 1u << 16, n_stat = 0;
    m.stats = (WordStat *)malloc(cap_stat * sizeof(WordStat));
    uint32_t *last_doc = (uint32_t *)malloc(cap_stat * sizeof(uint32_t));
    if (!m.stats || !last_doc) { free(last_doc); build_free(&m); return -5; }
    for (size_t i = 0; i < n_tok; ++i) {
        uint64_t key = stream[i];
        if (key == 0) continue;
        int32_t *slot = keymap_slot(&m.map, key, 1);
        if (!slot) break;
        if (*slot == -1) {
            if (n_stat == cap_stat) {
                cap_stat *= 2;
                WordStat *ns = (WordStat *)realloc(m.stats, cap_stat * sizeof(WordStat));
                uint32_t *nl = (uint32_t *)realloc(last_doc, cap_stat * sizeof(uint32_t));
                if (ns) m.stats = ns;
                if (nl) last_doc = nl;
                if (!ns || !nl) { free(last_doc); build_free(&m); return -5; }
            }
            *slot = (int32_t)n_stat;
            m.stats[n_stat].key = key; m.stats[n_stat].count = 0; m.stats[n_stat].df = 0;
            last_doc[n_stat] = 0xffffffffu;
            n_stat++;
        }
        WordStat *w = &m.stats[*slot];
        w->count++;
        if (last_doc[*slot] != doc_of[i]) { w->df++; last_doc[*slot] = doc_of[i]; }
    }
    free(last_doc);
    uint64_t distinct = n_stat;
    WordStat *stats = m.stats;

    /* vocabulary: min_count, top max_vocab by frequency */
    qsort(stats, n_stat, sizeof(WordStat), cmp_stat_count_desc);
    size_t vocab = 0;
    while (vocab < n_stat && vocab < opts.max_vocab && stats[vocab].count >= opts.min_count) vocab++;
    if (vocab < 8) { build_free(&m); return -3; }
    /* re-map keys to vocab index (-1 for out-of-vocabulary) */
    keymap_reset(&m.map);
    for (size_t v = 0; v < vocab; ++v) {
        int32_t *slot = keymap_slot(&m.map, stats[v].key, 1);
        if (slot) *slot = (int32_t)v;
    }

    /* phrases: adjacent in-vocabulary content-word pairs inside a sentence */
    size_t nphr = 0;
    if (opts.max_phrases) {
        if (keymap_init(&m.pmap, 1u << 22) != 0) { build_free(&m); return -5; }
        size_t cap_phr = 1u << 16, n_phr = 0;
        m.phr = (PhraseStat *)malloc(cap_phr * sizeof(PhraseStat));
        uint32_t *plast = (uint32_t *)malloc(cap_phr * sizeof(uint32_t));
        if (!m.phr || !plast) { free(plast); build_free(&m); return -5; }
        for (size_t i = 0; i + 1 < n_tok; ++i) {
            if (stream[i] == 0 || stream[i + 1] == 0) continue;
            int32_t *sa = keymap_slot(&m.map, stream[i], 0), *sb = keymap_slot(&m.map, stream[i + 1], 0);
            if (!sa || !sb || *sa < 0 || *sb < 0) continue;
            uint64_t pk = cnet_vsa_lexicon_phrase_key(stream[i], stream[i + 1]);
            int32_t *slot = keymap_slot(&m.pmap, pk, 1);
            if (!slot) break; /* table full: the phrases counted so far still stand */
            if (*slot == -1) {
                if (n_phr == cap_phr) {
                    cap_phr *= 2;
                    PhraseStat *np = (PhraseStat *)realloc(m.phr, cap_phr * sizeof(PhraseStat));
                    uint32_t *nl = (uint32_t *)realloc(plast, cap_phr * sizeof(uint32_t));
                    if (np) m.phr = np;
                    if (nl) plast = nl;
                    if (!np || !nl) { free(plast); build_free(&m); return -5; }
                }
                *slot = (int32_t)n_phr;
                m.phr[n_phr].key = pk; m.phr[n_phr].a = stream[i]; m.phr[n_phr].b = stream[i + 1];
                m.phr[n_phr].count = 0; m.phr[n_phr].df = 0; plast[n_phr] = 0xffffffffu;
                n_phr++;
            }
            PhraseStat *p = &m.phr[*slot];
            p->count++;
            if (plast[*slot] != doc_of[i]) { p->df++; plast[*slot] = doc_of[i]; }
        }
        free(plast);
        qsort(m.phr, n_phr, sizeof(PhraseStat), cmp_phrase_count_desc);
        while (nphr < n_phr && nphr < opts.max_phrases && m.phr[nphr].count >= opts.phrase_min_count) nphr++;
        keymap_reset(&m.pmap);
        for (size_t p = 0; p < nphr; ++p) {
            int32_t *slot = keymap_slot(&m.pmap, m.phr[p].key, 1);
            if (slot) *slot = (int32_t)p;
        }
    }
    const size_t rows = vocab + nphr;

    /* per-word sparse index vectors and neighbour damping */
    m.ipos = (int16_t *)malloc(vocab * opts.nnz * sizeof(int16_t));
    m.isgn = (int8_t *)malloc(vocab * opts.nnz * sizeof(int8_t));
    m.damp = (int32_t *)malloc(vocab * sizeof(int32_t));   /* integer weight of a neighbour */
    m.acc = (int32_t *)calloc(rows * DIM, sizeof(int32_t));
    if (!m.ipos || !m.isgn || !m.damp || !m.acc) { build_free(&m); return -5; }
    for (size_t v = 0; v < vocab; ++v) {
        index_vector(stats[v].key, opts.nnz, m.ipos + v * opts.nnz, m.isgn + v * opts.nnz);
        /* frequent neighbours carry less information: 1 / log2(2 + count), scaled
         * to integers; words present in more than max_df of the corpora carry
         * none and are not contexts at all (they would give every word the
         * same direction) */
        double dfrac = (double)stats[v].df / (double)files;
        if (opts.max_df > 0.0f && dfrac > (double)opts.max_df) { m.damp[v] = 0; continue; }
        double w = 1.0 / log2(2.0 + (double)stats[v].count);
        m.damp[v] = (int32_t)(w * 256.0 + 0.5);
        if (m.damp[v] < 1) m.damp[v] = 1;
    }

    /* pass 3: the sparse sweep. For each occurrence, add each window neighbour's
     * index vector rotated by the signed offset (direction-aware), damped.
     * A phrase occurrence (i, i+1) takes the neighbours before i and after i+1. */
    double t0 = now_s();
    uint64_t swept = 0;
    size_t s_start = 0;
    const int W = (int)opts.window;
    while (s_start < n_tok) {
        size_t s_end = s_start;
        while (s_end < n_tok && stream[s_end] != 0) s_end++;
        for (size_t i = s_start; i < s_end; ++i) {
            int32_t *ts = keymap_slot(&m.map, stream[i], 0);
            if (ts && *ts >= 0) {
                int32_t *row = m.acc + (size_t)(*ts) * DIM;
                swept++;
                for (int o = -W; o <= W; ++o) {
                    if (o == 0) continue;
                    long j = (long)i + o;
                    if (j < (long)s_start || j >= (long)s_end) continue;
                    int32_t *cs = keymap_slot(&m.map, stream[j], 0);
                    if (!cs || *cs < 0) continue;
                    add_context(row, *cs, o, m.ipos, m.isgn, m.damp, opts.nnz);
                }
            }
            if (nphr && i + 1 < s_end) {
                int32_t *ps = keymap_slot(&m.pmap, cnet_vsa_lexicon_phrase_key(stream[i], stream[i + 1]), 0);
                if (!ps || *ps < 0) continue;
                int32_t *row = m.acc + (vocab + (size_t)(*ps)) * DIM;
                for (int o = -W; o <= W; ++o) {
                    if (o == 0) continue;
                    long j = o < 0 ? (long)i + o : (long)i + 1 + o;
                    if (j < (long)s_start || j >= (long)s_end) continue;
                    int32_t *cs = keymap_slot(&m.map, stream[j], 0);
                    if (!cs || *cs < 0) continue;
                    add_context(row, *cs, o, m.ipos, m.isgn, m.damp, opts.nnz);
                }
            }
        }
        s_start = s_end + 1;
    }

    /* reflective passes (words only): re-index each word by its neighbours'
     * learned vectors (dense), so words sharing contexts but never
     * co-occurring also meet */
    for (uint32_t r = 0; r < opts.reflective; ++r) {
        float *prev = (float *)malloc(vocab * DIM * sizeof(float));
        int32_t *next = (int32_t *)calloc(vocab * DIM, sizeof(int32_t));
        if (!prev || !next) { free(prev); free(next); break; }
        for (size_t v = 0; v < vocab; ++v) {
            float *pv = prev + v * DIM;
            double nn = 0.0;
            for (int dd = 0; dd < DIM; ++dd) { pv[dd] = (float)m.acc[v * DIM + dd]; nn += (double)pv[dd] * pv[dd]; }
            nn = nn > 0.0 ? sqrt(nn) : 1.0;
            for (int dd = 0; dd < DIM; ++dd) pv[dd] = (float)(pv[dd] / nn * 64.0);
        }
        s_start = 0;
        while (s_start < n_tok) {
            size_t s_end = s_start;
            while (s_end < n_tok && stream[s_end] != 0) s_end++;
            for (size_t i = s_start; i < s_end; ++i) {
                int32_t *ts = keymap_slot(&m.map, stream[i], 0);
                if (!ts || *ts < 0) continue;
                int32_t *row = next + (size_t)(*ts) * DIM;
                for (int o = -W; o <= W; ++o) {
                    if (o == 0) continue;
                    long j = (long)i + o;
                    if (j < (long)s_start || j >= (long)s_end) continue;
                    int32_t *cs = keymap_slot(&m.map, stream[j], 0);
                    if (!cs || *cs < 0) continue;
                    const float *pv = prev + (size_t)(*cs) * DIM;
                    int32_t cw = m.damp[*cs];
                    if (cw == 0) continue;
                    for (int dd = 0; dd < DIM; ++dd) row[dd] += (int32_t)(pv[dd] * (float)cw / 256.0f);
                }
            }
            s_start = s_end + 1;
        }
        memcpy(m.acc, next, vocab * DIM * sizeof(int32_t));
        free(prev); free(next);
    }
    double sweep_s = now_s() - t0;

    /* pass 4: final vectors = normalize(identity + beta * normalize(context)) * idf */
    m.entries = (CnetVsaLexiconEntry *)calloc(rows, sizeof(CnetVsaLexiconEntry));
    m.fvec = (float *)malloc(rows * DIM * sizeof(float));
    m.ctxn = (float *)malloc(rows * DIM * sizeof(float));
    m.mean = (double *)calloc(DIM, sizeof(double));
    if (!m.entries || !m.fvec || !m.ctxn || !m.mean) { build_free(&m); return -5; }
    const double id_mag = 1.0 / sqrt((double)DIM);
    double max_abs = 0.0;
    uint64_t bits[CNET_VSA_TOPICAL_WORDS];

    /* normalised context signatures; then remove their common direction.
     * Even with damping, every word sees the same frequent neighbours, so the
     * raw signatures share one large component that makes unrelated texts look
     * alike (measured: null sd 0.05 -> 0.10). Subtracting the mean signature
     * and renormalising restores quasi-orthogonality between unrelated words
     * while keeping the shared-context similarity between related ones. */
    for (size_t v = 0; v < rows; ++v) {
        const int32_t *row = m.acc + v * DIM;
        float *cv = m.ctxn + v * DIM;
        for (int dd = 0; dd < DIM; ++dd) cv[dd] = (float)row[dd];
        unit_row(cv);
        for (int dd = 0; dd < DIM; ++dd) m.mean[dd] += cv[dd];
    }
    /* distilled transformer context: for keys present in the file (words or
     * phrases), blend the unit-normalised distilled vector into the signature
     * before centring */
    uint32_t n_distilled = 0;
    if (opts.distilled && *opts.distilled) {
        FILE *df = fopen(opts.distilled, "rb");
        if (!df) { build_free(&m); return -7; }
        uint32_t magic = 0, dcount = 0, ddim = 0;
        int ok = fread(&magic, 4, 1, df) == 1 && fread(&dcount, 4, 1, df) == 1 && fread(&ddim, 4, 1, df) == 1 &&
                 magic == CNET_VSA_DISTILL_MAGIC && ddim == DIM && dcount > 0 && dcount < (1u << 22);
        float *dv = ok ? (float *)malloc(sizeof(float) * DIM) : NULL;
        float a = opts.distill_alpha < 0.0f ? 0.0f : (opts.distill_alpha > 1.0f ? 1.0f : opts.distill_alpha);
        for (uint32_t i = 0; ok && dv && i < dcount; ++i) {
            uint64_t key = 0;
            if (fread(&key, 8, 1, df) != 1 || fread(dv, sizeof(float), DIM, df) != DIM) { ok = 0; break; }
            int32_t *slot = keymap_slot(&m.map, key, 0);
            size_t r;
            if (slot && *slot >= 0) r = (size_t)*slot;
            else {
                if (!nphr) continue;
                int32_t *ps = keymap_slot(&m.pmap, key, 0);
                if (!ps || *ps < 0) continue;
                r = vocab + (size_t)*ps;
            }
            float *cv = m.ctxn + r * DIM;
            double dn = 0.0;
            for (int dd = 0; dd < DIM; ++dd) dn += (double)dv[dd] * dv[dd];
            dn = dn > 0.0 ? sqrt(dn) : 1.0;
            for (int dd = 0; dd < DIM; ++dd) cv[dd] = (float)((1.0 - a) * cv[dd] + a * (dv[dd] / dn));
            unit_row(cv);
            n_distilled++;
        }
        free(dv);
        fclose(df);
        if (!ok) { build_free(&m); return -7; }
        /* the mean must reflect the blended signatures */
        memset(m.mean, 0, DIM * sizeof(double));
        for (size_t v = 0; v < rows; ++v) {
            const float *cv = m.ctxn + v * DIM;
            for (int dd = 0; dd < DIM; ++dd) m.mean[dd] += cv[dd];
        }
    }
    if (opts.center) {
        for (int dd = 0; dd < DIM; ++dd) m.mean[dd] /= (double)rows;
        for (size_t v = 0; v < rows; ++v) {
            float *cv = m.ctxn + v * DIM;
            for (int dd = 0; dd < DIM; ++dd) cv[dd] = (float)(cv[dd] - m.mean[dd]);
            unit_row(cv);
        }
    }

    /* all-but-the-top: the centred signatures still concentrate in a few
     * directions (technical-prose vocabulary shared by every topic), which
     * inflates the cosine between unrelated texts. Find the top principal
     * directions by power iteration with deflation and project them out. */
    for (uint32_t pc = 0; pc < opts.remove_pcs; ++pc) {
        double *u = (double *)malloc(DIM * sizeof(double));
        double *w = (double *)malloc(DIM * sizeof(double));
        if (!u || !w) { free(u); free(w); break; }
        uint64_t st = 0x1234567ULL + pc;
        for (int dd = 0; dd < DIM; ++dd) u[dd] = (xorshift64(&st) & 1u) ? 1.0 : -1.0;
        for (int it = 0; it < 24; ++it) {
            memset(w, 0, DIM * sizeof(double));
            for (size_t v = 0; v < rows; ++v) {
                const float *cv = m.ctxn + v * DIM;
                double dot = 0.0;
                for (int dd = 0; dd < DIM; ++dd) dot += cv[dd] * u[dd];
                for (int dd = 0; dd < DIM; ++dd) w[dd] += dot * cv[dd];
            }
            double nn = 0.0;
            for (int dd = 0; dd < DIM; ++dd) nn += w[dd] * w[dd];
            nn = nn > 0.0 ? sqrt(nn) : 1.0;
            for (int dd = 0; dd < DIM; ++dd) u[dd] = w[dd] / nn;
        }
        /* deflate every signature and renormalise */
        for (size_t v = 0; v < rows; ++v) {
            float *cv = m.ctxn + v * DIM;
            double dot = 0.0;
            for (int dd = 0; dd < DIM; ++dd) dot += cv[dd] * u[dd];
            for (int dd = 0; dd < DIM; ++dd) cv[dd] = (float)(cv[dd] - dot * u[dd]);
            unit_row(cv);
        }
        free(u); free(w);
    }

    for (size_t v = 0; v < rows; ++v) {
        float *fv = m.fvec + v * DIM;
        const float *cv = m.ctxn + v * DIM;
        int is_phrase = v >= vocab;
        uint64_t key = is_phrase ? m.phr[v - vocab].key : stats[v].key;
        uint32_t count = is_phrase ? m.phr[v - vocab].count : stats[v].count;
        uint32_t df = is_phrase ? m.phr[v - vocab].df : stats[v].df;
        /* identity: the same signature the hash encoders use for this key;
         * derivable from the stored key alone at runtime, so an unknown word's
         * fallback and a known word's identity come from one generator */
        uint64_t st = key;
        for (int w = 0; w < CNET_VSA_TOPICAL_WORDS; ++w) bits[w] = xorshift64(&st);
        double nn = 0.0;
        for (int dd = 0; dd < DIM; ++dd) {
            double id = ((bits[dd >> 6] >> (dd & 63)) & 1u) ? id_mag : -id_mag;
            double x = id + (double)opts.beta * (double)cv[dd];
            fv[dd] = (float)x;
            nn += x * x;
        }
        nn = nn > 0.0 ? sqrt(nn) : 1.0;
        /* idf weight in [floor, 1]^power: rare terms count fully, ubiquitous ones little */
        double idf = idf_weight(files, df, &opts);
        if (is_phrase) idf *= (double)opts.phrase_weight;
        m.entries[v].key = key;
        m.entries[v].count = count;
        m.entries[v].idf = (float)idf;
        for (int dd = 0; dd < DIM; ++dd) {
            fv[dd] = (float)(fv[dd] / nn * idf);
            double a = fabs((double)fv[dd]);
            if (a > max_abs) max_abs = a;
        }
    }
    float gscale = max_abs > 0.0 ? (float)(127.0 / max_abs) : 1.0f;
    for (size_t v = 0; v < rows; ++v) {
        const float *fv = m.fvec + v * DIM;
        for (int dd = 0; dd < DIM; ++dd) {
            float x = fv[dd] * gscale;
            int q = (int)(x >= 0.0f ? x + 0.5f : x - 0.5f);
            m.entries[v].q8[dd] = (int8_t)(q > 127 ? 127 : (q < -127 ? -127 : q));
        }
    }

    /* subwords: character n-grams of the vocabulary words, each carrying the
     * sign of the mean learned context of the words that contain it. Derived
     * from the learned table, so an unknown word composed from its n-grams
     * lands near the words it looks like, not at a random point. */
    size_t nsub = 0;
    if (opts.max_subwords) {
        KeyMap wmap;
        if (keymap_init(&wmap, 1u << 22) != 0) { build_free(&m); return -5; }
        size_t cap_sub = 1u << 16, n_sub = 0;
        m.subs = (SubStat *)malloc(cap_sub * sizeof(SubStat));
        if (!m.subs) { keymap_free(&wmap); build_free(&m); return -5; }
        uint64_t gk[128];
        int ok = 1;
        for (size_t v = 0; v < vocab && ok; ++v) {
            int32_t *ss = keymap_slot(&m.smap, stats[v].key, 0);
            if (!ss || *ss < 0) continue;
            int n = cnet_vsa_lexicon_subword_keys(m.surf[*ss], opts.subword_min_n, opts.subword_max_n, gk, 128);
            for (int i = 0; i < n; ++i) {
                int dup = 0;
                for (int j = 0; j < i; ++j) if (gk[j] == gk[i]) { dup = 1; break; }
                if (dup) continue;
                int32_t *slot = keymap_slot(&wmap, gk[i], 1);
                if (!slot) { ok = 0; break; }
                if (*slot == -1) {
                    if (n_sub == cap_sub) {
                        cap_sub *= 2;
                        SubStat *nsb = (SubStat *)realloc(m.subs, cap_sub * sizeof(SubStat));
                        if (!nsb) { keymap_free(&wmap); build_free(&m); return -5; }
                        m.subs = nsb;
                    }
                    *slot = (int32_t)n_sub;
                    m.subs[n_sub].key = gk[i]; m.subs[n_sub].words = 0;
                    n_sub++;
                }
                m.subs[*slot].words++;
            }
        }
        /* keep n-grams seen in at least min_words words and in no more than 5%
         * of the vocabulary (ubiquitous suffixes carry nothing); most-shared first */
        size_t kept = 0;
        uint32_t max_words = opts.subword_max_words ? opts.subword_max_words : (uint32_t)(vocab / 20);
        if (max_words < opts.subword_min_words) max_words = (uint32_t)vocab;
        for (size_t i = 0; i < n_sub; ++i) {
            if (m.subs[i].words >= opts.subword_min_words && m.subs[i].words <= max_words) m.subs[kept++] = m.subs[i];
        }
        qsort(m.subs, kept, sizeof(SubStat), cmp_sub_words_desc);
        nsub = kept < opts.max_subwords ? kept : opts.max_subwords;
        keymap_reset(&wmap);
        for (size_t i = 0; i < nsub; ++i) { int32_t *slot = keymap_slot(&wmap, m.subs[i].key, 1); if (slot) *slot = (int32_t)i; }
        m.ssum = (float *)calloc(nsub ? nsub * DIM : 1, sizeof(float));
        m.subwords = (CnetVsaLexiconSubword *)calloc(nsub ? nsub : 1, sizeof(CnetVsaLexiconSubword));
        if (!m.ssum || !m.subwords) { keymap_free(&wmap); build_free(&m); return -5; }
        for (size_t v = 0; v < vocab; ++v) {
            int32_t *ss = keymap_slot(&m.smap, stats[v].key, 0);
            if (!ss || *ss < 0) continue;
            int n = cnet_vsa_lexicon_subword_keys(m.surf[*ss], opts.subword_min_n, opts.subword_max_n, gk, 128);
            const float *cv = m.ctxn + v * DIM;
            for (int i = 0; i < n; ++i) {
                int dup = 0;
                for (int j = 0; j < i; ++j) if (gk[j] == gk[i]) { dup = 1; break; }
                if (dup) continue;
                int32_t *slot = keymap_slot(&wmap, gk[i], 0);
                if (!slot || *slot < 0) continue;
                float *sv = m.ssum + (size_t)(*slot) * DIM;
                for (int dd = 0; dd < DIM; ++dd) sv[dd] += cv[dd];
            }
        }
        for (size_t i = 0; i < nsub; ++i) {
            const float *sv = m.ssum + i * DIM;
            CnetVsaLexiconSubword *sw = &m.subwords[i];
            sw->key = m.subs[i].key;
            sw->words = m.subs[i].words;
            memset(sw->bits, 0, sizeof(sw->bits));
            for (int dd = 0; dd < DIM; ++dd) if (sv[dd] > 0.0f) sw->bits[dd >> 6] |= (1ULL << (dd & 63));
            /* rarer n-grams say more about a word: idf-like magnitude on the int8 scale */
            double iw = log((double)vocab / (double)sw->words) / log((double)vocab);
            if (iw < 0.15) iw = 0.15;
            if (iw > 1.0) iw = 1.0;
            int q = (int)(127.0 * iw + 0.5);
            sw->mag = (uint32_t)(q < 1 ? 1 : (q > 127 ? 127 : q));
        }
        qsort(m.subwords, nsub, sizeof(CnetVsaLexiconSubword), cmp_subword_key);
        keymap_free(&wmap);
    }

    if (opts.vocab_dump && *opts.vocab_dump) {
        FILE *vd = fopen(opts.vocab_dump, "w");
        if (vd) {
            fprintf(vd, "kind\tkey\tsurface_or_first\tsecond\tcount\tdf\n");
            for (size_t v = 0; v < vocab; ++v) {
                int32_t *ss = keymap_slot(&m.smap, stats[v].key, 0);
                fprintf(vd, "W\t%016llx\t%s\t-\t%u\t%u\n", (unsigned long long)stats[v].key,
                        (ss && *ss >= 0) ? m.surf[*ss] : "?", stats[v].count, stats[v].df);
            }
            for (size_t p = 0; p < nphr; ++p) {
                fprintf(vd, "P\t%016llx\t%016llx\t%016llx\t%u\t%u\n", (unsigned long long)m.phr[p].key,
                        (unsigned long long)m.phr[p].a, (unsigned long long)m.phr[p].b, m.phr[p].count, m.phr[p].df);
            }
            fclose(vd);
        }
    }

    qsort(m.entries, rows, sizeof(CnetVsaLexiconEntry), cmp_entry_key);

    CnetVsaLexiconHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = CNET_VSA_LEX_MAGIC;
    hdr.version = CNET_VSA_LEX_VERSION;
    hdr.dim = DIM;
    hdr.count = (uint32_t)rows;
    hdr.key_encoder = CNET_VSA_ENCODER_STEM;
    hdr.window = opts.window;
    hdr.nnz = opts.nnz;
    hdr.min_count = opts.min_count;
    hdr.beta = opts.beta;
    hdr.global_scale = gscale;
    /* an identity-only vector (unknown word, idf 1) has |coordinate| = 1/sqrt(D) */
    {
        double fm = id_mag * gscale;
        int q = (int)(fm + 0.5);
        hdr.fallback_mag = (uint32_t)(q < 1 ? 1 : (q > 127 ? 127 : q));
    }
    hdr.reflective = opts.reflective;
    hdr.max_df = opts.max_df;
    hdr.center = opts.center;
    hdr.remove_pcs = opts.remove_pcs;
    hdr.distilled = n_distilled;
    hdr.distill_alpha = n_distilled ? opts.distill_alpha : 0.0f;
    hdr.idf_floor = opts.idf_floor;
    hdr.idf_power = opts.idf_power;
    hdr.phrases = (uint32_t)nphr;
    hdr.phrase_min_count = nphr ? opts.phrase_min_count : 0;
    hdr.phrase_weight = nphr ? opts.phrase_weight : 0.0f;
    hdr.subwords = (uint32_t)nsub;
    hdr.subword_min_n = nsub ? opts.subword_min_n : 0;
    hdr.subword_max_n = nsub ? opts.subword_max_n : 0;
    hdr.subword_min_words = nsub ? opts.subword_min_words : 0;
    hdr.subword_max_words = nsub ? (opts.subword_max_words ? opts.subword_max_words : (uint32_t)(vocab / 20)) : 0;
    hdr.subword_weight = nsub ? opts.subword_weight : 0.0f;
    hdr.sentences = sentences;
    hdr.tokens = swept;
    hdr.corpus_hash = corpus_hash;
    hdr.digest = lexicon_digest(&hdr, m.entries, m.subwords);

    int rc = lexicon_write(&hdr, m.entries, m.subwords, out_path);

    if (report) {
        report->files = files; report->sentences = sentences; report->tokens = swept;
        report->distinct = distinct; report->vocab = (uint32_t)vocab; report->phrases = (uint32_t)nphr;
        report->subwords = (uint32_t)nsub; report->sweep_seconds = sweep_s;
    }
    build_free(&m);
    return rc;
}

/* ---- supervised training on (question, capsule) pairs ---------------------- */

/* the runtime's term rule: content tokens (all tokens when there is none),
 * word entries by stem key, phrase entries for adjacent content words, and the
 * out-of-vocabulary contribution (subword composition or identity) */
static int text_terms(const CnetVsaLexicon *lex, const char *text, int32_t *idx, int cap, float *fixed, float inv_scale) {
    CnetVsaTokenList tl;
    if (cnet_vsa_text_tokenize(text, &tl) <= 0) return 0;
    size_t limit = tl.count < 250 ? tl.count : 250;
    int n = 0, content = 0;
    for (size_t i = 0; i < limit; ++i) if (!cnet_vsa_text_is_stopword(tl.tokens[i].token)) content++;
    uint64_t prev = 0;
    char stem[CNET_VSA_TOKEN_LEN];
    int16_t oov[DIM];
    for (size_t i = 0; i < limit && n < cap; ++i) {
        if (content && cnet_vsa_text_is_stopword(tl.tokens[i].token)) continue;
        uint64_t k = word_key_and_stem(tl.tokens[i].token, stem, sizeof(stem));
        int32_t e = entry_index(lex, k);
        if (e >= 0) idx[n++] = e;
        else if (fixed) {
            cnet_vsa_lexicon_oov_vector(lex, stem, oov);
            for (int d = 0; d < DIM; ++d) fixed[d] += (float)oov[d] * inv_scale;
        }
        if (lex->hdr.phrases && prev && n < cap) {
            int32_t p = entry_index(lex, cnet_vsa_lexicon_phrase_key(prev, k));
            if (p >= 0) idx[n++] = p;
        }
        prev = k;
    }
    return n;
}

typedef struct { int corpus; int neg; char *text; } TrainPair;   /* neg: explicit confusable capsule to push away from (-1 = none) */

int cnet_vsa_lexicon_train(const char *in_path, const char *out_path, const char *corpus_dir,
                           const char *pairs_tsv, const CnetVsaLexiconTrainOpts *opts_in,
                           CnetVsaLexiconTrainReport *report) {
    if (!in_path || !out_path || !corpus_dir || !pairs_tsv) return -1;
    CnetVsaLexiconTrainOpts opts;
    if (opts_in) opts = *opts_in; else cnet_vsa_lexicon_train_opts_default(&opts);
    if (opts.epochs == 0 || opts.epochs > 1000 || !(opts.lr > 0.0f) || opts.margin < 0.0f) return -1;
    if (report) memset(report, 0, sizeof(*report));
    double t0 = now_s();

    CnetVsaLexicon lex;
    int rc = cnet_vsa_lexicon_load(&lex, in_path);
    if (rc != 0) return -2;
    const uint32_t N = lex.hdr.count;

    /* corpora: name -> sentences */
    DIR *d = opendir(corpus_dir);
    if (!d) { cnet_vsa_lexicon_free(&lex); return -3; }
    size_t cap_c = 256, nc = 0;
    char (*names)[128] = (char (*)[128])malloc(cap_c * 128);
    char ***sents = (char ***)malloc(cap_c * sizeof(char **));
    int *nsent = (int *)malloc(cap_c * sizeof(int));
    if (!names || !sents || !nsent) { closedir(d); cnet_vsa_lexicon_free(&lex); free(names); free(sents); free(nsent); return -5; }
    struct dirent *de;
    char path[1024], line[4096];
    while ((de = readdir(d)) != NULL) {
        size_t len = strlen(de->d_name);
        if (len < 12 || len - 11 >= 128 || strcmp(de->d_name + len - 11, "_corpus.txt") != 0) continue;
        snprintf(path, sizeof(path), "%s/%s", corpus_dir, de->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (nc == cap_c) {
            cap_c *= 2;
            char (*nn)[128] = (char (*)[128])realloc(names, cap_c * 128);
            char ***ns = (char ***)realloc(sents, cap_c * sizeof(char **));
            int *nn2 = (int *)realloc(nsent, cap_c * sizeof(int));
            if (nn) names = nn;
            if (ns) sents = ns;
            if (nn2) nsent = nn2;
            if (!nn || !ns || !nn2) { fclose(fp); break; }
        }
        memcpy(names[nc], de->d_name, len - 11); names[nc][len - 11] = 0;
        size_t cap_s = 64; int ns2 = 0;
        char **arr = (char **)malloc(cap_s * sizeof(char *));
        while (arr && fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if (strlen(line) < 8) continue;
            if ((size_t)ns2 == cap_s) { cap_s *= 2; char **na = (char **)realloc(arr, cap_s * sizeof(char *)); if (!na) break; arr = na; }
            arr[ns2++] = strdup(line);
        }
        fclose(fp);
        sents[nc] = arr; nsent[nc] = arr ? ns2 : 0; nc++;
    }
    closedir(d);

    /* pairs: "<name>\t<question>" */
    size_t cap_p = 1024, np = 0;
    TrainPair *pairs = (TrainPair *)malloc(cap_p * sizeof(TrainPair));
    FILE *pf = fopen(pairs_tsv, "r");
    if (!pairs || !pf) {
        if (pf) fclose(pf);
        free(pairs);
        for (size_t c = 0; c < nc; ++c) { for (int i = 0; i < nsent[c]; ++i) free(sents[c][i]); free(sents[c]); }
        free(names); free(sents); free(nsent); cnet_vsa_lexicon_free(&lex);
        return -4;
    }
    uint32_t n_explicit_neg = 0;
    while (fgets(line, sizeof(line), pf)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#') continue;
        char *t = strchr(line, '\t');
        if (!t) continue;
        *t = 0;
        char *t2 = strchr(t + 1, '\t');            /* optional third column: explicit negative capsule */
        if (t2) *t2 = 0;
        int ci = -1, ni = -1;
        for (size_t c = 0; c < nc; ++c) if (strcmp(names[c], line) == 0) { ci = (int)c; break; }
        if (t2 && t2[1]) for (size_t c = 0; c < nc; ++c) if (strcmp(names[c], t2 + 1) == 0) { ni = (int)c; break; }
        if (ci < 0 || strlen(t + 1) < 4) continue;
        if (ni == ci) ni = -1;
        if (np == cap_p) { cap_p *= 2; TrainPair *npr = (TrainPair *)realloc(pairs, cap_p * sizeof(TrainPair)); if (!npr) break; pairs = npr; }
        pairs[np].corpus = ci; pairs[np].neg = ni; pairs[np].text = strdup(t + 1); np++;
        if (ni >= 0) n_explicit_neg++;
    }
    fclose(pf);
    const size_t n_question_pairs = np;
    /* the contract itself: up to N of each corpus's own sentences, spread over the corpus */
    if (opts.sentences_per_corpus) {
        for (size_t c = 0; c < nc; ++c) {
            int n = nsent[c];
            if (n <= 0) continue;
            int take = (int)opts.sentences_per_corpus < n ? (int)opts.sentences_per_corpus : n;
            for (int t = 0; t < take; ++t) {
                int i = (int)(((long)t * n) / take);
                if (np == cap_p) { cap_p *= 2; TrainPair *npr = (TrainPair *)realloc(pairs, cap_p * sizeof(TrainPair)); if (!npr) break; pairs = npr; }
                pairs[np].corpus = (int)c; pairs[np].neg = -1; pairs[np].text = strdup(sents[c][i]); np++;
            }
        }
    }

    int status = 0;
    float *W = (float *)malloc((size_t)N * DIM * sizeof(float));
    float *rnorm = (float *)malloc(N * sizeof(float));
    float *C = (float *)malloc(nc * DIM * sizeof(float));
    float *q = (float *)malloc(DIM * sizeof(float));
    float *g = (float *)malloc(DIM * sizeof(float));
    float *scores = (float *)malloc(nc * sizeof(float));
    int32_t *idx = (int32_t *)malloc(512 * sizeof(int32_t));
    size_t *order = (size_t *)malloc((np ? np : 1) * sizeof(size_t));
    unsigned char *touched = (unsigned char *)calloc(N, 1);
    if (!W || !rnorm || !C || !q || !g || !scores || !idx || !order || !touched || nc < 2 || np == 0) {
        status = (nc < 2 || np == 0) ? -4 : -5;
        goto done;
    }
    const float inv_scale = 1.0f / lex.hdr.global_scale;
    for (uint32_t e = 0; e < N; ++e) {
        double nn = 0.0;
        for (int dd = 0; dd < DIM; ++dd) { float x = (float)lex.entries[e].q8[dd] * inv_scale; W[(size_t)e * DIM + dd] = x; nn += (double)x * x; }
        rnorm[e] = (float)sqrt(nn);
    }
    if (report) { report->pairs = (uint32_t)n_question_pairs; report->sentence_pairs = (uint32_t)(np - n_question_pairs); report->corpora = (uint32_t)nc; report->epochs = opts.epochs; report->explicit_negatives = n_explicit_neg; }

    uint64_t rs = opts.seed;
    float top1_before = -1.0f, top1_after = 0.0f;
    for (uint32_t ep = 0; ep <= opts.epochs; ++ep) {
        /* centroids from the current table: mean of unit sentence vectors, unit */
        for (size_t c = 0; c < nc; ++c) {
            float *cv = C + c * DIM;
            memset(cv, 0, DIM * sizeof(float));
            for (int i = 0; i < nsent[c]; ++i) {
                memset(q, 0, DIM * sizeof(float));
                int n = text_terms(&lex, sents[c][i], idx, 512, q, inv_scale);
                for (int t = 0; t < n; ++t) { const float *w = W + (size_t)idx[t] * DIM; for (int dd = 0; dd < DIM; ++dd) q[dd] += w[dd]; }
                unit_row(q);
                for (int dd = 0; dd < DIM; ++dd) cv[dd] += q[dd];
            }
            unit_row(cv);
        }
        /* one pass over the pairs: evaluate, and update unless this is the final pass */
        const int final_pass = ep == opts.epochs;
        const float lr = opts.lr * (1.0f - (float)ep / (float)opts.epochs);
        for (size_t i = 0; i < np; ++i) order[i] = i;
        for (size_t i = np; i > 1; --i) { size_t j = (size_t)(xorshift64(&rs) % i); size_t tmp = order[i - 1]; order[i - 1] = order[j]; order[j] = tmp; }
        int hits = 0;
        for (size_t oi = 0; oi < np; ++oi) {
            const TrainPair *p = &pairs[order[oi]];
            memset(q, 0, DIM * sizeof(float));
            int n = text_terms(&lex, p->text, idx, 512, q, inv_scale);
            for (int t = 0; t < n; ++t) { const float *w = W + (size_t)idx[t] * DIM; for (int dd = 0; dd < DIM; ++dd) q[dd] += w[dd]; }
            double qn = 0.0;
            for (int dd = 0; dd < DIM; ++dd) qn += (double)q[dd] * q[dd];
            qn = sqrt(qn);
            if (!(qn > 0.0)) continue;
            int best = -1; float sbest = -2.0f, sgold = -2.0f;
            for (size_t c = 0; c < nc; ++c) {
                const float *cv = C + c * DIM;
                double dot = 0.0;
                for (int dd = 0; dd < DIM; ++dd) dot += (double)q[dd] * cv[dd];
                float s = (float)(dot / qn);
                scores[c] = s;
                if ((int)c == p->corpus) sgold = s;
                else if (s > sbest) { sbest = s; best = (int)c; }
            }
            if (sgold > sbest && order[oi] < n_question_pairs) hits++;
            if (final_pass || n == 0 || best < 0) continue;
            /* the negative: the hardest other centroid, and the explicit confusable
             * capsule when the pair names one and it still violates the margin */
            int negc = best; float sneg = sbest;
            if (p->neg >= 0 && scores[p->neg] + opts.margin > sgold && scores[p->neg] > sbest - opts.margin) { negc = p->neg; sneg = scores[p->neg]; }
            float loss = opts.margin - sgold + sneg;
            if (loss <= 0.0f) continue;
            /* d cos(q, c) / dq = (c - cos * q_hat) / |q|; push toward gold, away from the negative */
            const float *cg = C + (size_t)p->corpus * DIM, *cn = C + (size_t)negc * DIM;
            for (int dd = 0; dd < DIM; ++dd) {
                float qh = (float)(q[dd] / qn);
                g[dd] = (float)(((cg[dd] - sgold * qh) - (cn[dd] - sneg * qh)) / qn);
            }
            for (int t = 0; t < n; ++t) {
                float *w = W + (size_t)idx[t] * DIM;
                for (int dd = 0; dd < DIM; ++dd) w[dd] += lr * g[dd];
                /* keep the row norm (idf weight): training moves directions only */
                double nn = 0.0;
                for (int dd = 0; dd < DIM; ++dd) nn += (double)w[dd] * w[dd];
                nn = nn > 0.0 ? sqrt(nn) : 1.0;
                float f = (float)(rnorm[idx[t]] / nn);
                for (int dd = 0; dd < DIM; ++dd) w[dd] *= f;
                touched[idx[t]] = 1;
            }
        }
        float top1 = n_question_pairs ? (float)hits / (float)n_question_pairs : 0.0f;
        if (ep == 0) top1_before = top1;
        top1_after = top1;
    }

    /* requantise under a fresh global scale; the identity magnitude follows it */
    {
        double max_abs = 0.0;
        for (size_t i = 0; i < (size_t)N * DIM; ++i) { double a = fabs((double)W[i]); if (a > max_abs) max_abs = a; }
        float gscale = max_abs > 0.0 ? (float)(127.0 / max_abs) : 1.0f;
        for (uint32_t e = 0; e < N; ++e) {
            for (int dd = 0; dd < DIM; ++dd) {
                float x = W[(size_t)e * DIM + dd] * gscale;
                int qv = (int)(x >= 0.0f ? x + 0.5f : x - 0.5f);
                lex.entries[e].q8[dd] = (int8_t)(qv > 127 ? 127 : (qv < -127 ? -127 : qv));
            }
        }
        lex.hdr.global_scale = gscale;
        double fm = (1.0 / sqrt((double)DIM)) * gscale;
        int qv = (int)(fm + 0.5);
        lex.hdr.fallback_mag = (uint32_t)(qv < 1 ? 1 : (qv > 127 ? 127 : qv));
        lex.hdr.trained_pairs = (uint32_t)n_question_pairs;
        lex.hdr.trained_sentences = (uint32_t)(np - n_question_pairs);
        lex.hdr.train_epochs = opts.epochs;
        lex.hdr.train_lr = opts.lr;
        lex.hdr.train_margin = opts.margin;
        lex.hdr.digest = lexicon_digest(&lex.hdr, lex.entries, lex.subwords);
        status = lexicon_write(&lex.hdr, lex.entries, lex.subwords, out_path);
    }
    if (report) {
        uint32_t nt = 0; for (uint32_t e = 0; e < N; ++e) nt += touched[e];
        report->terms_touched = nt; report->train_top1_before = top1_before; report->train_top1_after = top1_after;
        report->seconds = now_s() - t0;
    }
done:
    free(W); free(rnorm); free(C); free(q); free(g); free(scores); free(idx); free(order); free(touched);
    for (size_t i = 0; i < np; ++i) free(pairs[i].text);
    free(pairs);
    for (size_t c = 0; c < nc; ++c) { for (int i = 0; i < nsent[c]; ++i) free(sents[c][i]); free(sents[c]); }
    free(names); free(sents); free(nsent);
    cnet_vsa_lexicon_free(&lex);
    return status;
}
