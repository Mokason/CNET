#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include "../include/cnet_vsa.h"
#include "../include/cnet_vsa_bsc.h"
#include "../include/cnet_vsa_text.h"

static int checks = 0;
static int failures = 0;

static void check(int ok, const char *msg) {
    checks++;
    printf("  %-62s %s\n", msg, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint64_t bench_mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Task 1: Positional N-Gram Text Projector Benchmark\n");
    printf("=================================================================\n\n");

    const int D = CNET_VSA_DEFAULT_DIM;

    /* -------------------------------------------------------------
     * Suite 1: Tokenization & Determinism
     * ------------------------------------------------------------- */
    printf("[1/5] Testing Tokenization & Deterministic Hypervector Projection...\n");
    const char *sample = "CNET builds certified Micro Tensor Kernels for specialized intelligence.";
    CnetVsaTokenList tlist;
    int count = cnet_vsa_text_tokenize(sample, &tlist);
    printf("  Token count: %d\n", count);
    check(count == 10, "Sentence correctly tokenized into 10 tokens");
    check(strcmp(tlist.tokens[0].token, "cnet") == 0, "Token 0 parsed as 'cnet'");

    float v1[CNET_VSA_DEFAULT_DIM], v2[CNET_VSA_DEFAULT_DIM], v3[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_text_token_vec("transformer", v1, D);
    cnet_vsa_text_token_vec("transformer", v2, D);
    cnet_vsa_text_token_vec("recurrent", v3, D);

    float sim_ident = cnet_vsa_similarity(v1, v2, D);
    float sim_ortho = cnet_vsa_similarity(v1, v3, D);
    printf("  Identical token similarity: %.6f\n", sim_ident);
    printf("  Distinct token similarity:  %.6f\n", sim_ortho);
    check(fabsf(sim_ident - 1.0f) < 1e-5f, "Identical tokens produce bit-identical hypervectors (sim = 1.0)");
    check(fabsf(sim_ortho) < 0.15f, "Distinct tokens produce quasi-orthogonal hypervectors (|sim| < 0.15)");

    /* -------------------------------------------------------------
     * Suite 2: Sequence Anagram Discrimination (Order Sensitivity)
     * ------------------------------------------------------------- */
    printf("\n[2/5] Testing Positional Roll Anagram Discrimination...\n");
    const char *sent_a = "dog bit cat";
    const char *sent_b = "cat bit dog";
    const char *sent_c = "dog bit cat";

    float sim_anagram = cnet_vsa_text_sequence_similarity(sent_a, sent_b, D);
    float sim_self = cnet_vsa_text_sequence_similarity(sent_a, sent_c, D);

    printf("  Self Similarity ('dog bit cat' vs self):            %.4f\n", sim_self);
    printf("  Anagram Similarity ('dog bit cat' vs 'cat bit dog'): %.4f\n", sim_anagram);

    check(fabsf(sim_self - 1.0f) < 1e-4f, "Identical sentences produce exact match (sim = 1.000)");
    check(sim_anagram < 0.50f, "Positional roll distinguishes anagrams ('dog bit cat' vs 'cat bit dog' < 0.50)");

    /* -------------------------------------------------------------
     * Suite 3: Decoding Token at Specific Position
     * ------------------------------------------------------------- */
    printf("\n[3/5] Testing Positional Unbinding (Token Extraction at Index t)...\n");
    const char *sent_space = "apollo lander reached lunar surface";
    CnetVsaTokenList space_tokens;
    cnet_vsa_text_tokenize(sent_space, &space_tokens);

    float space_vec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_text_encode_continuous(&space_tokens, space_vec, D);

    /* Build vocabulary codebook */
    CnetVsaCodebook vocab;
    cnet_vsa_codebook_init(&vocab, D, 16);
    for (size_t i = 0; i < space_tokens.count; ++i) {
        float tv[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_token_vec(space_tokens.tokens[i].token, tv, D);
        cnet_vsa_codebook_add(&vocab, space_tokens.tokens[i].token, tv);
    }

    int pos_correct = 0;
    for (size_t i = 0; i < space_tokens.count; ++i) {
        char recovered[CNET_VSA_TOKEN_LEN] = {0};
        float score = 0.0f;
        cnet_vsa_text_decode_at_pos(space_vec, (int)i, &vocab, recovered, sizeof(recovered), &score);
        if (strcmp(recovered, space_tokens.tokens[i].token) == 0) {
            pos_correct++;
        }
    }
    printf("  Positional recoveries: %d / %zu\n", pos_correct, space_tokens.count);
    check(pos_correct == (int)space_tokens.count, "Positional unbinding extracts 100% of tokens at their exact indices");

    /* -------------------------------------------------------------
     * Suite 4: Token Position Localization
     * ------------------------------------------------------------- */
    printf("\n[4/5] Testing Token Position Localization...\n");
    const char *sent_cog = "deep knowledge accumulation yields superior intelligence";
    CnetVsaTokenList cog_tokens;
    cnet_vsa_text_tokenize(sent_cog, &cog_tokens);
    float cog_vec[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_text_encode_continuous(&cog_tokens, cog_vec, D);

    int found_pos = -1;
    float found_sim = 0.0f;
    cnet_vsa_text_find_token_pos(cog_vec, "accumulation", (int)cog_tokens.count, &found_pos, &found_sim, D);
    printf("  Query 'accumulation' -> located at index %d (expected 2), sim: %.4f\n", found_pos, found_sim);
    check(found_pos == 2, "Token 'accumulation' localized at exact index (Index 2)");
    check(found_sim > 0.30f, "Position match has strong positive projection margin (> 0.30)");

    /* -------------------------------------------------------------
     * Suite 5: Throughput & Stress Test
     * ------------------------------------------------------------- */
    printf("\n[5/5] Stress-Testing Encoding Throughput...\n");
    const int RUNS = 10000;
    uint64_t t0 = bench_mono_ns();
    for (int r = 0; r < RUNS; ++r) {
        float dummy[CNET_VSA_DEFAULT_DIM];
        cnet_vsa_text_encode_continuous(&space_tokens, dummy, D);
    }
    uint64_t t1 = bench_mono_ns();
    double total_ms = (double)(t1 - t0) / 1000000.0;
    double us_per_sent = (total_ms * 1000.0) / (double)RUNS;
    double words_per_sec = ((double)(RUNS * space_tokens.count) / (total_ms * 1e-3));
    printf("  %d sentences (%zu words) encoded in %.2f ms\n", RUNS, RUNS * space_tokens.count, total_ms);
    printf("  Latency:     %.2f us/sentence\n", us_per_sent);
    printf("  Throughput:  %.0f words/sec\n", words_per_sec);
    check(us_per_sent < 15.0, "Encoding latency is sub-15 microseconds per sentence");
    check(words_per_sec > 250000.0, "Throughput exceeds 250,000 words per second on CPU");

    cnet_vsa_codebook_free(&vocab);

    printf("\n-----------------------------------------------------------------\n");
    if (failures == 0) {
        printf("CNET_VSA_TEXT_BENCH_PASS: all %d checks passed.\n", checks);
        return 0;
    } else {
        printf("CNET_VSA_TEXT_BENCH_FAIL: %d of %d checks failed.\n", failures, checks);
        return 1;
    }
}
