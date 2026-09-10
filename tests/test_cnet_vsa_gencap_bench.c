#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <math.h>

#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_ngram.h"
#include "cnet_vsa_gen_capsule.h"

static double get_time_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
}

static void encode_text(const char *text, float *out_vec, int dim) {
    cnet_vsa_gencap_encode_intent(text, out_vec, dim);
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Generative Knowledge Capsule Benchmark & Certification \n");
    printf("=================================================================\n\n");

    /* Allocate capsules on heap (CnetVsaGenCapsule is ~10.5 MB) */
    CnetVsaGenCapsule *cap_nature = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    CnetVsaGenCapsule *cap_aero = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    CnetVsaGenCapsule *loaded_cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    CnetVsaGenCapsule *tampered_cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    assert(cap_nature && cap_aero && loaded_cap && tampered_cap);

    /* -------------------------------------------------------------
     * [1/5] Building Specialized Generative Capsules
     * ------------------------------------------------------------- */
    printf("[1/5] Compiling Two Specialized Independent Capsules...\n");

    /* Capsule A: Narrative Forest Domain */
    assert(cnet_vsa_gencap_init(cap_nature, "nature_forest_v1", "NARRATIVE", CNET_VSA_DEFAULT_DIM) == 0);

    const char *nature_sentences[] = {
        "Once upon a time there was a curious fox who found a glowing mushroom in the enchanted forest.",
        "Mia the rabbit hopped fast across the sunny meadow to hide from the summer rain.",
        "The clever bird sang beautiful songs and told the princess where the golden crown was hidden.",
        "A caring boy found a lost puppy in the woods and returned it to its happy family.",
        "The girl planted seeds in the spring and by summer the flowers were taller than she was."
    };
    for (size_t i = 0; i < sizeof(nature_sentences)/sizeof(nature_sentences[0]); ++i) {
        cnet_vsa_gencap_ingest(cap_nature, nature_sentences[i]);
    }

    const char nature_slots[3][32] = {"hero", "setting", "action"};
    assert(cnet_vsa_gencap_add_frame(cap_nature, "forest_exploration", NULL, 3, nature_slots) == 0);
    assert(cnet_vsa_gencap_seal(cap_nature) == 0);
    assert(cap_nature->certified == 1);
    printf("  Capsule A ('%s') sealed: %zu words, %zu transitions, digest=0x%llx\n",
           cap_nature->name, cap_nature->ngram.vocab_count, cap_nature->ngram.transition_count,
           (unsigned long long)cap_nature->digest);

    /* Capsule B: Aerospace Technical Domain */
    assert(cnet_vsa_gencap_init(cap_aero, "aerospace_orbital_v1", "TECHNICAL", CNET_VSA_DEFAULT_DIM) == 0);

    const char *aero_sentences[] = {
        "The orbital velocity of the spacecraft reached seven kilometers per second in low earth orbit.",
        "Telemetry confirmed nominal telemetry data transmission across the deep space tracking network.",
        "The propulsion thruster ignited delivering delta velocity to adjust the trajectory towards mars.",
        "Solar arrays deployed smoothly providing electrical power to all payload instruments on board.",
        "Ground control verified the guidance navigation and control system status prior to engine burn."
    };
    for (size_t i = 0; i < sizeof(aero_sentences)/sizeof(aero_sentences[0]); ++i) {
        cnet_vsa_gencap_ingest(cap_aero, aero_sentences[i]);
    }

    const char aero_slots[3][32] = {"spacecraft", "system", "maneuver"};
    assert(cnet_vsa_gencap_add_frame(cap_aero, "orbital_maneuver", NULL, 3, aero_slots) == 0);
    assert(cnet_vsa_gencap_seal(cap_aero) == 0);
    assert(cap_aero->certified == 1);
    printf("  Capsule B ('%s') sealed: %zu words, %zu transitions, digest=0x%llx\n",
           cap_aero->name, cap_aero->ngram.vocab_count, cap_aero->ngram.transition_count,
           (unsigned long long)cap_aero->digest);

    /* -------------------------------------------------------------
     * [2/5] Testing Domain Isolation & Out-of-Domain Abstention
     * ------------------------------------------------------------- */
    printf("\n[2/5] Testing Domain Isolation & Fail-Closed Abstention...\n");

    float query_nature[CNET_VSA_DEFAULT_DIM];
    encode_text("curious fox exploring trees in the forest", query_nature, CNET_VSA_DEFAULT_DIM);

    float query_aero[CNET_VSA_DEFAULT_DIM];
    encode_text("spacecraft velocity propulsion engine orbit telemetry", query_aero, CNET_VSA_DEFAULT_DIM);

    char gen_buf[512];
    int toks_out = 0;

    /* In-Domain for Capsule A */
    int rc1 = cnet_vsa_gencap_generate(cap_nature, "the", query_nature, 0.45f, 20, gen_buf, sizeof(gen_buf), &toks_out);
    assert(rc1 >= 0);
    printf("  Capsule A in-domain generation: \"%s\" (tokens=%d)\n", gen_buf, toks_out);

    /* Out-of-Domain for Capsule A -> Must Abstain! */
    int rc2 = cnet_vsa_gencap_generate(cap_nature, "the", query_aero, 0.45f, 20, gen_buf, sizeof(gen_buf), &toks_out);
    assert(rc2 == -2);
    assert(strstr(gen_buf, "ABSTAIN") != NULL);
    printf("  Capsule A OOD refusal: \"%s\" [REFUSED AS EXPECTED]\n", gen_buf);

    /* In-Domain for Capsule B */
    int rc3 = cnet_vsa_gencap_generate(cap_aero, "the", query_aero, 0.45f, 20, gen_buf, sizeof(gen_buf), &toks_out);
    assert(rc3 >= 0);
    printf("  Capsule B in-domain generation: \"%s\" (tokens=%d)\n", gen_buf, toks_out);

    /* Out-of-Domain for Capsule B -> Must Abstain! */
    int rc4 = cnet_vsa_gencap_generate(cap_aero, "the", query_nature, 0.45f, 20, gen_buf, sizeof(gen_buf), &toks_out);
    assert(rc4 == -2);
    assert(strstr(gen_buf, "ABSTAIN") != NULL);
    printf("  Capsule B OOD refusal: \"%s\" [REFUSED AS EXPECTED]\n", gen_buf);

    /* -------------------------------------------------------------
     * [3/5] Testing Portable Binary Serialization & Integrity Check
     * ------------------------------------------------------------- */
    printf("\n[3/5] Testing Portable Binary Export & Cryptographic Verification...\n");

    const char *cap_a_path = "bin/test_nature.gencap";
    assert(cnet_vsa_gencap_save(cap_nature, cap_a_path) == 0);

    /* Load into clean capsule */
    assert(cnet_vsa_gencap_load(loaded_cap, cap_a_path) == 0);
    assert(loaded_cap->certified == 1);
    assert(loaded_cap->digest == cap_nature->digest);
    assert(strcmp(loaded_cap->name, cap_nature->name) == 0);
    printf("  Capsule successfully reloaded from '%s' with verified digest 0x%llx\n",
           cap_a_path, (unsigned long long)loaded_cap->digest);

    /* Tamper detection: flip a single byte in file and verify load refusal */
    FILE *fp = fopen(cap_a_path, "r+b");
    assert(fp != NULL);
    fseek(fp, 128, SEEK_SET);
    char byte_val = 0;
    size_t nr = fread(&byte_val, 1, 1, fp);
    assert(nr == 1);
    byte_val ^= 0xFF; /* flip bits */
    fseek(fp, 128, SEEK_SET);
    fwrite(&byte_val, 1, 1, fp);
    fclose(fp);

    int tamper_rc = cnet_vsa_gencap_load(tampered_cap, cap_a_path);
    assert(tamper_rc != 0); /* Must refuse load! */
    printf("  Tampered capsule load attempt: rc=%d (Refused corrupt/modified payload) PASS\n", tamper_rc);

    /* -------------------------------------------------------------
     * [4/5] Autonomous Generation Throughput (Zero Neural Model)
     * ------------------------------------------------------------- */
    printf("\n[4/5] Measuring Autonomous Generation Throughput...\n");

    int test_cycles = 500;
    int total_tokens = 0;
    double t_start = get_time_us();

    for (int i = 0; i < test_cycles; ++i) {
        int gen_toks = 0;
        cnet_vsa_gencap_generate(cap_aero, "the", NULL, 0.0f, 15, gen_buf, sizeof(gen_buf), &gen_toks);
        total_tokens += gen_toks;
    }
    double total_us = get_time_us() - t_start;
    double tok_per_sec = (double)total_tokens / (total_us / 1000000.0);
    double us_per_tok = total_us / (double)total_tokens;

    printf("  Synthesized %d tokens in %.2f ms (%.2f us/token)\n",
           total_tokens, total_us / 1000.0, us_per_tok);
    printf("  Autonomous Generation Throughput: %.0f tokens/sec (Pure VSA, Zero LLM)\n",
           tok_per_sec);
    assert(tok_per_sec > 4000.0);

    /* -------------------------------------------------------------
     * [5/5] Syntactic Cleanliness & Punctuation Invariants
     * ------------------------------------------------------------- */
    printf("\n[5/5] Verifying Syntactic Termination Invariants...\n");
    for (int i = 0; i < 5; ++i) {
        cnet_vsa_gencap_generate(cap_nature, "once", query_nature, 0.40f, 22, gen_buf, sizeof(gen_buf), &toks_out);
        size_t len = strlen(gen_buf);
        assert(len > 0);
        assert(gen_buf[0] >= 'A' && gen_buf[0] <= 'Z'); /* Capitalized */
        char term = gen_buf[len - 1];
        assert(term == '.' || term == '!' || term == '?'); /* Valid punctuation */
    }
    printf("  100%% of generated utterances start capitalized and terminate cleanly with punctuation PASS\n");

    printf("\n=================================================================\n");
    printf(" CNET_VSA_GENCAP_BENCH_PASS: All 5 validation gates passed cleanly\n");
    printf("=================================================================\n");

    /* Cleanup temporary test file and heap allocations */
    remove(cap_a_path);
    free(cap_nature);
    free(cap_aero);
    free(loaded_cap);
    free(tampered_cap);
    return 0;
}
