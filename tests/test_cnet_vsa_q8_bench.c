/* Exact int8 arithmetic, query isolation, and repeatable route timings.
 * --wrap pauses one route after encoding so a second route can overwrite any
 * shared query buffer. This tests overlap deterministically, without sleeps. */
#define _POSIX_C_SOURCE 200809L
#include "cnet_vsa_gen_capsule.h"
#include "cnet_vsa_lexicon.h"
#include "cnet_vsa_text.h"
#include <assert.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *queries[] = {
    "wavefront lds shared memory coalescing hip execution",
    "baking sourdough bread yeast flour fermentation oven",
    "elliptic curve cryptography aes encryption private keys",
    "linux kernel inodes dentries epoll virtual filesystem",
    "compiler llvm ssa dead code elimination vectorization",
    "silk velvet dress tailoring draping fashion textiles",
    "the and of to in", "embedding embedded embeds running runs"
};
#define N_QUERY (sizeof(queries) / sizeof(queries[0]))
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int phase;
static _Thread_local int role = -1;
float __real_cnet_vsa_text_q8_norm(const int8_t *v);
float __wrap_cnet_vsa_text_q8_norm(const int8_t *v) {
    float n = __real_cnet_vsa_text_q8_norm(v);
    if (role < 0) return n;
    assert(pthread_mutex_lock(&lock) == 0);
    if (role == 0) {
        phase = 1;
        assert(pthread_cond_broadcast(&changed) == 0);
        while (phase < 2) assert(pthread_cond_wait(&changed, &lock) == 0);
    } else {
        phase = 2;
        assert(pthread_cond_broadcast(&changed) == 0);
        while (phase < 3) assert(pthread_cond_wait(&changed, &lock) == 0);
    }
    assert(pthread_mutex_unlock(&lock) == 0);
    role = -1; /* only pause the first norm, even with multiple encoders */
    return n;
}

static void check_arithmetic(void) {
    int8_t v[CNET_VSA_TOPICAL_DIM], other[CNET_VSA_TOPICAL_DIM];
    uint32_t rng = 7;
    assert(cnet_vsa_text_q8_norm(NULL) == 0.0f);
    for (int sample = 0; sample < 1256; ++sample) {
        int64_t squared = 0, dot = 0;
        for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            v[d] = (int8_t)(sample < 256 ? sample - 128 : (int)(rng & 255) - 128);
            squared += (int64_t)v[d] * v[d];
            other[d] = (int8_t)(d + sample);
            dot += (int64_t)v[d] * other[d];
        }
        float expected = (float)sqrt((double)squared);
        assert(cnet_vsa_text_q8_norm(v) == expected);
        assert(cnet_vsa_text_q8_similarity_n(v, 4000.f, other, 4000.f) == (float)dot / (4000.f * 4000.f));
    }
    /* Wide encoders have exactly two representations, including fallback on
     * all-stopword input. This equivalence is required by shared encoding. */
    for (size_t i = 0; i < N_QUERY; ++i) {
        int8_t q[CNET_VSA_ENCODER_COUNT][CNET_VSA_TOPICAL_DIM];
        for (uint32_t e = 0; e < CNET_VSA_ENCODER_COUNT; ++e) {
            if (e == CNET_VSA_ENCODER_LEX && !cnet_vsa_lexicon_active()) continue; /* needs its lexicon */
            assert(cnet_vsa_gencap_encode_intent_q8(queries[i], q[e], e) == 0);
        }
        assert(memcmp(q[0], q[1], sizeof q[0]) == 0);
        assert(memcmp(q[0], q[4], sizeof q[0]) == 0);
        assert(memcmp(q[2], q[3], sizeof q[0]) == 0);
    }
}

static CnetVsaGenRegistry *make_registry(int count, int mixed) {
    CnetVsaGenRegistry *reg = calloc(1, sizeof(*reg));
    assert(reg && cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM) == 0);
    reg->count = (size_t)count;
    for (int i = 0; i < count; ++i) {
        CnetVsaRegisteredCap *c = &reg->capsules[i];
        c->header.certified = 1;
        c->header.safe_radius = c->topical_radius = 0.9f;
        uint32_t n_enc = cnet_vsa_lexicon_active() ? CNET_VSA_ENCODER_COUNT : CNET_VSA_ENCODER_COUNT - 1u; /* LEX is last */
        c->encoder_id = mixed ? (uint32_t)i % n_enc : CNET_VSA_ENCODER_STEM;
        c->has_topical = 1;
        char text[512];
        snprintf(text, sizeof text, "%s domain%d", queries[(size_t)i % N_QUERY], i);
        assert(cnet_vsa_gencap_encode_intent_q8(text, c->topical, c->encoder_id) == 0);
        c->topical_norm = cnet_vsa_text_q8_norm(c->topical);
        assert(cnet_vsa_gencap_encode_intent_ex(text, c->header.centroid, reg->dim, c->encoder_id) == 0);
    }
    return reg;
}

static int same_route(const CnetVsaRouteResult *a, const CnetVsaRouteResult *b) {
#define SAME(f) if (a->f != b->f) return 0
    SAME(space); SAME(status); SAME(best_idx); SAME(best_sim); SAME(best_dist);
    SAME(radius); SAME(second_idx); SAME(second_sim); SAME(gap); SAME(null_mean);
    SAME(null_std); SAME(z); SAME(z_min); SAME(radius_ok); SAME(margin_ok);
    SAME(margin_checked); SAME(ambiguous); SAME(ambiguity_ok);
#undef SAME
    return 1;
}

typedef struct {
    const CnetVsaGenRegistry *reg;
    int id;
    CnetVsaRouteResult result;
} Worker;
static void *route_worker(void *ptr) {
    Worker *w = ptr;
    if (w->id == 1) {
        assert(pthread_mutex_lock(&lock) == 0);
        while (phase < 1) assert(pthread_cond_wait(&changed, &lock) == 0);
        assert(pthread_mutex_unlock(&lock) == 0);
    }
    role = w->id;
    cnet_vsa_registry_route_query(w->reg, queries[w->id], &w->result);
    if (w->id == 0) {
        assert(pthread_mutex_lock(&lock) == 0);
        phase = 3;
        assert(pthread_cond_broadcast(&changed) == 0);
        assert(pthread_mutex_unlock(&lock) == 0);
    }
    return NULL;
}

static int check_overlap(const CnetVsaGenRegistry *reg) {
    CnetVsaRouteResult serial[2];
    Worker w[2] = {{.reg = reg, .id = 0}, {.reg = reg, .id = 1}};
    pthread_t threads[2];
    for (int i = 0; i < 2; ++i)
        cnet_vsa_registry_route_query(reg, queries[i], &serial[i]);
    phase = 0;
    for (int i = 0; i < 2; ++i) assert(pthread_create(&threads[i], NULL, route_worker, &w[i]) == 0);
    for (int i = 0; i < 2; ++i) assert(pthread_join(threads[i], NULL) == 0);
    return same_route(&serial[0], &w[0].result) && same_route(&serial[1], &w[1].result);
}

static double now_us(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}
static volatile float sink;
static void benchmark(CnetVsaGenRegistry *reg, const char *label) {
    CnetVsaRouteResult r;
    for (int i = 0; i < 256; ++i) cnet_vsa_registry_route_query(reg, queries[(size_t)i % N_QUERY], &r);
    double t = now_us();
    for (int i = 0; i < 12000; ++i) {
        cnet_vsa_registry_route_query(reg, queries[(size_t)i % N_QUERY], &r);
        sink = r.best_sim;
    }
    printf("Q8_TIMING %s capsules=%zu us/query=%.6f\n", label, reg->count, (now_us() - t) / 12000.0);
    for (size_t i = 0; i < N_QUERY; ++i) {
        cnet_vsa_registry_route_query(reg, queries[i], &r);
        printf("Q8_ROUTE %s %zu %d %d %d %a %a %a %a %a\n", label, i, r.space, r.status,
               r.best_idx, (double)r.best_sim, (double)r.second_sim, (double)r.null_std,
               (double)r.z, (double)r.gap);
    }
}
static int cmp_entry_key(const void *a, const void *b) {
    uint64_t x = ((const CnetVsaLexiconEntry *)a)->key, y = ((const CnetVsaLexiconEntry *)b)->key;
    return (x > y) - (x < y);
}

static void check_leave_one_out(void) {
    const char *words[] = {"alpha", "beta", "gamma", "delta"};
    char boundary[6][2048];
    const int lengths[] = {124, 125, 126, 249, 250, 251};
    for (int j = 0; j < 6; ++j) {
        boundary[j][0] = 0;
        for (int i = 0; i < lengths[j]; ++i)
            strcat(boundary[j], i % 5 == 3 ? "the " : (i % 2 ? "alpha " : "beta "));
    }
    const char *texts[] = {"alpha beta gamma delta", "the alpha and beta gamma", "alpha beta alpha gamma", "delta gamma beta alpha",
        "alpha beta", "delta delta", "alpha quux beta", "alpha zephyr beta",
        boundary[0], boundary[1], boundary[2], boundary[3], boundary[4], boundary[5]};
    CnetVsaLexicon lex; memset(&lex, 0, sizeof(lex));
    lex.hdr.count = 7; lex.hdr.phrases = 3; lex.hdr.dim = CNET_VSA_TOPICAL_DIM;
    lex.entries = calloc(7, sizeof(*lex.entries)); lex.keys = calloc(7, sizeof(*lex.keys));
    assert(lex.entries && lex.keys);
    for (int i = 0; i < 4; ++i) lex.entries[i].key = cnet_vsa_lexicon_word_key(words[i]);
    lex.entries[4].key = cnet_vsa_lexicon_phrase_key(lex.entries[0].key, lex.entries[1].key);
    lex.entries[5].key = cnet_vsa_lexicon_phrase_key(lex.entries[1].key, lex.entries[2].key);
    lex.entries[6].key = cnet_vsa_lexicon_phrase_key(lex.entries[0].key, lex.entries[2].key);
    uint32_t rng = 17;
    for (int i = 0; i < 7; ++i) for (int d = 0; d < CNET_VSA_TOPICAL_DIM; ++d) {
        rng = rng * 1664525u + 1013904223u; lex.entries[i].q8[d] = (int8_t)((rng >> 24) % 255 - 127);
    }
    memset(lex.entries[3].q8, 0, sizeof(lex.entries[3].q8)); /* zero residual */
    lex.hdr.fallback_mag = 127;
    lex.hdr.subwords = 1; lex.hdr.subword_min_n = 3; lex.hdr.subword_max_n = 3;
    lex.hdr.subword_weight = 1.f;
    lex.subwords = calloc(1, sizeof(*lex.subwords)); lex.skeys = calloc(1, sizeof(*lex.skeys));
    assert(lex.subwords && lex.skeys);
    uint64_t subkeys[128];
    assert(cnet_vsa_lexicon_subword_keys("zephyr", 3, 3, subkeys, 128) > 0);
    lex.skeys[0] = lex.subwords[0].key = subkeys[0]; lex.subwords[0].mag = 127;
    for (int d = 0; d < CNET_VSA_TOPICAL_WORDS; ++d) lex.subwords[0].bits[d] = UINT64_C(0xa5a5a5a5a5a5a5a5);
    int16_t oov[CNET_VSA_TOPICAL_DIM];
    assert(cnet_vsa_lexicon_oov_vector(&lex, "zephyr", oov) == 1);
    assert(cnet_vsa_lexicon_oov_vector(&lex, "quux", oov) == 0);
    qsort(lex.entries, 7, sizeof(*lex.entries), cmp_entry_key);
    for (int i = 0; i < 7; ++i) lex.keys[i] = lex.entries[i].key;
    cnet_vsa_lexicon_set_active(&lex);
    const uint32_t encoders[] = {CNET_VSA_ENCODER_BAG, CNET_VSA_ENCODER_STEM, CNET_VSA_ENCODER_LEX};
    for (size_t ei = 0; ei < sizeof(encoders)/sizeof(encoders[0]); ++ei) {
        uint32_t enc = encoders[ei]; int8_t target[CNET_VSA_TOPICAL_DIM];
        assert(cnet_vsa_gencap_encode_intent_q8(texts[0], target, enc) == 0);
        float tn = cnet_vsa_text_q8_norm(target);
        for (size_t qi = 0; qi < sizeof(texts)/sizeof(texts[0]); ++qi) {
            CnetVsaTokenList tl; assert(cnet_vsa_text_tokenize(texts[qi], &tl) > 0);
            float distances[CNET_VSA_MAX_TOKENS];
            if (tl.count > (enc == CNET_VSA_ENCODER_LEX ? 125u : 250u)) {
                assert(cnet_vsa_text_leave_one_out_distances(&tl, enc, target, tn, 2.f, distances, CNET_VSA_MAX_TOKENS) == 0);
                continue; /* caller must retain the legacy re-encoding path */
            }
            assert(cnet_vsa_text_leave_one_out_distances(&tl, enc, target, tn, 2.f, distances, 1) == -1);
            for (int mode = 0; mode < 2; ++mode) {
                float radius = mode ? .1f : 2.f;
                int got = cnet_vsa_text_leave_one_out_distances(&tl, enc, target, tn, radius, distances, CNET_VSA_MAX_TOKENS), k = 0;
                assert(got > 0);
                for (size_t removed = 0; removed < tl.count; ++removed) {
                    if (cnet_vsa_text_is_stopword(tl.tokens[removed].token)) continue;
                    char text[8192]; size_t pos = 0;
                    for (size_t i = 0; i < tl.count; ++i) if (i != removed) {
                        size_t len = strlen(tl.tokens[i].token); memcpy(text + pos, tl.tokens[i].token, len); pos += len; text[pos++] = ' ';
                    }
                    text[pos] = 0; int8_t q[CNET_VSA_TOPICAL_DIM];
                    assert(cnet_vsa_gencap_encode_intent_q8(text, q, enc) == 0);
                    float d = 1.f - cnet_vsa_text_q8_similarity_n(q, cnet_vsa_text_q8_norm(q), target, tn);
                    assert(k < got && distances[k++] == d);
                    if (d > radius) break;
                }
                assert(k == got);
            }
        }
    }
    cnet_vsa_lexicon_set_active(NULL); free(lex.entries); free(lex.keys); free(lex.subwords); free(lex.skeys);
    puts("CNET_VSA_Q8_LOO_PASS: exact deletion, phrase bridge, OOV/subwords, zero residual, token boundaries, early refusal");
}

int main(int argc, char **argv) {
    int timing_only = argc == 2 && strcmp(argv[1], "--timing-only") == 0;
    check_arithmetic();
    check_leave_one_out();
    CnetVsaGenRegistry *reg = make_registry(101, 0);
    if (!timing_only && !check_overlap(reg)) {
        fprintf(stderr, "CNET_VSA_Q8_BENCH_RED: overlapping routes changed a query's result\n");
        free(reg);
        return 1;
    }
    double t = now_us();
    for (int i = 0; i < 1000000; ++i) sink = cnet_vsa_text_q8_norm(reg->capsules[(unsigned)i % 101].topical);
    printf("Q8_TIMING norm ns/vector=%.6f\n", (now_us() - t) / 1000.0);
    benchmark(reg, "wide_stem");
    reg->force_float = 1;
    benchmark(reg, "float_stem");
    free(reg);
    reg = make_registry(101, 1);
    if (!timing_only && !check_overlap(reg)) {
        fprintf(stderr, "CNET_VSA_Q8_BENCH_RED: overlapping mixed-encoder routes changed a query's result\n");
        free(reg);
        return 1;
    }
    benchmark(reg, "wide_mixed");
    free(reg);
    puts(timing_only ? "CNET_VSA_Q8_TIMING_DONE: overlap test skipped" :
         "CNET_VSA_Q8_BENCH_PASS: exact norms, encoder equivalence, and isolated overlapping queries");
    return 0;
}
