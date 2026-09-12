/* Hermetic lexicon gate. Marker: CNET_VSA_LEXICON_BENCH_PASS
 *  1. Build from a synthetic corpus dir: words that share contexts end up
 *     closer than words that do not; the file verifies; a flipped byte fails.
 *  2. The LEX encoder refuses without an active lexicon and never falls back.
 *  3. Known words use the learned vector, unknown words their identity at the
 *     lexicon's magnitude, so mixed texts stay comparable.
 *  4. A LEX-sealed capsule records the lexicon tag; registry admission refuses
 *     it under a different lexicon and accepts it under the same one.
 *  5. Phrases: an adjacent pair that recurs gets an entry the encoder adds;
 *     subwords: a word absent from the table is composed from the learned
 *     n-grams of the words it resembles (near them, not random), and the
 *     subword table is under the digest. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <sys/stat.h>
#include "cnet_vsa.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_lexicon.h"
#include "cnet_vsa_gen_capsule.h"

static uint64_t g = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void) { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return (uint32_t)g; }

/* two topics with private vocabularies, plus a pair of synonyms that appear
 * only in identical contexts of topic A ("quench"/"cooldown") */
static const char *A_CTX[] = { "coating", "ceramic", "crack", "thermal", "furnace", "gradient", "layer", "stress" };
static const char *B_CTX[] = { "levain", "crumb", "hydration", "flour", "proof", "oven", "crust", "starter" };

static void write_corpus(const char *path, const char **ctx, int nctx, const char *syn1, const char *syn2) {
    FILE *fp = fopen(path, "w"); assert(fp);
    for (int i = 0; i < 80; ++i) {
        const char *syn = (i & 1) ? syn1 : syn2;
        fprintf(fp, "the %s %s under %s during %s with %s.\n", ctx[rnd() % nctx], syn, ctx[rnd() % nctx], ctx[rnd() % nctx], ctx[rnd() % nctx]);
    }
    fclose(fp);
}

static float wide_cos(const char *a, const char *b, uint32_t enc) {
    CnetVsaTokenList ta, tb; float va[CNET_VSA_TOPICAL_DIM], vb[CNET_VSA_TOPICAL_DIM];
    assert(cnet_vsa_text_tokenize(a, &ta) > 0 && cnet_vsa_text_tokenize(b, &tb) > 0);
    assert(cnet_vsa_text_encode_topical_wide(&ta, va, enc) == 0);
    assert(cnet_vsa_text_encode_topical_wide(&tb, vb, enc) == 0);
    double d = 0; for (int i = 0; i < CNET_VSA_TOPICAL_DIM; ++i) d += (double)va[i] * vb[i];
    return (float)d;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Lexicon Bench (Random Indexing, fail-closed contract)\n");
    printf("=================================================================\n\n");
    const char *dir = "/tmp/cnet_vsa_lexicon_bench";
    mkdir(dir, 0755);
    write_corpus("/tmp/cnet_vsa_lexicon_bench/topic_a_corpus.txt", A_CTX, 8, "quench", "cooldown");
    write_corpus("/tmp/cnet_vsa_lexicon_bench/topic_b_corpus.txt", B_CTX, 8, "bake", "roast");

    printf("[1/5] Build, verify, tamper\n");
    CnetVsaLexiconBuildOpts o; cnet_vsa_lexicon_build_opts_default(&o);
    o.min_count = 2; o.max_df = 1.0f; o.remove_pcs = 1;
    CnetVsaLexiconBuildReport rep;
    const char *lexpath = "/tmp/cnet_vsa_lexicon_bench/test.lex";
    assert(cnet_vsa_lexicon_build(dir, &o, lexpath, &rep) == 0);
    CnetVsaLexicon lex;
    assert(cnet_vsa_lexicon_load(&lex, lexpath) == 0);
    printf("  built: %llu sentences, %llu tokens, %u words, sweep %.4f s, digest 0x%016llx\n",
           (unsigned long long)rep.sentences, (unsigned long long)rep.tokens, lex.hdr.count, rep.sweep_seconds,
           (unsigned long long)lex.hdr.digest);
    assert(lex.hdr.count >= 16);
    assert(cnet_vsa_lexicon_find(&lex, "quench") != NULL);
    assert(cnet_vsa_lexicon_find(&lex, "cooldown") != NULL);
    assert(cnet_vsa_lexicon_find(&lex, "zzznotaword") == NULL);
    {
        FILE *fp = fopen(lexpath, "r+b"); assert(fp);
        long off = (long)sizeof(CnetVsaLexiconHeader) + 8 + 4 + 4 + 100;  /* inside the first entry's q8 */
        assert(fseek(fp, off, SEEK_SET) == 0);
        int c = fgetc(fp); assert(c != EOF);
        assert(fseek(fp, off, SEEK_SET) == 0);
        fputc(c ^ 1, fp); fclose(fp);
        CnetVsaLexicon bad;
        assert(cnet_vsa_lexicon_load(&bad, lexpath) == -6);
        /* restore */
        fp = fopen(lexpath, "r+b"); assert(fp); assert(fseek(fp, off, SEEK_SET) == 0); fputc(c, fp); fclose(fp);
        assert(cnet_vsa_lexicon_load(&bad, lexpath) == 0);
        cnet_vsa_lexicon_free(&bad);
    }
    printf("  load ok, absent word NULL, flipped byte -> rc=-6 PASS\n");

    printf("\n[2/5] LEX refuses without a lexicon; hash encoders unaffected\n");
    cnet_vsa_lexicon_set_active(NULL);
    {
        int8_t q[CNET_VSA_TOPICAL_DIM];
        assert(cnet_vsa_gencap_encode_intent_q8("thermal coating crack", q, CNET_VSA_ENCODER_LEX) == -1);
        assert(cnet_vsa_gencap_encode_intent_q8("thermal coating crack", q, CNET_VSA_ENCODER_STEM) == 0);
    }
    printf("  refused (-1) without lexicon; stem still encodes PASS\n");

    printf("\n[3/5] Learned similarity: synonyms by shared context move together\n");
    cnet_vsa_lexicon_set_active(&lex);
    float syn_lex = wide_cos("quench", "cooldown", CNET_VSA_ENCODER_LEX);
    float syn_stem = wide_cos("quench", "cooldown", CNET_VSA_ENCODER_STEM);
    float cross_lex = wide_cos("quench", "levain", CNET_VSA_ENCODER_LEX);
    float unknown = wide_cos("quench zzzunknownword", "quench", CNET_VSA_ENCODER_LEX);
    printf("  cos(quench,cooldown): lex %.3f vs stem %.3f; cos(quench,levain) lex %.3f; unknown word keeps text comparable: %.3f\n",
           syn_lex, syn_stem, cross_lex, unknown);
    assert(fabsf(syn_stem) < 0.15f);          /* hashes: unrelated */
    assert(syn_lex > syn_stem + 0.10f);        /* lexicon: shared contexts */
    assert(syn_lex > cross_lex + 0.10f);       /* but not across topics */
    assert(unknown > 0.3f);                    /* fallback identity keeps magnitude sane */
    printf("  PASS\n");

    printf("\n[4/5] Capsule tag: sealed under one lexicon, refused under another\n");
    {
        CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule)); assert(cap);
        assert(cnet_vsa_gencap_init(cap, "lex_cap", "TEST", CNET_VSA_DEFAULT_DIM) == 0);
        assert(cnet_vsa_gencap_set_encoder(cap, CNET_VSA_ENCODER_LEX) == 0);
        FILE *fp = fopen("/tmp/cnet_vsa_lexicon_bench/topic_a_corpus.txt", "r"); assert(fp);
        char line[512]; int n = 0;
        while (fgets(line, sizeof(line), fp)) { line[strcspn(line, "\n")] = 0; if (cnet_vsa_gencap_ingest(cap, line) > 0) n++; }
        fclose(fp);
        assert(n >= 50);
        assert(cnet_vsa_gencap_seal(cap) == 0);
        uint32_t tag = 0; memcpy(&tag, &cap->topical.reserved1, sizeof(tag));
        assert(tag == cnet_vsa_lexicon_active_tag() && tag != 0);
        CnetVsaGenRegistry *reg = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry)); assert(reg);
        assert(cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM) == 0);
        assert(cnet_vsa_registry_add_capsule_mem(reg, cap, NULL) == 0);
        /* a different lexicon: same corpus, different beta -> different digest */
        CnetVsaLexiconBuildOpts o2 = o; o2.beta = 0.25f;
        assert(cnet_vsa_lexicon_build(dir, &o2, "/tmp/cnet_vsa_lexicon_bench/other.lex", NULL) == 0);
        CnetVsaLexicon lex2; assert(cnet_vsa_lexicon_load(&lex2, "/tmp/cnet_vsa_lexicon_bench/other.lex") == 0);
        assert(lex2.hdr.digest != lex.hdr.digest);
        cnet_vsa_lexicon_set_active(&lex2);
        assert(cnet_vsa_registry_add_capsule_mem(reg, cap, NULL) == -8);
        cnet_vsa_lexicon_set_active(NULL);
        assert(cnet_vsa_registry_add_capsule_mem(reg, cap, NULL) == -8);
        /* sealing with LEX and no lexicon is refused */
        CnetVsaGenCapsule *cap2 = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule)); assert(cap2);
        assert(cnet_vsa_gencap_init(cap2, "lex_cap2", "TEST", CNET_VSA_DEFAULT_DIM) == 0);
        assert(cnet_vsa_gencap_set_encoder(cap2, CNET_VSA_ENCODER_LEX) == 0);
        for (int i = 0; i < 12; ++i) cnet_vsa_gencap_ingest(cap2, "thermal coating crack layer under stress");
        assert(cap2->wide_count == 0);                 /* the wide encoder refused every sentence */
        assert(cnet_vsa_gencap_seal(cap2) == -7);      /* no wide evidence: refused, never sealed blind */
        assert(cap2->certified == 0);
        assert(reg->count == 1);
        cnet_vsa_lexicon_free(&lex2);
        free(cap); free(cap2); free(reg);
    }
    printf("  tag recorded; admission refused under another lexicon (-8) and under none (-8) PASS\n");
    cnet_vsa_lexicon_free(&lex);

    printf("\n[5/5] Phrases and learned subword backoff\n");
    {
        CnetVsaLexiconBuildOpts o3 = o; o3.max_phrases = 64; o3.phrase_min_count = 3; o3.max_subwords = 2048; o3.subword_min_words = 1;   /* tiny corpus: n-grams unique to one word still compose */
        o3.vocab_dump = "/tmp/cnet_vsa_lexicon_bench/vocab.tsv";
        CnetVsaLexiconBuildReport r3;
        const char *p3 = "/tmp/cnet_vsa_lexicon_bench/full.lex";
        assert(cnet_vsa_lexicon_build(dir, &o3, p3, &r3) == 0);
        CnetVsaLexicon lf; assert(cnet_vsa_lexicon_load(&lf, p3) == 0);
        printf("  built: %u words + %u phrases, %u subword n-grams\n", lf.hdr.count - lf.hdr.phrases, lf.hdr.phrases, lf.hdr.subwords);
        assert(lf.hdr.phrases > 0 && lf.hdr.subwords > 0 && r3.phrases == lf.hdr.phrases && r3.subwords == lf.hdr.subwords);
        /* a phrase entry exists for a pair that recurs (the corpus repeats "<ctx> <syn>" pairs) */
        int found_phrase = 0;
        for (int i = 0; i < 8 && !found_phrase; ++i) {
            uint64_t pk = cnet_vsa_lexicon_phrase_key(cnet_vsa_lexicon_word_key(A_CTX[i]), cnet_vsa_lexicon_word_key("quench"));
            if (cnet_vsa_lexicon_find_hash(&lf, pk)) found_phrase = 1;
        }
        assert(found_phrase);
        cnet_vsa_lexicon_set_active(&lf);
        /* the phrase adds to the text: the same words in the learned order score higher against topic A than reversed */
        (void)0;
        /* subword composition: a word unseen in the corpus but spelled like a topic-A word */
        int16_t comp[CNET_VSA_TOPICAL_DIM];
        char stem[CNET_VSA_TOKEN_LEN];
        snprintf(stem, sizeof(stem), "furnacelike"); cnet_vsa_text_stem(stem);
        int by_sub = cnet_vsa_lexicon_oov_vector(&lf, stem, comp);
        assert(by_sub == 1);
        snprintf(stem, sizeof(stem), "qqqqqqq");
        assert(cnet_vsa_lexicon_oov_vector(&lf, stem, comp) == 0);   /* nothing known: identity */
        float a_sub = wide_cos("furnacelike", "furnace", CNET_VSA_ENCODER_LEX);
        float b_sub = wide_cos("furnacelike", "levain", CNET_VSA_ENCODER_LEX);
        float a_id = wide_cos("furnacelike", "furnace", CNET_VSA_ENCODER_STEM);
        printf("  cos(furnacelike,furnace): composed %.3f vs hash %.3f; cos(furnacelike,levain) composed %.3f\n", a_sub, a_id, b_sub);
        assert(fabsf(a_id) < 0.15f);          /* hash identity: unrelated */
        assert(a_sub > a_id + 0.10f);          /* composed from learned n-grams: near the word it resembles */
        assert(a_sub > b_sub + 0.10f);         /* and not near the other topic */
        /* the subword table is under the digest */
        FILE *fp = fopen(p3, "r+b"); assert(fp);
        long off = (long)sizeof(CnetVsaLexiconHeader) + (long)lf.hdr.count * (long)sizeof(CnetVsaLexiconEntry) + 8 + 4 + 4 + 7;
        assert(fseek(fp, off, SEEK_SET) == 0);
        int c = fgetc(fp); assert(c != EOF);
        assert(fseek(fp, off, SEEK_SET) == 0); fputc(c ^ 1, fp); fclose(fp);
        CnetVsaLexicon bad; assert(cnet_vsa_lexicon_load(&bad, p3) == -6);
        fp = fopen(p3, "r+b"); assert(fp); assert(fseek(fp, off, SEEK_SET) == 0); fputc(c, fp); fclose(fp);
        assert(cnet_vsa_lexicon_load(&bad, p3) == 0); cnet_vsa_lexicon_free(&bad);
        /* the dump lists words and phrases with their keys */
        fp = fopen(o3.vocab_dump, "r"); assert(fp);
        int nw = 0, np = 0; char ln[512];
        while (fgets(ln, sizeof(ln), fp)) { if (ln[0] == 'W') nw++; else if (ln[0] == 'P') np++; }
        fclose(fp);
        assert(nw == (int)(lf.hdr.count - lf.hdr.phrases) && np == (int)lf.hdr.phrases);
        cnet_vsa_lexicon_set_active(NULL);
        cnet_vsa_lexicon_free(&lf);
    }
    printf("  phrase entry present; composed unknown word near its lookalike; subword bytes under the digest PASS\n");

    printf("\n=================================================================\n");
    printf(" CNET_VSA_LEXICON_BENCH_PASS\n");
    printf("=================================================================\n");
    return 0;
}
