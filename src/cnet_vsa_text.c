#include "../include/cnet_vsa_text.h"
#include "../include/cnet_vsa_lexicon.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#if defined(__AVX512VNNI__) && defined(__AVX512BW__)
#include <immintrin.h>
#endif

static uint64_t fnv1a64(const char *str) {
    uint64_t hash = 14695981039346656037ULL;
    while (*str) {
        hash ^= (uint8_t)(*str++);
        hash *= 1099511628211ULL;
    }
    return hash;
}

static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    if (x == 0) x = 88172645463325252ULL;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

void cnet_vsa_text_token_vec(const char *token, float *out_vec, int dim) {
    if (!token || !out_vec || dim <= 0) return;
    uint64_t seed = fnv1a64(token);
    float inv_sqrt = 1.0f / sqrtf((float)dim);
    for (int i = 0; i < dim; ++i) {
        out_vec[i] = (xorshift64(&seed) & 1) ? inv_sqrt : -inv_sqrt;
    }
}

void cnet_vsa_text_token_bsc(const char *token, CnetVsaBsc *out_bsc) {
    if (!token || !out_bsc) return;
    uint64_t seed = fnv1a64(token);
    for (int i = 0; i < CNET_VSA_BSC_WORDS; ++i) {
        out_bsc->w[i] = xorshift64(&seed);
    }
}

int cnet_vsa_text_tokenize(const char *text, CnetVsaTokenList *out_list) {
    if (!text || !out_list) return -1;
    memset(out_list, 0, sizeof(*out_list));

    const char *p = text;
    int pos = 0;

    while (*p && out_list->count < CNET_VSA_MAX_TOKENS) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        char buf[CNET_VSA_TOKEN_LEN];
        size_t len = 0;

        if (isalnum((unsigned char)*p) || *p == '_') {
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && len < sizeof(buf) - 1) {
                buf[len++] = (char)tolower((unsigned char)*p++);
            }
        } else {
            buf[len++] = *p++;
        }
        buf[len] = '\0';

        CnetVsaToken *t = &out_list->tokens[out_list->count++];
        snprintf(t->token, sizeof(t->token), "%s", buf);
        t->position = pos++;
    }

    return (int)out_list->count;
}

int cnet_vsa_text_encode_continuous(const CnetVsaTokenList *tokens, float *out_seq_vec, int dim) {
    if (!tokens || !out_seq_vec || dim <= 0 || tokens->count == 0) return -1;

    double accum[CNET_VSA_DEFAULT_DIM] = {0};
    double *p_accum = accum;
    if (dim > CNET_VSA_DEFAULT_DIM) {
        p_accum = (double*)calloc((size_t)dim, sizeof(double));
    }

    for (size_t i = 0; i < tokens->count; ++i) {
        float base_vec[CNET_VSA_DEFAULT_DIM];
        float perm_vec[CNET_VSA_DEFAULT_DIM];

        cnet_vsa_text_token_vec(tokens->tokens[i].token, base_vec, dim);
        /* Circular roll by position index */
        cnet_vsa_permute(perm_vec, base_vec, (int)i, dim);

        for (int j = 0; j < dim; ++j) {
            p_accum[j] += (double)perm_vec[j];
        }
    }

    for (int j = 0; j < dim; ++j) {
        out_seq_vec[j] = (float)p_accum[j];
    }
    cnet_vsa_normalize(out_seq_vec, dim);

    if (p_accum != accum) free(p_accum);
    return 0;
}

int cnet_vsa_text_encode_bsc(const CnetVsaTokenList *tokens, CnetVsaBsc *out_seq_bsc) {
    if (!tokens || !out_seq_bsc || tokens->count == 0) return -1;

    CnetVsaBsc perm_list[CNET_VSA_MAX_TOKENS];
    for (size_t i = 0; i < tokens->count; ++i) {
        CnetVsaBsc base_bsc;
        cnet_vsa_text_token_bsc(tokens->tokens[i].token, &base_bsc);
        /* Shift each position by (i * 7 + 1) bits for quasi-orthogonal dispersion */
        cnet_vsa_bsc_permute(&perm_list[i], &base_bsc, (int)(i * 7 + 1));
    }

    const CnetVsaBsc *ptrs[CNET_VSA_MAX_TOKENS];
    for (size_t i = 0; i < tokens->count; ++i) {
        ptrs[i] = &perm_list[i];
    }
    cnet_vsa_bsc_bundle(out_seq_bsc, ptrs, (int)tokens->count);
    return 0;
}

int cnet_vsa_text_decode_at_pos(const float *seq_vec, int pos, const CnetVsaCodebook *vocab,
                                char *out_token, size_t max_len, float *out_sim) {
    if (!seq_vec || !vocab || !out_token || max_len == 0) return -1;

    int D = vocab->dim;
    float probe[CNET_VSA_DEFAULT_DIM];
    /* Inverse permutation roll: -pos */
    cnet_vsa_permute(probe, seq_vec, -pos, D);

    return cnet_vsa_codebook_cleanup(vocab, probe, NULL, out_token, max_len, out_sim);
}

int cnet_vsa_text_find_token_pos(const float *seq_vec, const char *token, int max_pos,
                                  int *out_pos, float *out_sim, int dim) {
    if (!seq_vec || !token || max_pos <= 0 || dim <= 0) return -1;

    float base_vec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_text_token_vec(token, base_vec, dim);

    float best_sim = -2.0f;
    int best_pos = -1;

    for (int p = 0; p < max_pos; ++p) {
        float perm_vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_permute(perm_vec, base_vec, p, dim);
        float sim = cnet_vsa_similarity(perm_vec, seq_vec, dim);
        if (sim > best_sim) {
            best_sim = sim;
            best_pos = p;
        }
    }

    if (out_pos) *out_pos = best_pos;
    if (out_sim) *out_sim = best_sim;
    return 0;
}

float cnet_vsa_text_sequence_similarity(const char *text_a, const char *text_b, int dim) {
    if (!text_a || !text_b || dim <= 0) return 0.0f;

    CnetVsaTokenList list_a, list_b;
    if (cnet_vsa_text_tokenize(text_a, &list_a) <= 0) return 0.0f;
    if (cnet_vsa_text_tokenize(text_b, &list_b) <= 0) return 0.0f;

    float vec_a[CNET_VSA_DEFAULT_DIM];
    float vec_b[CNET_VSA_DEFAULT_DIM];

    cnet_vsa_text_encode_continuous(&list_a, vec_a, dim);
    cnet_vsa_text_encode_continuous(&list_b, vec_b, dim);

    return cnet_vsa_similarity(vec_a, vec_b, dim);
}

int cnet_vsa_text_is_stopword(const char *token) {
    if (!token || !*token) return 1;

    /* Punctuation and non-alphanumeric tokens are purely syntactic delimiters, not topical content */
    int has_alnum = 0;
    for (const char *p = token; *p; ++p) {
        if (isalnum((unsigned char)*p)) {
            has_alnum = 1;
            break;
        }
    }
    if (!has_alnum) return 1;

    static const char * const stopwords[] = {
        "the", "a", "an", "is", "was", "are", "were", "to", "in", "on",
        "of", "and", "or", "for", "with", "from", "at", "by", "this",
        "that", "it", "its", "as", "be", "than", "there", "all", "so",
        "if", "into", "up", "out", "he", "she", "they", "we", "i", "you",
        "how", "what", "which", "where", "when", "why", "who", "do", "does",
        "did", "can", "could", "would", "should", "have", "has", "had",
        "will", "may", "might", "differ", "between",
        NULL
    };
    for (int i = 0; stopwords[i]; ++i) {
        if (strcmp(token, stopwords[i]) == 0) return 1;
    }
    return 0;
}

int cnet_vsa_text_encode_topical(const CnetVsaTokenList *tokens, float *out_topical_vec, int dim) {
    if (!tokens || !out_topical_vec || dim <= 0 || dim > CNET_VSA_DEFAULT_DIM || tokens->count == 0) return -1;

    double accum[CNET_VSA_DEFAULT_DIM] = {0};
    double *p_accum = accum;
    if (dim > CNET_VSA_DEFAULT_DIM) {
        p_accum = (double*)calloc((size_t)dim, sizeof(double));
        if (!p_accum) return -2;
    }

    int content_count = 0;
    for (size_t i = 0; i < tokens->count; ++i) {
        if (cnet_vsa_text_is_stopword(tokens->tokens[i].token)) continue;

        float base_vec[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_token_vec(tokens->tokens[i].token, base_vec, dim);

        for (int j = 0; j < dim; ++j) {
            p_accum[j] += (double)base_vec[j];
        }
        content_count++;
    }

    /* Fallback: if all tokens were stopwords, encode all tokens */
    if (content_count == 0) {
        for (size_t i = 0; i < tokens->count; ++i) {
            float base_vec[CNET_VSA_DEFAULT_DIM];
            cnet_vsa_text_token_vec(tokens->tokens[i].token, base_vec, dim);

            for (int j = 0; j < dim; ++j) {
                p_accum[j] += (double)base_vec[j];
            }
        }
    }

    for (int j = 0; j < dim; ++j) {
        out_topical_vec[j] = (float)p_accum[j];
    }
    cnet_vsa_normalize(out_topical_vec, dim);

    if (p_accum != accum) free(p_accum);
    return 0;
}

void cnet_vsa_text_token_vec_chargram(const char *token, float *out_vec, int dim) {
    if (!token || !out_vec || dim <= 0 || dim > CNET_VSA_DEFAULT_DIM) return;
    size_t n = strlen(token);
    if (n < 3) {
        cnet_vsa_text_token_vec(token, out_vec, dim);
        return;
    }

    char pad[CNET_VSA_TOKEN_LEN + 4];
    pad[0] = '#';
    size_t copy = n < CNET_VSA_TOKEN_LEN - 1 ? n : (CNET_VSA_TOKEN_LEN - 1);
    memcpy(pad + 1, token, copy);
    pad[1 + copy] = '#';
    pad[2 + copy] = '\0';
    size_t pn = 2 + copy;

    double acc[CNET_VSA_DEFAULT_DIM];
    memset(acc, 0, sizeof(double) * (size_t)dim);
    float gram[CNET_VSA_DEFAULT_DIM];
    int grams = 0;
    for (size_t i = 0; i + 3 <= pn; ++i) {
        char tri[4] = { pad[i], pad[i + 1], pad[i + 2], 0 };
        cnet_vsa_text_token_vec(tri, gram, dim);
        for (int j = 0; j < dim; ++j) acc[j] += (double)gram[j];
        grams++;
    }
    if (grams == 0) {
        cnet_vsa_text_token_vec(token, out_vec, dim);
        return;
    }
    for (int j = 0; j < dim; ++j) out_vec[j] = (float)acc[j];
    cnet_vsa_normalize(out_vec, dim);
}

/* ---- encoder registry ------------------------------------------------------ */

static const char *const k_encoder_names[CNET_VSA_ENCODER_COUNT] = {
    "bag", "hd", "stem", "stem_bi", "cgram_c", "lex"
};

int cnet_vsa_encoder_valid(uint32_t encoder_id) {
    return encoder_id < CNET_VSA_ENCODER_COUNT ? 1 : 0;
}

const char *cnet_vsa_encoder_name(uint32_t encoder_id) {
    return encoder_id < CNET_VSA_ENCODER_COUNT ? k_encoder_names[encoder_id] : NULL;
}

int cnet_vsa_encoder_parse(const char *name, uint32_t *out) {
    if (!name || !out) return -1;
    char low[32];
    size_t n = strlen(name);
    if (n == 0 || n >= sizeof(low)) return -1;
    for (size_t i = 0; i <= n; ++i) low[i] = (char)tolower((unsigned char)name[i]);
    if (strcmp(low, "default") == 0) { *out = CNET_VSA_ENCODER_DEFAULT; return 0; }
    for (uint32_t i = 0; i < CNET_VSA_ENCODER_COUNT; ++i) {
        if (strcmp(low, k_encoder_names[i]) == 0) { *out = i; return 0; }
    }
    return -1;
}

/* ---- stemming -------------------------------------------------------------- */
/* Porter (1980) steps 1a, 1b, 1c, the adverbial part of step 2, and step 5,
 * applied only to all-lowercase alphabetic tokens of 4+ letters. Derivational
 * steps 2-4 (-ation, -ness, ...) are deliberately left out: they merge more
 * unrelated technical terms than they unify. Known quirks kept for fidelity:
 * embed -> emb while embedded -> embed; use/used/uses stay distinct (3-letter
 * base is below the length guard). The registry sweep gate is the arbiter. */

static int stem_is_cons(const char *t, int i) {
    char c = t[i];
    if (c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u') return 0;
    if (c == 'y') return (i == 0) ? 1 : !stem_is_cons(t, i - 1);
    return 1;
}

/* Porter measure m: number of VC sequences in t[0..n) */
static int stem_measure(const char *t, int n) {
    int m = 0, i = 0;
    while (i < n && stem_is_cons(t, i)) i++;
    while (i < n) {
        while (i < n && !stem_is_cons(t, i)) i++;
        if (i >= n) break;
        m++;
        while (i < n && stem_is_cons(t, i)) i++;
    }
    return m;
}

static int stem_has_vowel(const char *t, int n) {
    for (int i = 0; i < n; ++i) if (!stem_is_cons(t, i)) return 1;
    return 0;
}

static int stem_double_cons(const char *t, int n) {
    return n >= 2 && t[n - 1] == t[n - 2] && stem_is_cons(t, n - 1);
}

/* *o: stem ends consonant-vowel-consonant, final consonant not w, x, y */
static int stem_cvc(const char *t, int n) {
    if (n < 3) return 0;
    if (!stem_is_cons(t, n - 1) || stem_is_cons(t, n - 2) || !stem_is_cons(t, n - 3)) return 0;
    char c = t[n - 1];
    return (c != 'w' && c != 'x' && c != 'y');
}

static int stem_ends(const char *t, int n, const char *suf) {
    int m = (int)strlen(suf);
    return (n >= m && memcmp(t + n - m, suf, (size_t)m) == 0) ? 1 : 0;
}

void cnet_vsa_text_stem(char *t) {
    if (!t) return;
    int n = (int)strlen(t);
    if (n < 4) return;
    for (int i = 0; i < n; ++i) {
        if (t[i] < 'a' || t[i] > 'z') return; /* digits, underscores: leave as is */
    }

    /* step 1a: plurals */
    if (stem_ends(t, n, "sses")) n -= 2;
    else if (stem_ends(t, n, "ies")) n -= 2;
    else if (stem_ends(t, n, "ss")) { /* keep */ }
    else if (t[n - 1] == 's' && t[n - 2] != 'u') n -= 1;   /* bus, plus, status keep their s */
    t[n] = '\0';

    /* step 1b: -eed / -ed / -ing */
    if (stem_ends(t, n, "eed")) {
        if (stem_measure(t, n - 3) > 0) { n -= 1; t[n] = '\0'; }   /* agreed -> agree, feed stays */
    } else {
        int stripped = 0;
        if (stem_ends(t, n, "ed") && n - 2 >= 3 && stem_has_vowel(t, n - 2)) { n -= 2; stripped = 1; }
        else if (stem_ends(t, n, "ing") && n - 3 >= 3 && stem_has_vowel(t, n - 3)) { n -= 3; stripped = 1; }
        if (stripped) {
            t[n] = '\0';
            if (stem_ends(t, n, "at") || stem_ends(t, n, "bl") || stem_ends(t, n, "iz")) {
                t[n++] = 'e'; t[n] = '\0';                              /* conflat -> conflate */
            } else if (n >= 4 && stem_double_cons(t, n) &&
                       t[n - 1] != 'l' && t[n - 1] != 's' && t[n - 1] != 'z' && t[n - 1] != 'f') {
                n -= 1; t[n] = '\0';                                    /* runn -> run; add, staff keep */
            } else if (stem_measure(t, n) == 1 && stem_cvc(t, n)) {
                t[n++] = 'e'; t[n] = '\0';                              /* fil -> file, tim -> time */
            }
        }
    }

    /* step 1c: y -> i when the stem has a vowel */
    if (n >= 3 && t[n - 1] == 'y' && stem_has_vowel(t, n - 1)) t[n - 1] = 'i';

    /* step 2, adverbial suffixes only, (m > 0) on the stem */
    {
        static const struct { const char *from; const char *to; } adv[] = {
            { "ousli", "ous" }, { "entli", "ent" }, { "fulli", "ful" },
            { "alli", "al" }, { "bli", "ble" }, { "eli", "e" },
        };
        for (size_t k = 0; k < sizeof(adv) / sizeof(adv[0]); ++k) {
            int fl = (int)strlen(adv[k].from);
            if (stem_ends(t, n, adv[k].from) && stem_measure(t, n - fl) > 0) {
                int tl = (int)strlen(adv[k].to);
                memcpy(t + n - fl, adv[k].to, (size_t)tl);
                n = n - fl + tl;
                t[n] = '\0';
                break;
            }
        }
    }

    /* step 5a: drop a final e when m > 1, or m == 1 and not *o */
    if (n >= 4 && t[n - 1] == 'e') {
        int m = stem_measure(t, n - 1);
        if (m > 1 || (m == 1 && !stem_cvc(t, n - 1))) { n -= 1; t[n] = '\0'; }
    }
    /* step 5b: -ll -> -l when m > 1 */
    if (n >= 4 && stem_measure(t, n) > 1 && stem_ends(t, n, "ll")) { n -= 1; t[n] = '\0'; }
}

static void token_vec_stem(const char *token, float *out_vec, int dim) {
    char buf[CNET_VSA_TOKEN_LEN];
    snprintf(buf, sizeof(buf), "%s", token);
    cnet_vsa_text_stem(buf);
    cnet_vsa_text_token_vec(buf, out_vec, dim);
}

/* ---- frequent-trigram stoplist for the centered char-gram encoder ----------- */

/* 24-bit packed trigrams, sorted, of the most frequent English/technical-prose
 * trigrams including '#' word boundaries. They carry no topical information and
 * are what makes every char-gram word vector share a common component. */
static uint32_t pack_tri(const char *t) {
    return ((uint32_t)(unsigned char)t[0] << 16) | ((uint32_t)(unsigned char)t[1] << 8) | (uint32_t)(unsigned char)t[2];
}

static const char *const k_stop_trigrams_src[] = {
    "#an", "#co", "#de", "#di", "#en", "#ex", "#in", "#pr", "#re", "#st", "#th", "#un",
    "abl", "ain", "ali", "all", "anc", "and", "ant", "are", "ate", "ati", "cal", "com", "con",
    "cti", "ear", "ect", "ed#", "enc", "ent", "er#", "ere", "ers", "es#", "ess", "est", "eve",
    "for", "hat", "her", "his", "ial", "ica", "ing", "int", "ion", "ist", "ite", "iti", "ity",
    "ive", "ly#", "men", "nce", "ng#", "ns#", "not", "on#", "one", "ons", "ous", "our", "per",
    "pro", "rat", "rea", "res", "sta", "ted", "ter", "tha", "the", "thi", "tio", "tur", "ure",
    "ver", "was", "wit", "ith"
};
#define K_STOP_TRIGRAMS (sizeof(k_stop_trigrams_src) / sizeof(k_stop_trigrams_src[0]))

static uint32_t k_stop_trigrams[K_STOP_TRIGRAMS];
static int k_stop_trigrams_ready = 0;

static int cmp_u32(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

static void stop_trigrams_init(void) {
    if (k_stop_trigrams_ready) return;
    for (size_t i = 0; i < K_STOP_TRIGRAMS; ++i) k_stop_trigrams[i] = pack_tri(k_stop_trigrams_src[i]);
    qsort(k_stop_trigrams, K_STOP_TRIGRAMS, sizeof(uint32_t), cmp_u32);
    k_stop_trigrams_ready = 1;
}

static int is_stop_trigram(const char *tri) {
    uint32_t key = pack_tri(tri);
    size_t lo = 0, hi = K_STOP_TRIGRAMS;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (k_stop_trigrams[mid] < key) lo = mid + 1;
        else if (k_stop_trigrams[mid] > key) hi = mid;
        else return 1;
    }
    return 0;
}

/* char-trigram word vector with frequent trigrams removed; falls back to the
 * whole-word hash when nothing informative is left */
static void token_vec_chargram_centered(const char *token, float *out_vec, int dim) {
    stop_trigrams_init();
    size_t n = strlen(token);
    if (n < 3) { cnet_vsa_text_token_vec(token, out_vec, dim); return; }

    char pad[CNET_VSA_TOKEN_LEN + 4];
    pad[0] = '#';
    size_t copy = n < CNET_VSA_TOKEN_LEN - 1 ? n : (CNET_VSA_TOKEN_LEN - 1);
    memcpy(pad + 1, token, copy);
    pad[1 + copy] = '#';
    size_t pn = 2 + copy;

    double acc[CNET_VSA_DEFAULT_DIM];
    memset(acc, 0, sizeof(double) * (size_t)dim);
    float gram[CNET_VSA_DEFAULT_DIM];
    int grams = 0;
    for (size_t i = 0; i + 3 <= pn; ++i) {
        char tri[4] = { pad[i], pad[i + 1], pad[i + 2], 0 };
        if (is_stop_trigram(tri)) continue;
        cnet_vsa_text_token_vec(tri, gram, dim);
        for (int j = 0; j < dim; ++j) acc[j] += (double)gram[j];
        grams++;
    }
    if (grams == 0) { cnet_vsa_text_token_vec(token, out_vec, dim); return; }
    for (int j = 0; j < dim; ++j) out_vec[j] = (float)acc[j];
    cnet_vsa_normalize(out_vec, dim);
}

/* Generic bag (+ optional bound bigram) encoder over a word-vector function. */
typedef void (*WordVecFn)(const char *token, float *out_vec, int dim);

static int encode_bag_bigram(const CnetVsaTokenList *tokens, float *out_vec, int dim,
                             WordVecFn wv, float uni_w, float bi_w) {
    double uni[CNET_VSA_DEFAULT_DIM];
    double bi[CNET_VSA_DEFAULT_DIM];
    memset(uni, 0, sizeof(double) * (size_t)dim);
    memset(bi, 0, sizeof(double) * (size_t)dim);
    float prev[CNET_VSA_DEFAULT_DIM];
    int have_prev = 0, n_content = 0;

    for (size_t i = 0; i < tokens->count; ++i) {
        if (cnet_vsa_text_is_stopword(tokens->tokens[i].token)) continue;
        float w[CNET_VSA_DEFAULT_DIM];
        wv(tokens->tokens[i].token, w, dim);
        for (int j = 0; j < dim; ++j) uni[j] += (double)w[j];
        if (bi_w > 0.0f && have_prev) {
            float rolled[CNET_VSA_DEFAULT_DIM], bound[CNET_VSA_DEFAULT_DIM];
            cnet_vsa_permute(rolled, prev, 1, dim);
            cnet_vsa_bind(bound, rolled, w, dim);
            for (int j = 0; j < dim; ++j) bi[j] += (double)bound[j];
        }
        if (bi_w > 0.0f) { memcpy(prev, w, sizeof(float) * (size_t)dim); have_prev = 1; }
        n_content++;
    }

    if (n_content == 0) {
        /* all stopwords: encode everything so the vector is never empty */
        for (size_t i = 0; i < tokens->count; ++i) {
            float w[CNET_VSA_DEFAULT_DIM];
            wv(tokens->tokens[i].token, w, dim);
            for (int j = 0; j < dim; ++j) uni[j] += (double)w[j];
        }
        for (int j = 0; j < dim; ++j) out_vec[j] = (float)uni[j];
        cnet_vsa_normalize(out_vec, dim);
        return 0;
    }
    if (n_content == 1 || bi_w <= 0.0f) {
        for (int j = 0; j < dim; ++j) out_vec[j] = (float)uni[j];
        cnet_vsa_normalize(out_vec, dim);
        return 0;
    }
    /* normalize each part so the mix weights mean what they say regardless of
     * sentence length (a raw sum of k unigrams grows like sqrt(k)) */
    double nu = 0.0, nb = 0.0;
    for (int j = 0; j < dim; ++j) { nu += uni[j] * uni[j]; nb += bi[j] * bi[j]; }
    nu = nu > 0.0 ? sqrt(nu) : 1.0;
    nb = nb > 0.0 ? sqrt(nb) : 1.0;
    for (int j = 0; j < dim; ++j) {
        out_vec[j] = (float)((double)uni_w * uni[j] / nu + (double)bi_w * bi[j] / nb);
    }
    cnet_vsa_normalize(out_vec, dim);
    return 0;
}

/* ---- wide int8 topical space ---------------------------------------------- */

void cnet_vsa_text_token_bits(const char *token, uint64_t *out_words) {
    if (!token || !out_words) return;
    uint64_t seed = fnv1a64(token);
    for (int i = 0; i < CNET_VSA_TOPICAL_WORDS; ++i) {
        out_words[i] = xorshift64(&seed);
    }
}

/* the word key every encoder uses in the wide space */
static void wide_word_key(const char *token, uint32_t encoder_id, char *out, size_t cap) {
    snprintf(out, cap, "%s", token);
    if (encoder_id == CNET_VSA_ENCODER_STEM || encoder_id == CNET_VSA_ENCODER_STEM_BI ||
        encoder_id == CNET_VSA_ENCODER_LEX) {
        cnet_vsa_text_stem(out);
    }
}

/* accumulate +-1 per bit into a 16-bit count array.
 * A byte -> eight signed lanes lookup keeps the inner loop a plain vector add
 * instead of a shift-and-branch per bit (the scalar form measured ~160 us per
 * 9-word query at 8192 bits; this form is an order of magnitude cheaper). */
static int8_t k_bit_lanes[256][8];
static int k_bit_lanes_ready = 0;

static void bit_lanes_init(void) {
    if (k_bit_lanes_ready) return;
    for (int v = 0; v < 256; ++v) {
        for (int k = 0; k < 8; ++k) k_bit_lanes[v][k] = (int8_t)(((v >> k) & 1) ? 1 : -1);
    }
    k_bit_lanes_ready = 1; /* idempotent: a racing init writes identical values */
}

static void wide_accumulate(int16_t *acc, const uint64_t *bits) {
    bit_lanes_init();
    for (int w = 0; w < CNET_VSA_TOPICAL_WORDS; ++w) {
        uint64_t b = bits[w];
        int16_t *a = acc + (w << 6);
        for (int j = 0; j < 8; ++j) {
            const int8_t *lanes = k_bit_lanes[(b >> (8 * j)) & 0xffu];
            int16_t *aj = a + (j << 3);
            for (int k = 0; k < 8; ++k) aj[k] = (int16_t)(aj[k] + lanes[k]);
        }
    }
}

static int wide_add_word(int16_t *acc, const char *token, uint32_t encoder_id, const CnetVsaLexicon *lex, uint64_t *prev_key) {
    char key[CNET_VSA_TOKEN_LEN];
    wide_word_key(token, encoder_id, key, sizeof(key));
    if (encoder_id == CNET_VSA_ENCODER_LEX) {
        /* learned vector when the word is known; otherwise its composition from
         * known subword n-grams, or its identity at the magnitude an
         * identity-only word has in the lexicon's scale */
        uint64_t k = cnet_vsa_lexicon_key_hash(key);
        const CnetVsaLexiconEntry *e = cnet_vsa_lexicon_find_hash(lex, k);
        if (e) {
            for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) acc[d] = (int16_t)(acc[d] + e->q8[d]);
        } else {
            int16_t oov[CNET_VSA_TOPICAL_DIM];
            cnet_vsa_lexicon_oov_vector(lex, key, oov);
            for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) acc[d] = (int16_t)(acc[d] + oov[d]);
        }
        /* a learned phrase for this word and the previous content word */
        if (lex->hdr.phrases && prev_key && *prev_key) {
            const CnetVsaLexiconEntry *p = cnet_vsa_lexicon_find_hash(lex, cnet_vsa_lexicon_phrase_key(*prev_key, k));
            if (p) for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) acc[d] = (int16_t)(acc[d] + p->q8[d]);
        }
        if (prev_key) *prev_key = k;
        return 1;
    }
    uint64_t bits[CNET_VSA_TOPICAL_WORDS];
    cnet_vsa_text_token_bits(key, bits);
    wide_accumulate(acc, bits);
    return 1;
}

static int wide_accumulate_tokens(const CnetVsaTokenList *tokens, uint32_t encoder_id, int16_t *acc) {
    if (!tokens || !acc || tokens->count == 0) return -1;
    if (!cnet_vsa_encoder_valid(encoder_id)) return -1;
    const CnetVsaLexicon *lex = NULL;
    if (encoder_id == CNET_VSA_ENCODER_LEX) {
        lex = cnet_vsa_lexicon_active();
        if (!lex || !lex->entries) return -1; /* LEX without its lexicon: refuse, never fall back silently */
    }
    memset(acc, 0, sizeof(int16_t) * CNET_VSA_TOPICAL_DIM);
    int n = 0;
    /* a query of 127+ full-magnitude lexicon words could wrap int16; the count
     * cap keeps the accumulator exact for any realistic text (a phrase entry
     * can add a second full-magnitude vector per word, hence the lower cap) */
    size_t limit = tokens->count < 250 ? tokens->count : 250;
    if (lex && lex->hdr.phrases && limit > 125) limit = 125;
    uint64_t prev_key = 0;
    for (size_t i = 0; i < limit; ++i) {
        if (cnet_vsa_text_is_stopword(tokens->tokens[i].token)) continue;
        n += wide_add_word(acc, tokens->tokens[i].token, encoder_id, lex, &prev_key);
    }
    if (n == 0) {
        prev_key = 0;
        for (size_t i = 0; i < limit; ++i) {
            n += wide_add_word(acc, tokens->tokens[i].token, encoder_id, lex, &prev_key);
        }
    }
    return n;
}

int cnet_vsa_text_encode_topical_wide(const CnetVsaTokenList *tokens, float *out_wide,
                                      uint32_t encoder_id) {
    if (!out_wide) return -1;
    int16_t acc[CNET_VSA_TOPICAL_DIM];
    int n = wide_accumulate_tokens(tokens, encoder_id, acc);
    if (n <= 0) return -1;
    double nn = 0.0;
    for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) nn += (double)acc[d] * (double)acc[d];
    nn = nn > 0.0 ? sqrt(nn) : 1.0;
    for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) out_wide[d] = (float)((double)acc[d] / nn);
    return 0;
}

static int wide_acc_to_q8(const int16_t *acc, int8_t *out_q8) {
    /* hash encoders: counts are exact in int8 below 128 content words. Lexicon
     * vectors carry magnitudes up to 127 each, so their sums are rescaled by
     * the max-abs to the int8 range (cosine is scale-free); the hash path is
     * untouched unless a text exceeds 127 content words. */
    int mx = 0;
    for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) { int a = acc[d] < 0 ? -acc[d] : acc[d]; if (a > mx) mx = a; }
    if (mx <= 127) {
        for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) out_q8[d] = (int8_t)acc[d];
    } else {
        float sc = 127.0f / (float)mx;
        for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) {
            float v = (float)acc[d] * sc;
            int q = (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
            out_q8[d] = (int8_t)(q > 127 ? 127 : (q < -127 ? -127 : q));
        }
    }
    return 0;
}

int cnet_vsa_text_encode_topical_q8(const CnetVsaTokenList *tokens, int8_t *out_q8,
                                    uint32_t encoder_id) {
    if (!out_q8) return -1;
    int16_t acc[CNET_VSA_TOPICAL_DIM];
    if (wide_accumulate_tokens(tokens, encoder_id, acc) <= 0) return -1;
    return wide_acc_to_q8(acc, out_q8);
}

int cnet_vsa_text_leave_one_out_distances(const CnetVsaTokenList *tokens, uint32_t encoder_id,
    const int8_t *target, float target_norm, float radius, float *distances, size_t capacity) {
    if (!tokens || !target || !distances || !cnet_vsa_encoder_valid(encoder_id) ||
        !isfinite(target_norm) || target_norm <= 0 || !isfinite(radius)) return -1;
    if (tokens->count > 250) return 0; /* removal can change the token-cap boundary */
    const CnetVsaLexicon *lex = encoder_id == CNET_VSA_ENCODER_LEX ? cnet_vsa_lexicon_active() : NULL;
    if (encoder_id == CNET_VSA_ENCODER_LEX && (!lex || !lex->entries)) return -1;
    if (lex && lex->hdr.phrases && tokens->count > 125) return 0;
    size_t nc = 0;
    for (size_t i = 0; i < tokens->count; ++i) nc += !cnet_vsa_text_is_stopword(tokens->tokens[i].token);
    if (nc < 2) return 0; /* preserve the encoder's all-stopword fallback */
    if (capacity < nc) return -1;
    int16_t sum[CNET_VSA_TOPICAL_DIM], word[CNET_VSA_TOPICAL_DIM], residual[CNET_VSA_TOPICAL_DIM];
    if (wide_accumulate_tokens(tokens, encoder_id, sum) <= 0) return -1;
    uint64_t keys[CNET_VSA_MAX_TOKENS]; size_t nk = 0;
    if (lex && lex->hdr.phrases) {
        for (size_t i = 0; i < tokens->count; ++i) {
            if (cnet_vsa_text_is_stopword(tokens->tokens[i].token)) continue;
            char key[CNET_VSA_TOKEN_LEN];
            wide_word_key(tokens->tokens[i].token, encoder_id, key, sizeof(key));
            keys[nk++] = cnet_vsa_lexicon_key_hash(key);
        }
    }
    int8_t q[CNET_VSA_TOPICAL_DIM]; int measured = 0;
    for (size_t i = 0; i < tokens->count; ++i) {
        if (cnet_vsa_text_is_stopword(tokens->tokens[i].token)) continue;
        memset(word, 0, sizeof(word));
        wide_add_word(word, tokens->tokens[i].token, encoder_id, lex, NULL);
        for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) residual[d] = (int16_t)(sum[d] - word[d]);
        if (lex && lex->hdr.phrases) {
            size_t k = (size_t)measured;
            const CnetVsaLexiconEntry *left = k ? cnet_vsa_lexicon_find_hash(lex, cnet_vsa_lexicon_phrase_key(keys[k-1], keys[k])) : NULL;
            const CnetVsaLexiconEntry *right = k+1 < nk ? cnet_vsa_lexicon_find_hash(lex, cnet_vsa_lexicon_phrase_key(keys[k], keys[k+1])) : NULL;
            const CnetVsaLexiconEntry *bridge = k && k+1 < nk ? cnet_vsa_lexicon_find_hash(lex, cnet_vsa_lexicon_phrase_key(keys[k-1], keys[k+1])) : NULL;
            if (left) for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) residual[d] = (int16_t)(residual[d] - left->q8[d]);
            if (right) for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) residual[d] = (int16_t)(residual[d] - right->q8[d]);
            if (bridge) for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) residual[d] = (int16_t)(residual[d] + bridge->q8[d]);
        }
        wide_acc_to_q8(residual, q);
        float distance = 1.0f - cnet_vsa_text_q8_similarity_n(q, cnet_vsa_text_q8_norm(q), target, target_norm);
        distances[measured++] = distance;
        if (distance > radius) break;
    }
    return measured;
}

void cnet_vsa_text_wide_to_q8(const float *wide, int8_t *out_q8) {
    if (!wide || !out_q8) return;
    float mx = 0.0f;
    for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) { float a = fabsf(wide[d]); if (a > mx) mx = a; }
    float sc = mx > 0.0f ? 127.0f / mx : 0.0f;
    for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) {
        float v = wide[d] * sc;
        int q = (int)(v >= 0.0f ? v + 0.5f : v - 0.5f);
        out_q8[d] = (int8_t)(q > 127 ? 127 : (q < -127 ? -127 : q));
    }
}

float cnet_vsa_text_q8_norm(const int8_t *v) {
    if (!v) return 0.0f;
    /* Even INT8_MIN in every lane totals only 33,554,432 at width 2048.
     * A 32-bit reduction stays exact and avoids widening every product. */
    _Static_assert(CNET_VSA_TOPICAL_DIM <= INT32_MAX / (128 * 128), "q8 norm must fit int32");
    int32_t s = 0;
    for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) s += (int32_t)v[d] * (int32_t)v[d];
    return (float)sqrt((double)s);
}

float cnet_vsa_text_q8_similarity_n(const int8_t *a, float norm_a, const int8_t *b, float norm_b) {
    if (!a || !b || norm_a <= 0.0f || norm_b <= 0.0f) return 0.0f;
    int32_t dot = 0;
#if defined(__AVX512VNNI__) && defined(__AVX512BW__)
    /* VNNI multiplies unsigned by signed bytes. Bias a by 128, then remove
     * 128*sum(b); this is exact even for INT8_MIN (no saturating products).
     * Keep the scalar path on hosts/builds without these instructions. */
    _Static_assert(CNET_VSA_TOPICAL_DIM % 64 == 0, "VNNI requires complete lanes");
    _Static_assert(CNET_VSA_TOPICAL_DIM <= INT32_MAX / (255 * 128), "VNNI accumulator bound");
    __m512i products = _mm512_setzero_si512(), sum_b = _mm512_setzero_si512();
    const __m512i bias = _mm512_set1_epi8((char)128), ones = _mm512_set1_epi8(1);
    for (int d = 0; d < CNET_VSA_TOPICAL_DIM; d += 64) {
        __m512i av = _mm512_xor_si512(_mm512_loadu_si512(a + d), bias);
        __m512i bv = _mm512_loadu_si512(b + d);
        products = _mm512_dpbusd_epi32(products, av, bv);
        sum_b = _mm512_dpbusd_epi32(sum_b, ones, bv);
    }
    dot = _mm512_reduce_add_epi32(products) - 128 * _mm512_reduce_add_epi32(sum_b);
#else
    for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) dot += (int32_t)a[d] * (int32_t)b[d];
#endif
    return (float)dot / (norm_a * norm_b);
}

float cnet_vsa_text_q8_similarity(const int8_t *a, const int8_t *b) {
    return cnet_vsa_text_q8_similarity_n(a, cnet_vsa_text_q8_norm(a), b, cnet_vsa_text_q8_norm(b));
}

int cnet_vsa_text_encode_topical_ex(const CnetVsaTokenList *tokens, float *out_topical_vec,
                                    int dim, uint32_t encoder_id) {
    if (encoder_id == CNET_VSA_ENCODER_BAG) {
        return cnet_vsa_text_encode_topical(tokens, out_topical_vec, dim);
    }
    if (!cnet_vsa_encoder_valid(encoder_id)) return -1;
    if (!tokens || !out_topical_vec || dim <= 0 || dim > CNET_VSA_DEFAULT_DIM || tokens->count == 0) {
        return -1;
    }
    if (encoder_id == CNET_VSA_ENCODER_STEM || encoder_id == CNET_VSA_ENCODER_LEX) {
        return encode_bag_bigram(tokens, out_topical_vec, dim, token_vec_stem, 1.0f, 0.0f);
    }
    if (encoder_id == CNET_VSA_ENCODER_STEM_BI) {
        return encode_bag_bigram(tokens, out_topical_vec, dim, token_vec_stem, 0.80f, 0.20f);
    }
    if (encoder_id == CNET_VSA_ENCODER_CGRAM_C) {
        return encode_bag_bigram(tokens, out_topical_vec, dim, token_vec_chargram_centered, 1.0f, 0.0f);
    }
    /* CNET_VSA_ENCODER_HD: original experimental encoder, kept verbatim below */

    double uni[CNET_VSA_DEFAULT_DIM];
    double bi[CNET_VSA_DEFAULT_DIM];
    memset(uni, 0, sizeof(double) * (size_t)dim);
    memset(bi, 0, sizeof(double) * (size_t)dim);

    float prev[CNET_VSA_DEFAULT_DIM];
    int have_prev = 0;
    int n_content = 0;

    for (size_t i = 0; i < tokens->count; ++i) {
        if (cnet_vsa_text_is_stopword(tokens->tokens[i].token)) continue;
        float w[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_token_vec_chargram(tokens->tokens[i].token, w, dim);
        for (int j = 0; j < dim; ++j) uni[j] += (double)w[j];
        if (have_prev) {
            float rolled[CNET_VSA_DEFAULT_DIM];
            float bound[CNET_VSA_DEFAULT_DIM];
            cnet_vsa_permute(rolled, prev, 1, dim);
            cnet_vsa_bind(bound, rolled, w, dim);
            for (int j = 0; j < dim; ++j) bi[j] += (double)bound[j];
        }
        memcpy(prev, w, sizeof(float) * (size_t)dim);
        have_prev = 1;
        n_content++;
    }

    if (n_content == 0) {
        for (size_t i = 0; i < tokens->count; ++i) {
            float w[CNET_VSA_DEFAULT_DIM];
            cnet_vsa_text_token_vec_chargram(tokens->tokens[i].token, w, dim);
            for (int j = 0; j < dim; ++j) uni[j] += (double)w[j];
        }
        for (int j = 0; j < dim; ++j) out_topical_vec[j] = (float)uni[j];
        cnet_vsa_normalize(out_topical_vec, dim);
        return 0;
    }

    if (n_content == 1) {
        for (int j = 0; j < dim; ++j) out_topical_vec[j] = (float)uni[j];
        cnet_vsa_normalize(out_topical_vec, dim);
        return 0;
    }

    /* 0.40 unigram + 0.60 bound bigram: shared words still pull domains
     * together, collocations split neighbors the bag encoder cannot. */
    for (int j = 0; j < dim; ++j) {
        out_topical_vec[j] = (float)(0.40 * uni[j] + 0.60 * bi[j]);
    }
    cnet_vsa_normalize(out_topical_vec, dim);
    return 0;
}

float cnet_vsa_text_topical_similarity(const char *text_a, const char *text_b, int dim) {
    if (!text_a || !text_b || dim <= 0) return 0.0f;

    CnetVsaTokenList list_a, list_b;
    if (cnet_vsa_text_tokenize(text_a, &list_a) <= 0) return 0.0f;
    if (cnet_vsa_text_tokenize(text_b, &list_b) <= 0) return 0.0f;

    float vec_a[CNET_VSA_DEFAULT_DIM];
    float vec_b[CNET_VSA_DEFAULT_DIM];

    if (cnet_vsa_text_encode_topical(&list_a, vec_a, dim) != 0) return 0.0f;
    if (cnet_vsa_text_encode_topical(&list_b, vec_b, dim) != 0) return 0.0f;

    return cnet_vsa_similarity(vec_a, vec_b, dim);
}

