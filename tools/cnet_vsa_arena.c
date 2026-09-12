/* CNET side of the frozen routing arena (benchmarks/vsa_routing_arena_*).
 *
 * Reads the frozen fixture (corpora.tsv: name, role, sentence; role=test rows
 * are the held-out queries and never enter a centroid, a lexicon or a
 * calibration), builds the lexicon from the train rows only, and scores each
 * encoder with the PRODUCTION representation and functions: int8-2048 centroid
 * quantised from the wide sum (cnet_vsa_text_wide_to_q8), int8 query vectors
 * (cnet_vsa_gencap_encode_intent_q8), cosine via cnet_vsa_text_q8_similarity_n.
 *
 * Metrics (identical to tools/cnet_vsa_arena_transformer.py):
 *   test_sentences  nearest corpus top-1 / top-3 / mean runner-up margin
 *   questions       same for questions.tsv when frozen (teacher-written)
 *   separability    leave-one-out vs 512 seeded negatives at three policies
 *   aliens          best cosine and z over the centroid population
 *   encode_us       per-query encode latency
 * Writes results_cnet.json. No thresholds live here; tools/cnet_vsa_arena_check.py
 * compares against expected.json and the manifest hashes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "cnet_vsa.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_lexicon.h"
#include "cnet_vsa_gen_capsule.h"

#define MAXC   4096
#define MAXL   512
#define MAXQ   16
#define NNEG   512
#define XMAX   48
#define XSETS  8
#define DIM    CNET_VSA_TOPICAL_DIM

typedef struct {
    char name[128];
    char *train[MAXL]; int n_train;
    char *test[MAXQ];  int n_test;
    char *q[MAXQ];     int n_q;
    char *xq[XMAX];    int n_xq; int xset[XMAX];   /* extra questions (written after the model freeze), per set, scored as their own blocks */
    int xalt[XMAX][4]; int n_xalt[XMAX];            /* per-question alternate valid targets ("gold|alt|alt" in the name column) */
    int alt[8]; int n_alt;         /* alternate valid targets (justified duplicates), for lenient top-1 */
} Corpus;

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }
static uint64_t xs(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *s = x; return x; }
static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return (x > y) - (x < y); }
static int separable(const float *d_in, size_t n, const float *d_neg, size_t m, float ti, float tn) {
    size_t k = (size_t)ceil(ti * (double)n - 1e-9); if (k == 0) k = 1; if (k > n) k = n;
    size_t j = (size_t)floor((1.0 - tn) * (double)m + 1e-9); if (j >= m) j = m - 1;
    return d_in[k - 1] <= d_neg[j] - 1e-4f;
}
static uint64_t fnv_file(const char *path) {
    FILE *fp = fopen(path, "rb"); if (!fp) return 0;
    uint64_t h = 14695981039346656037ULL; unsigned char buf[65536]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) for (size_t i = 0; i < n; ++i) { h ^= buf[i]; h *= 1099511628211ULL; }
    fclose(fp); return h;
}

static int find_corpus(Corpus *c, int nc, const char *name) {
    for (int i = 0; i < nc; ++i) if (strcmp(c[i].name, name) == 0) return i;
    return -1;
}

typedef struct { double top1, top3, margin, lenient1; int n; } TopK;   /* lenient1: top-1 in {gold} + its alternates */

typedef struct { const char *const *texts; const char *kind; const char *encoder; const Corpus *c; FILE *f; } DumpCtx;

static int count_oov(const char *text, uint32_t enc) {
    if (enc != CNET_VSA_ENCODER_LEX || !cnet_vsa_lexicon_active()) return 0;
    CnetVsaTokenList tl; if (cnet_vsa_text_tokenize(text, &tl) <= 0) return 0;
    int oov = 0;
    for (size_t i = 0; i < tl.count; ++i) {
        if (cnet_vsa_text_is_stopword(tl.tokens[i].token)) continue;
        if (!cnet_vsa_lexicon_find_hash(cnet_vsa_lexicon_active(), cnet_vsa_lexicon_word_key(tl.tokens[i].token))) oov++;
    }
    return oov;
}
static int count_content(const char *text) {
    CnetVsaTokenList tl; if (cnet_vsa_text_tokenize(text, &tl) <= 0) return 0;
    int n = 0; for (size_t i = 0; i < tl.count; ++i) if (!cnet_vsa_text_is_stopword(tl.tokens[i].token)) n++;
    return n;
}

static TopK score_topk(const int8_t *cent, const float *cnorm, int nc, const int8_t *queries, const float *qnorm,
                       const int *gold, int nq, const DumpCtx *dc, uint32_t enc, int (*qalt)[4], const int *nqalt) {
    TopK r = {0, 0, 0, 0, nq};
    int hit1 = 0, hit3 = 0, hitl = 0; double msum = 0;
    for (int i = 0; i < nq; ++i) {
        float b1 = -2, b2 = -2, b3 = -2, sg = -2; int i1 = -1, i2 = -1, i3 = -1;
        for (int c = 0; c < nc; ++c) {
            float s = cnet_vsa_text_q8_similarity_n(queries + (size_t)i * DIM, qnorm[i], cent + (size_t)c * DIM, cnorm[c]);
            if (c == gold[i]) sg = s;
            if (s > b1) { b3 = b2; i3 = i2; b2 = b1; i2 = i1; b1 = s; i1 = c; }
            else if (s > b2) { b3 = b2; i3 = i2; b2 = s; i2 = c; }
            else if (s > b3) { b3 = s; i3 = c; }
        }
        if (i1 == gold[i]) hit1++;
        if (i1 == gold[i] || i2 == gold[i] || i3 == gold[i]) hit3++;
        if (dc) { int ok = i1 == gold[i]; const Corpus *gc = &dc->c[gold[i]];
          for (int a = 0; a < gc->n_alt && !ok; ++a) if (gc->alt[a] == i1) ok = 1;
          if (qalt && nqalt) for (int a = 0; a < nqalt[i] && !ok; ++a) if (qalt[i][a] == i1) ok = 1;
          hitl += ok; } else hitl += i1 == gold[i];
        msum += b1 - b2;
        if (dc && dc->f) {
            fprintf(dc->f, "%s\t%s\t%s\t%s\t%s\t%.4f\t%.4f\t%.4f\t%d\t%d\t%s\n", dc->encoder, dc->kind, dc->c[gold[i]].name,
                    i1 >= 0 ? dc->c[i1].name : "-", i2 >= 0 ? dc->c[i2].name : "-", b1, b2, sg,
                    count_content(dc->texts[i]), count_oov(dc->texts[i], enc), dc->texts[i]);
        }
    }
    r.top1 = (double)hit1 / nq; r.top3 = (double)hit3 / nq; r.margin = msum / nq; r.lenient1 = (double)hitl / nq;
    return r;
}

int main(int argc, char **argv) {
    const char *fixture = "benchmarks/vsa_routing_arena_20260911";
    const char *out = NULL;
    const char *cache = "var/arena_cache/cnet";
    float beta = 0.5f; unsigned pcs = 4;
    const char *distilled = NULL; float dalpha = 0.5f; const char *dump = NULL;
    float idf_floor = 0.25f, idf_power = 1.0f;
    float dbeta = 1.0f; unsigned dpcs = 16;   /* the distilled lexicon has its own context weight and PC removal */
    unsigned phrases = 0, subwords = 0;       /* lex_full row: phrases + subword backoff on top of the best lexicon */
    unsigned sub_min_n = 3, sub_max_n = 5, sub_min_words = 4, sub_max_words = 0; float sub_weight = 1.0f, phrase_weight = 1.0f;
    const char *train_q = NULL; unsigned train_epochs = 8; float train_lr = 0.05f, train_margin = 0.10f; unsigned train_sent = 0;
    const char *vocab_dump = NULL;
    const char *extra_files[XSETS]; int n_extra = 0;   /* name<TAB>question sets written after the freeze; never trained on */
    const char *alternates = NULL;             /* capsule<TAB>alternate[<TAB>why]: justified multiple valid targets (lenient top-1) */
    unsigned full_min_count = 0, full_max_vocab = 0;   /* vocabulary rule for the lex_full build only (0 = the shared default) */
    const char *lexicon_extra = NULL;          /* name<TAB>text: training-register questions appended to each corpus for the lex_full BUILD only
                                                  (vocabulary and phrase coverage of everyday phrasing); the trainer still pairs pure corpus sentences */
    const char *neighbors = NULL;              /* write each corpus's nearest centroids per row (to build alternates) */
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fixture")) fixture = argv[i + 1];
        else if (!strcmp(argv[i], "--out")) out = argv[i + 1];
        else if (!strcmp(argv[i], "--cache")) cache = argv[i + 1];
        else if (!strcmp(argv[i], "--beta")) beta = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--remove-pcs")) pcs = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--distilled")) distilled = argv[i + 1];
        else if (!strcmp(argv[i], "--distill-alpha")) dalpha = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--dump")) dump = argv[i + 1];   /* per-query TSV for error analysis */
        else if (!strcmp(argv[i], "--distill-beta")) dbeta = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--distill-pcs")) dpcs = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--idf-floor")) idf_floor = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--idf-power")) idf_power = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--phrases")) phrases = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--subwords")) subwords = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--train-questions")) train_q = argv[i + 1];   /* name<TAB>question pairs, disjoint from questions.tsv */
        else if (!strcmp(argv[i], "--train-epochs")) train_epochs = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--train-lr")) train_lr = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--train-margin")) train_margin = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--train-sentences")) train_sent = (unsigned)atoi(argv[i + 1]);   /* per corpus, its own train sentences as pairs */
        else if (!strcmp(argv[i], "--vocab-dump")) vocab_dump = argv[i + 1];   /* words + phrases with keys (distillation tooling) */
        else if (!strcmp(argv[i], "--extra-questions")) { if (n_extra < XSETS) extra_files[n_extra++] = argv[i + 1]; }
        else if (!strcmp(argv[i], "--alternates")) alternates = argv[i + 1];
        else if (!strcmp(argv[i], "--lexicon-extra")) lexicon_extra = argv[i + 1];
        else if (!strcmp(argv[i], "--full-min-count")) full_min_count = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--full-max-vocab")) full_max_vocab = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--neighbors")) neighbors = argv[i + 1];
        else if (!strcmp(argv[i], "--subword-min-n")) sub_min_n = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--subword-max-n")) sub_max_n = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--subword-min-words")) sub_min_words = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--subword-max-words")) sub_max_words = (unsigned)atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--subword-weight")) sub_weight = (float)atof(argv[i + 1]);
        else if (!strcmp(argv[i], "--phrase-weight")) phrase_weight = (float)atof(argv[i + 1]);
    }
    char path[2304], outpath[1024];
    snprintf(outpath, sizeof(outpath), "%s", out ? out : "");
    if (!out) snprintf(outpath, sizeof(outpath), "%s/results_cnet.json", fixture);

    /* ---- fixture ---- */
    Corpus *c = (Corpus *)calloc(MAXC, sizeof(Corpus)); int nc = 0;
    snprintf(path, sizeof(path), "%s/corpora.tsv", fixture);
    uint64_t corpora_fnv = fnv_file(path);
    FILE *fp = fopen(path, "r");
    if (!fp) { fprintf(stderr, "cannot open %s\n", path); return 2; }
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0;
        char *t2 = strchr(t1 + 1, '\t'); if (!t2) continue; *t2 = 0;
        const char *name = line, *role = t1 + 1, *sent = t2 + 1;
        int ci = (nc > 0 && strcmp(c[nc - 1].name, name) == 0) ? nc - 1 : find_corpus(c, nc, name);
        if (ci < 0) { if (nc >= MAXC) continue; ci = nc++; snprintf(c[ci].name, sizeof(c[ci].name), "%.127s", name); }
        if (!strcmp(role, "test")) { if (c[ci].n_test < MAXQ) c[ci].test[c[ci].n_test++] = strdup(sent); }
        else if (c[ci].n_train < MAXL) c[ci].train[c[ci].n_train++] = strdup(sent);
    }
    fclose(fp);
    int n_alien = 0; char *aliens[64];
    snprintf(path, sizeof(path), "%s/queries_alien.txt", fixture);
    fp = fopen(path, "r");
    if (fp) { while (n_alien < 64 && fgets(line, sizeof(line), fp)) { line[strcspn(line, "\r\n")] = 0; if (*line) aliens[n_alien++] = strdup(line); } fclose(fp); }
    int n_q = 0;
    snprintf(path, sizeof(path), "%s/questions.tsv", fixture);
    uint64_t questions_fnv = 0;
    fp = fopen(path, "r");
    if (fp) {
        questions_fnv = fnv_file(path);
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0;
            int ci = find_corpus(c, nc, line);
            if (ci >= 0 && c[ci].n_q < MAXQ) { c[ci].q[c[ci].n_q++] = strdup(t1 + 1); n_q++; }
        }
        fclose(fp);
    }
    int n_xq = 0, n_xset[XSETS] = {0};
    for (int xs = 0; xs < n_extra; ++xs) {
        fp = fopen(extra_files[xs], "r");
        if (!fp) { fprintf(stderr, "cannot open %s\n", extra_files[xs]); return 2; }
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#') continue;
            char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0;
            char *t2 = strchr(t1 + 1, '\t'); if (t2) *t2 = 0;   /* optional third column (contrast capsule / section) is ignored here */
            char *bar = strchr(line, '|'); if (bar) *bar = 0;     /* gold|alt|alt: per-question alternates */
            int ci = find_corpus(c, nc, line);
            if (ci >= 0 && c[ci].n_xq < XMAX) {
                int k = c[ci].n_xq; c[ci].n_xalt[k] = 0;
                while (bar && c[ci].n_xalt[k] < 4) {
                    char *nxt = strchr(bar + 1, '|'); if (nxt) *nxt = 0;
                    int ai = find_corpus(c, nc, bar + 1);
                    if (ai >= 0 && ai != ci) c[ci].xalt[k][c[ci].n_xalt[k]++] = ai;
                    bar = nxt;
                }
                c[ci].xset[k] = xs; c[ci].xq[k] = strdup(t1 + 1); c[ci].n_xq++; n_xq++; n_xset[xs]++;
            }
        }
        fclose(fp);
        printf("extra questions: %d from %s\n", n_xset[xs], extra_files[xs]);
    }
    int n_altp = 0;
    if (alternates && *alternates) {
        fp = fopen(alternates, "r");
        if (!fp) { fprintf(stderr, "cannot open %s\n", alternates); return 2; }
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#') continue;
            char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0;
            char *t2 = strchr(t1 + 1, '\t'); if (t2) *t2 = 0;
            int a = find_corpus(c, nc, line), b = find_corpus(c, nc, t1 + 1);
            if (a < 0 || b < 0 || a == b) continue;
            if (c[a].n_alt < 8) c[a].alt[c[a].n_alt++] = b;
            n_altp++;
        }
        fclose(fp);
        printf("alternates: %d directed pairs from %s (lenient top-1 accepts them; the same labels apply to every system)\n", n_altp, alternates);
    }
    int n_test = 0; for (int i = 0; i < nc; ++i) n_test += c[i].n_test;
    printf("fixture: %d corpora, %d test sentences, %d questions, %d aliens (corpora.tsv fnv 0x%016llx)\n",
           nc, n_test, n_q, n_alien, (unsigned long long)corpora_fnv);

    /* ---- lexicon from train rows only ---- */
    char traindir[1024]; snprintf(traindir, sizeof(traindir), "%s/train", cache);
    mkdir("var", 0755); mkdir("var/arena_cache", 0755); mkdir(cache, 0755); mkdir(traindir, 0755);
    for (int i = 0; i < nc; ++i) {
        snprintf(path, sizeof(path), "%.1024s/%.127s_corpus.txt", traindir, c[i].name);
        FILE *o = fopen(path, "w"); if (!o) continue;
        for (int j = 0; j < c[i].n_train; ++j) fprintf(o, "%s\n", c[i].train[j]);
        fclose(o);
    }
    /* lex_full may be built over the train rows PLUS training-register questions
     * (never evaluation text): everyday words then get learned vectors and
     * phrases instead of falling to identity noise. Written as a second
     * directory so the RI/distilled rows and the trainer's sentence pairs stay
     * on the pure corpora. */
    char lexdir[1024]; snprintf(lexdir, sizeof(lexdir), "%s", traindir);
    if (lexicon_extra && *lexicon_extra) {
        snprintf(lexdir, sizeof(lexdir), "%s/train_lex", cache); mkdir(lexdir, 0755);
        long n_added = 0;
        for (int i = 0; i < nc; ++i) {
            snprintf(path, sizeof(path), "%.1024s/%.127s_corpus.txt", lexdir, c[i].name);
            FILE *o = fopen(path, "w"); if (!o) continue;
            for (int j = 0; j < c[i].n_train; ++j) fprintf(o, "%s\n", c[i].train[j]);
            fclose(o);
        }
        fp = fopen(lexicon_extra, "r");
        if (!fp) { fprintf(stderr, "cannot open %s\n", lexicon_extra); return 2; }
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#') continue;
            char *t1 = strchr(line, '\t'); if (!t1) continue; *t1 = 0;
            char *t2 = strchr(t1 + 1, '\t'); if (t2) *t2 = 0;
            int ci = find_corpus(c, nc, line);
            if (ci < 0) continue;
            snprintf(path, sizeof(path), "%.1024s/%.127s_corpus.txt", lexdir, c[ci].name);
            FILE *o = fopen(path, "a"); if (!o) continue;
            fprintf(o, "%s\n", t1 + 1); fclose(o); n_added++;
        }
        fclose(fp);
        printf("lexicon build corpus: train rows + %ld training-register lines from %s (lex_full/lex_trained only)\n", n_added, lexicon_extra);
    }
    /* two lexicons from the train rows: pure Random Indexing, and (when a
     * distilled file is given) the same blended with the transformer context */
    CnetVsaLexiconBuildOpts lo; cnet_vsa_lexicon_build_opts_default(&lo);
    lo.beta = beta; lo.remove_pcs = pcs; lo.idf_floor = idf_floor; lo.idf_power = idf_power;
    CnetVsaLexiconBuildReport rep;
    char lexpath[1024]; snprintf(lexpath, sizeof(lexpath), "%s/arena.lex", cache);
    double t0 = now_us();
    if (cnet_vsa_lexicon_build(traindir, &lo, lexpath, &rep) != 0) { fprintf(stderr, "lexicon build failed\n"); return 3; }
    double lex_build_us = now_us() - t0;
    static CnetVsaLexicon lex, lexd, lexf, lext;
    if (cnet_vsa_lexicon_load(&lex, lexpath) != 0) { fprintf(stderr, "lexicon load failed\n"); return 3; }
    printf("lexicon: %u words from train rows only, beta %.2f, %u PCs removed, build %.0f ms, digest 0x%016llx\n",
           lex.hdr.count, beta, pcs, lex_build_us / 1e3, (unsigned long long)lex.hdr.digest);
    int have_dist = 0;
    double lexd_build_us = 0;
    if (distilled && *distilled) {
        CnetVsaLexiconBuildOpts ld = lo; ld.distilled = distilled; ld.distill_alpha = dalpha; ld.beta = dbeta; ld.remove_pcs = dpcs;
        char lexdpath[1024]; snprintf(lexdpath, sizeof(lexdpath), "%s/arena_distilled.lex", cache);
        t0 = now_us();
        if (cnet_vsa_lexicon_build(traindir, &ld, lexdpath, &rep) != 0 || cnet_vsa_lexicon_load(&lexd, lexdpath) != 0) {
            fprintf(stderr, "distilled lexicon build failed\n"); return 3;
        }
        lexd_build_us = now_us() - t0;
        have_dist = 1;
        printf("lexicon+distilled: %u keys blended (alpha %.2f, beta %.2f, %u PCs removed) from %s, build %.0f ms, digest 0x%016llx\n",
               lexd.hdr.distilled, lexd.hdr.distill_alpha, dbeta, dpcs, distilled, lexd_build_us / 1e3, (unsigned long long)lexd.hdr.digest);
    }
    /* lex_full: the best lexicon so far (distilled when given) plus learned
     * phrases and subword backoff; lex_trained: the same after a supervised pass
     * on (question, capsule) pairs that are disjoint from the frozen questions */
    int have_full = 0, have_trained = 0;
    double lexf_build_us = 0;
    CnetVsaLexiconTrainReport trep; memset(&trep, 0, sizeof(trep));
    if (phrases || subwords) {
        CnetVsaLexiconBuildOpts lf = lo;
        if (have_dist) { lf.distilled = distilled; lf.distill_alpha = dalpha; lf.beta = dbeta; lf.remove_pcs = dpcs; }
        lf.max_phrases = phrases; lf.max_subwords = subwords; lf.vocab_dump = vocab_dump;
        if (full_min_count) lf.min_count = full_min_count;
        if (full_max_vocab) lf.max_vocab = full_max_vocab;
        lf.subword_min_n = sub_min_n; lf.subword_max_n = sub_max_n; lf.subword_min_words = sub_min_words; lf.subword_max_words = sub_max_words; lf.subword_weight = sub_weight; lf.phrase_weight = phrase_weight;
        char lexfpath[1024]; snprintf(lexfpath, sizeof(lexfpath), "%s/arena_full.lex", cache);
        t0 = now_us();
        if (cnet_vsa_lexicon_build(lexdir, &lf, lexfpath, &rep) != 0 || cnet_vsa_lexicon_load(&lexf, lexfpath) != 0) {
            fprintf(stderr, "full lexicon build failed\n"); return 3;
        }
        lexf_build_us = now_us() - t0;
        have_full = 1;
        printf("lexicon+phrases+subwords: %u words + %u phrases, %u subword n-grams, distilled %u keys, build %.0f ms, table %.1f MB, digest 0x%016llx\n",
               lexf.hdr.count - lexf.hdr.phrases, lexf.hdr.phrases, lexf.hdr.subwords, lexf.hdr.distilled, lexf_build_us / 1e3,
               (sizeof(CnetVsaLexiconHeader) + (double)lexf.hdr.count * sizeof(CnetVsaLexiconEntry) + (double)lexf.hdr.subwords * sizeof(CnetVsaLexiconSubword)) / 1048576.0,
               (unsigned long long)lexf.hdr.digest);
        if (train_q && *train_q) {
            CnetVsaLexiconTrainOpts to; cnet_vsa_lexicon_train_opts_default(&to);
            to.epochs = train_epochs; to.lr = train_lr; to.margin = train_margin; to.sentences_per_corpus = train_sent;
            char lextpath[1024]; snprintf(lextpath, sizeof(lextpath), "%s/arena_trained.lex", cache);
            if (cnet_vsa_lexicon_train(lexfpath, lextpath, traindir, train_q, &to, &trep) != 0 || cnet_vsa_lexicon_load(&lext, lextpath) != 0) {
                fprintf(stderr, "lexicon training failed\n"); return 3;
            }
            have_trained = 1;
            printf("lexicon trained: %u question pairs (%u with an explicit negative) + %u sentence pairs over %u corpora, %u epochs, lr %.3f, margin %.2f, %u terms moved, train top-1 %.1f%% -> %.1f%%, %.1f s, digest 0x%016llx\n",
                   trep.pairs, trep.explicit_negatives, trep.sentence_pairs, trep.corpora, trep.epochs, train_lr, train_margin, trep.terms_touched, 100 * trep.train_top1_before, 100 * trep.train_top1_after,
                   trep.seconds, (unsigned long long)lext.hdr.digest);
        }
    }
    FILE *dumpf = dump ? fopen(dump, "w") : NULL;
    if (dumpf) fprintf(dumpf, "encoder\tkind\tgold\ttop1\ttop2\tsim1\tsim2\tsim_gold\tn_tok\tn_oov\tquery\n");

    FILE *jo = fopen(outpath, "w");
    if (!jo) { fprintf(stderr, "cannot write %s\n", outpath); return 4; }
    fprintf(jo, "{\n  \"fixture\": \"%s\",\n  \"fixture_fnv1a\": {\"corpora.tsv\": \"0x%016llx\", \"questions.tsv\": \"0x%016llx\"},\n",
            fixture, (unsigned long long)corpora_fnv, (unsigned long long)questions_fnv);
    fprintf(jo, "  \"corpora\": %d, \"test_sentences_n\": %d, \"questions_n\": %d, \"aliens_n\": %d,\n", nc, n_test, n_q, n_alien);
    fprintf(jo, "  \"lexicon\": {\"words\": %u, \"beta\": %.2f, \"remove_pcs\": %u, \"build_ms\": %.1f, \"digest\": \"0x%016llx\", \"table_bytes\": %zu},\n",
            lex.hdr.count, beta, pcs, lex_build_us / 1e3, (unsigned long long)lex.hdr.digest,
            sizeof(CnetVsaLexiconHeader) + (size_t)lex.hdr.count * sizeof(CnetVsaLexiconEntry));
    if (have_dist) fprintf(jo, "  \"lexicon_distilled\": {\"source\": \"%s\", \"keys\": %u, \"alpha\": %.2f, \"beta\": %.2f, \"remove_pcs\": %u, \"build_ms\": %.1f, \"digest\": \"0x%016llx\"},\n",
                           distilled, lexd.hdr.distilled, lexd.hdr.distill_alpha, dbeta, dpcs, lexd_build_us / 1e3, (unsigned long long)lexd.hdr.digest);
    if (have_full) fprintf(jo, "  \"lexicon_full\": {\"words\": %u, \"phrases\": %u, \"subwords\": %u, \"distilled\": %u, \"build_corpus_extra\": \"%s\", \"build_ms\": %.1f, \"table_bytes\": %zu, \"digest\": \"0x%016llx\"},\n",
                           lexf.hdr.count - lexf.hdr.phrases, lexf.hdr.phrases, lexf.hdr.subwords, lexf.hdr.distilled, lexicon_extra ? lexicon_extra : "", lexf_build_us / 1e3,
                           sizeof(CnetVsaLexiconHeader) + (size_t)lexf.hdr.count * sizeof(CnetVsaLexiconEntry) + (size_t)lexf.hdr.subwords * sizeof(CnetVsaLexiconSubword),
                           (unsigned long long)lexf.hdr.digest);
    if (have_trained) fprintf(jo, "  \"lexicon_trained\": {\"pairs\": %u, \"sentence_pairs\": %u, \"epochs\": %u, \"lr\": %.4f, \"margin\": %.3f, \"terms_moved\": %u, \"train_top1_before\": %.4f, \"train_top1_after\": %.4f, \"seconds\": %.1f, \"digest\": \"0x%016llx\"},\n",
                              trep.pairs, trep.sentence_pairs, trep.epochs, train_lr, train_margin, trep.terms_touched, trep.train_top1_before, trep.train_top1_after, trep.seconds,
                              (unsigned long long)lext.hdr.digest);
    fprintf(jo, "  \"encoders\": {\n");

    uint32_t encs[5] = { CNET_VSA_ENCODER_STEM, CNET_VSA_ENCODER_LEX, CNET_VSA_ENCODER_LEX, CNET_VSA_ENCODER_LEX, CNET_VSA_ENCODER_LEX };
    const char *enc_labels[5] = { "stem", "lex", "lex_distilled", "lex_full", "lex_trained" };
    const CnetVsaLexicon *enc_lex[5] = { NULL, &lex, &lexd, &lexf, &lext };
    int n_encs = 0;
    { int keep[5] = { 1, 1, have_dist, have_full, have_trained };
      for (int e = 0; e < 5; ++e) if (keep[e]) { encs[n_encs] = encs[e]; enc_labels[n_encs] = enc_labels[e]; enc_lex[n_encs] = enc_lex[e]; n_encs++; } }
    int8_t *cent = (int8_t *)malloc((size_t)nc * DIM);
    float *cnorm = (float *)malloc(nc * sizeof(float));
    float *wide = (float *)malloc(sizeof(float) * DIM * MAXL), *sum = (float *)malloc(sizeof(float) * DIM), *loo = (float *)malloc(sizeof(float) * DIM);
    float *d_in = (float *)malloc(sizeof(float) * MAXL), *d_neg = (float *)malloc(sizeof(float) * NNEG);
    int8_t *tq = (int8_t *)malloc((size_t)n_test * DIM); float *tqn = (float *)malloc(n_test * sizeof(float)); int *tgold = (int *)malloc(n_test * sizeof(int));
    int8_t *qq = (int8_t *)malloc((size_t)(n_q ? n_q : 1) * DIM); float *qqn = (float *)malloc((n_q ? n_q : 1) * sizeof(float)); int *qgold = (int *)malloc((n_q ? n_q : 1) * sizeof(int));
    int8_t *xx = (int8_t *)malloc((size_t)(n_xq ? n_xq : 1) * DIM); float *xxn = (float *)malloc((n_xq ? n_xq : 1) * sizeof(float)); int *xgold = (int *)malloc((n_xq ? n_xq : 1) * sizeof(int));
    if (!cent || !cnorm || !wide || !sum || !loo || !d_in || !d_neg || !tq || !tqn || !tgold || !qq || !qqn || !qgold || !xx || !xxn || !xgold) return 5;

    for (int e = 0; e < n_encs; ++e) {
        uint32_t enc = encs[e];
        cnet_vsa_lexicon_set_active(enc == CNET_VSA_ENCODER_LEX ? enc_lex[e] : NULL);
        size_t sep[3] = {0, 0, 0}; double nsum = 0, nsq = 0; long nn = 0; double insum = 0; long inn = 0; int evaluated = 0;
        double t_enc = 0; long n_enc = 0;
        for (int ci = 0; ci < nc; ++ci) {
            Corpus *k = &c[ci];
            memset(sum, 0, sizeof(float) * DIM);
            int nv = 0;
            for (int i = 0; i < k->n_train; ++i) {
                CnetVsaTokenList tl;
                if (cnet_vsa_text_tokenize(k->train[i], &tl) <= 0) continue;
                if (cnet_vsa_text_encode_topical_wide(&tl, wide + (size_t)nv * DIM, enc) != 0) continue;
                for (int d = 0; d < DIM; ++d) sum[d] += wide[(size_t)nv * DIM + d];
                nv++;
            }
            if (nv < 8) { cnorm[ci] = 0; memset(cent + (size_t)ci * DIM, 0, DIM); continue; }
            cnet_vsa_text_wide_to_q8(sum, cent + (size_t)ci * DIM);
            cnorm[ci] = cnet_vsa_text_q8_norm(cent + (size_t)ci * DIM);
            /* LOO in the production representation: q8 query vs q8(sum - v) */
            size_t ni = 0;
            int8_t vq[DIM], lq[DIM];
            for (int i = 0; i < nv; ++i) {
                if (cnet_vsa_gencap_encode_intent_q8(k->train[i], vq, enc) != 0) continue;
                for (int d = 0; d < DIM; ++d) loo[d] = sum[d] - wide[(size_t)i * DIM + d];
                cnet_vsa_text_wide_to_q8(loo, lq);
                float s = cnet_vsa_text_q8_similarity(vq, lq);
                d_in[ni++] = 1.0f - s; insum += s; inn++;
            }
            uint64_t sd = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)ci * 0xD1B54A32D192ED03ULL);
            size_t nneg = 0, guard = 0;
            while (nneg < NNEG && guard < NNEG * 20) {
                guard++;
                size_t oc = xs(&sd) % (size_t)nc; if ((int)oc == ci) continue;
                size_t li = xs(&sd) % (size_t)c[oc].n_train;
                if (cnet_vsa_gencap_encode_intent_q8(c[oc].train[li], vq, enc) != 0) continue;
                float s = cnet_vsa_text_q8_similarity_n(vq, cnet_vsa_text_q8_norm(vq), cent + (size_t)ci * DIM, cnorm[ci]);
                d_neg[nneg++] = 1.0f - s; nsum += s; nsq += (double)s * s; nn++;
            }
            if (ni < 8 || nneg < 8) continue;
            qsort(d_in, ni, sizeof(float), cmpf); qsort(d_neg, nneg, sizeof(float), cmpf);
            evaluated++;
            if (separable(d_in, ni, d_neg, nneg, 0.90f, 0.95f)) sep[0]++;
            if (separable(d_in, ni, d_neg, nneg, 0.80f, 0.90f)) sep[1]++;
            if (separable(d_in, ni, d_neg, nneg, 0.70f, 0.90f)) sep[2]++;
        }
        if (neighbors && *neighbors) {
            char npath[1200]; snprintf(npath, sizeof(npath), "%s.%s.tsv", neighbors, enc_labels[e]);
            FILE *nf = fopen(npath, "w");
            if (nf) {
                for (int ci = 0; ci < nc; ++ci) {
                    if (cnorm[ci] == 0) continue;
                    int b[5] = {-1, -1, -1, -1, -1}; float bs[5] = {-2, -2, -2, -2, -2};
                    for (int cj = 0; cj < nc; ++cj) {
                        if (cj == ci || cnorm[cj] == 0) continue;
                        float sim = cnet_vsa_text_q8_similarity_n(cent + (size_t)ci * DIM, cnorm[ci], cent + (size_t)cj * DIM, cnorm[cj]);
                        for (int k = 0; k < 5; ++k) if (sim > bs[k]) { for (int m = 4; m > k; --m) { bs[m] = bs[m - 1]; b[m] = b[m - 1]; } bs[k] = sim; b[k] = cj; break; }
                    }
                    fprintf(nf, "%s", c[ci].name);
                    for (int k = 0; k < 5; ++k) if (b[k] >= 0) fprintf(nf, "\t%s\t%.4f", c[b[k]].name, bs[k]);
                    fprintf(nf, "\n");
                }
                fclose(nf);
            }
        }
        /* queries */
        int ti = 0;
        for (int ci = 0; ci < nc; ++ci) for (int i = 0; i < c[ci].n_test; ++i) {
            double a = now_us();
            if (cnet_vsa_gencap_encode_intent_q8(c[ci].test[i], tq + (size_t)ti * DIM, enc) != 0) { memset(tq + (size_t)ti * DIM, 0, DIM); }
            t_enc += now_us() - a; n_enc++;
            tqn[ti] = cnet_vsa_text_q8_norm(tq + (size_t)ti * DIM); tgold[ti] = ci; ti++;
        }
        int qi = 0;
        for (int ci = 0; ci < nc; ++ci) for (int i = 0; i < c[ci].n_q; ++i) {
            if (cnet_vsa_gencap_encode_intent_q8(c[ci].q[i], qq + (size_t)qi * DIM, enc) != 0) memset(qq + (size_t)qi * DIM, 0, DIM);
            qqn[qi] = cnet_vsa_text_q8_norm(qq + (size_t)qi * DIM); qgold[qi] = ci; qi++;
        }
        int xi = 0; int *xsetv = (int *)malloc(sizeof(int) * (n_xq ? n_xq : 1));
        int (*xaltv)[4] = (int (*)[4])malloc(sizeof(int[4]) * (n_xq ? n_xq : 1)); int *xnalt = (int *)malloc(sizeof(int) * (n_xq ? n_xq : 1));
        for (int ci = 0; ci < nc; ++ci) for (int i = 0; i < c[ci].n_xq; ++i) {
            if (cnet_vsa_gencap_encode_intent_q8(c[ci].xq[i], xx + (size_t)xi * DIM, enc) != 0) memset(xx + (size_t)xi * DIM, 0, DIM);
            xxn[xi] = cnet_vsa_text_q8_norm(xx + (size_t)xi * DIM); xgold[xi] = ci; xsetv[xi] = c[ci].xset[i];
            xnalt[xi] = c[ci].n_xalt[i]; for (int a = 0; a < 4; ++a) xaltv[xi][a] = c[ci].xalt[i][a]; xi++;
        }
        /* query texts in the same order as the vectors, for the dump */
        const char **ttexts = (const char **)malloc(sizeof(char *) * (ti ? ti : 1));
        const char **qtexts = (const char **)malloc(sizeof(char *) * (qi ? qi : 1));
        const char **xtexts = (const char **)malloc(sizeof(char *) * (xi ? xi : 1));
        { int k = 0; for (int ci = 0; ci < nc; ++ci) for (int i = 0; i < c[ci].n_test; ++i) ttexts[k++] = c[ci].test[i];
          k = 0; for (int ci = 0; ci < nc; ++ci) for (int i = 0; i < c[ci].n_q; ++i) qtexts[k++] = c[ci].q[i];
          k = 0; for (int ci = 0; ci < nc; ++ci) for (int i = 0; i < c[ci].n_xq; ++i) xtexts[k++] = c[ci].xq[i]; }
        DumpCtx dct = { ttexts, "test", enc_labels[e], c, dumpf };
        DumpCtx dcq = { qtexts, "question", enc_labels[e], c, dumpf };
        TopK ts = score_topk(cent, cnorm, nc, tq, tqn, tgold, ti, &dct, enc, NULL, NULL);
        TopK qs = {0, 0, 0, 0, 0}, xs_[XSETS];
        if (qi > 0) qs = score_topk(cent, cnorm, nc, qq, qqn, qgold, qi, &dcq, enc, NULL, NULL);
        /* each extra set scored on its own: gather its vectors in fixture order */
        for (int xs = 0; xs < n_extra; ++xs) {
            int8_t *sv = (int8_t *)malloc((size_t)(n_xset[xs] ? n_xset[xs] : 1) * DIM); float *svn = (float *)malloc(sizeof(float) * (n_xset[xs] ? n_xset[xs] : 1));
            int *sg = (int *)malloc(sizeof(int) * (n_xset[xs] ? n_xset[xs] : 1)); const char **st = (const char **)malloc(sizeof(char *) * (n_xset[xs] ? n_xset[xs] : 1));
            int (*sa)[4] = (int (*)[4])malloc(sizeof(int[4]) * (n_xset[xs] ? n_xset[xs] : 1)); int *sna = (int *)malloc(sizeof(int) * (n_xset[xs] ? n_xset[xs] : 1));
            int k = 0;
            for (int i = 0; i < xi; ++i) if (xsetv[i] == xs) { memcpy(sv + (size_t)k * DIM, xx + (size_t)i * DIM, DIM); svn[k] = xxn[i]; sg[k] = xgold[i]; st[k] = xtexts[i];
                                                            sna[k] = xnalt[i]; for (int a = 0; a < 4; ++a) sa[k][a] = xaltv[i][a]; k++; }
            char kind[64]; snprintf(kind, sizeof(kind), "extra%d", xs);
            DumpCtx dcx = { st, kind, enc_labels[e], c, dumpf };
            TopK z = {0, 0, 0, 0, 0};
            if (k > 0) z = score_topk(cent, cnorm, nc, sv, svn, sg, k, &dcx, enc, sa, sna);
            xs_[xs] = z;
            free(sv); free(svn); free(sg); free(st); free(sa); free(sna);
        }
        free(xsetv); free(xaltv); free(xnalt);
        free(ttexts); free(qtexts); free(xtexts);
        /* aliens */
        double abest = 0, az = 0;
        for (int a = 0; a < n_alien; ++a) {
            int8_t av[DIM]; if (cnet_vsa_gencap_encode_intent_q8(aliens[a], av, enc) != 0) continue;
            float an = cnet_vsa_text_q8_norm(av); double m = 0, m2 = 0; float best = -2;
            for (int ci = 0; ci < nc; ++ci) { float s = cnet_vsa_text_q8_similarity_n(av, an, cent + (size_t)ci * DIM, cnorm[ci]); m += s; m2 += (double)s * s; if (s > best) best = s; }
            m /= nc; double sd = sqrt(m2 / nc - m * m) + 1e-9; abest += best; az += (best - m) / sd;
        }
        double nm = nsum / (nn ? nn : 1), nsd = sqrt(nsq / (nn ? nn : 1) - nm * nm);
        fprintf(jo, "    \"%s\": {\n", enc_labels[e]);
        fprintf(jo, "      \"test_sentences\": {\"n\": %d, \"top1\": %.4f, \"top3\": %.4f, \"margin\": %.4f, \"top1_lenient\": %.4f},\n", ts.n, ts.top1, ts.top3, ts.margin, ts.lenient1);
        if (qi > 0) fprintf(jo, "      \"questions\": {\"n\": %d, \"top1\": %.4f, \"top3\": %.4f, \"margin\": %.4f, \"top1_lenient\": %.4f},\n", qs.n, qs.top1, qs.top3, qs.margin, qs.lenient1);
        for (int xs = 0; xs < n_extra; ++xs) {
            const char *base = strrchr(extra_files[xs], '/'); base = base ? base + 1 : extra_files[xs];
            fprintf(jo, "      \"questions_extra:%s\": {\"n\": %d, \"top1\": %.4f, \"top3\": %.4f, \"margin\": %.4f, \"top1_lenient\": %.4f, \"source\": \"%s\"},\n",
                    base, xs_[xs].n, xs_[xs].top1, xs_[xs].top3, xs_[xs].margin, xs_[xs].lenient1, extra_files[xs]);
        }
        fprintf(jo, "      \"separability\": {\"0.90/0.95\": {\"count\": %zu, \"fraction\": %.4f}, \"0.80/0.90\": {\"count\": %zu, \"fraction\": %.4f}, \"0.70/0.90\": {\"count\": %zu, \"fraction\": %.4f}},\n",
                sep[0], (double)sep[0] / evaluated, sep[1], (double)sep[1] / evaluated, sep[2], (double)sep[2] / evaluated);
        fprintf(jo, "      \"evaluated\": %d, \"null_sd\": %.4f, \"in_domain_mean_cos\": %.4f,\n", evaluated, nsd, insum / (inn ? inn : 1));
        fprintf(jo, "      \"aliens\": {\"n\": %d, \"best_cos_mean\": %.4f, \"z_mean\": %.3f},\n", n_alien, abest / (n_alien ? n_alien : 1), az / (n_alien ? n_alien : 1));
        fprintf(jo, "      \"encode_us\": %.2f\n    }%s\n", t_enc / (n_enc ? n_enc : 1), e + 1 < n_encs ? "," : "");
        printf("| %-13s | test top-1 %.1f%% top-3 %.1f%% margin %.4f | questions top-1 %.1f%% top-3 %.1f%% (n=%d) | separable 0.80/0.90 %.1f%% 0.90/0.95 %.1f%% | null sd %.4f | alien z %.2f | encode %.2f us |",
               enc_labels[e], 100 * ts.top1, 100 * ts.top3, ts.margin, 100 * qs.top1, 100 * qs.top3, qs.n,
               100.0 * sep[1] / evaluated, 100.0 * sep[0] / evaluated, nsd, az / (n_alien ? n_alien : 1), t_enc / (n_enc ? n_enc : 1));
        for (int xs = 0; xs < n_extra; ++xs) printf(" extra%d top-1 %.1f%% top-3 %.1f%% lenient %.1f%% (n=%d) |", xs, 100 * xs_[xs].top1, 100 * xs_[xs].top3, 100 * xs_[xs].lenient1, xs_[xs].n);
        printf("\n");
    }
    fprintf(jo, "  }\n}\n");
    fclose(jo);
    if (dumpf) fclose(dumpf);
    printf("wrote %s\n", outpath);
    return 0;
}
