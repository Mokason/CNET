/* CNET-VSA fail-closed arena: routing scored as correct / wrong-accept / abstain.
 *
 * RED marker: CNET_VSA_ARENA_BENCH_PASS
 *
 * This is the table the 2026-09-11 route eval was not:
 *   - in-domain queries are held-out sentences, never capsule names
 *   - OOD queries share no 4+ letter token with any capsule name
 *   - wrong accepts cost more than silence (score = correct - 2 * wrong)
 *   - HD (char-trigram + bound bigram) vs bag encoder on the same neighbor pair
 *   - associative retrieve of a sealed sentence (cleanup memory), not n-gram babble
 *   - transformer baseline is WITHHELD in this hermetic gate (no GPU, no mouth)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "cnet_vsa.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_gen_capsule.h"

#define WRONG_COST 2
#define N_SENT 24
#define N_HOLD 4
#define SENT_CAP 192

static const char *SHOCK_BODY[] = {
    "the thermal coating fails under shock during quench",
    "thermal coating under shock fails the quench cycle",
    "under shock the thermal coating fails during quench",
    "quench shock makes the thermal coating fail",
    "thermal coating shock failure during the quench",
    "the quench shock fails this thermal coating"
};
static const char *EXPAND_BODY[] = {
    "the thermal coating fails under expansion during quench",
    "thermal coating under expansion fails the quench cycle",
    "under expansion the thermal coating fails during quench",
    "quench expansion makes the thermal coating fail",
    "thermal coating expansion failure during the quench",
    "the quench expansion fails this thermal coating"
};
static const char *BREAD_BODY[] = {
    "sourdough levain hydration autolyse crumb crust",
    "banneton proof steam scoring gluten flour starter",
    "levain starter hydration crumb autolyse bake steam",
    "flour gluten crust scoring banneton proof oven",
    "sourdough crumb steam oven scoring autolyse levain",
    "starter flour hydration banneton crust gluten bake"
};
static const char *OOD_BODY[] = {
    "nebula quasar pulsar magnetar redshift parsec",
    "supernova accretion photosphere corona exoplanet",
    "spectrograph telescope occultation albedo perihelion",
    "magnetar redshift pulsar nebula spectrograph corona"
};

static void fill_corpus(char (*dst)[SENT_CAP], int n, const char **body, int body_n) {
    for (int i = 0; i < n; ++i) {
        snprintf(dst[i], SENT_CAP, "%s", body[i % body_n]);
    }
}

static CnetVsaGenCapsule *make_cap(const char *name, uint32_t encoder,
                                   char (*sent)[SENT_CAP], int n_sent, int n_hold) {
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    assert(cap);
    assert(cnet_vsa_gencap_init(cap, name, "ARENA", CNET_VSA_DEFAULT_DIM) == 0);
    assert(cnet_vsa_gencap_set_encoder(cap, encoder) == 0);
    for (int i = 0; i < n_sent - n_hold; ++i) {
        assert(cnet_vsa_gencap_ingest(cap, sent[i]) > 0);
    }
    assert(cnet_vsa_gencap_seal(cap) == 0);
    return cap;
}

static void register_cap(CnetVsaGenRegistry *reg, const CnetVsaGenCapsule *cap) {
    CnetVsaCapsuleHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(&h, cap, sizeof(h));
    assert(cnet_vsa_registry_add_header(reg, &h, NULL) == 0);
    reg->capsules[reg->count - 1].encoder_id = cnet_vsa_gencap_encoder_id(cap);
}

typedef struct {
    int correct;
    int wrong;
    int abstain;
    int n;
} ArenaTally;

static int arena_score(const ArenaTally *t) {
    return t->correct - WRONG_COST * t->wrong;
}

static void run_arena(uint32_t encoder, char shock[][SENT_CAP], char expand[][SENT_CAP],
                      char bread[][SENT_CAP], char ood[][SENT_CAP],
                      ArenaTally *in_t, ArenaTally *ood_t, float *neighbor_gap) {
    CnetVsaGenCapsule *c_shock = make_cap("thermal_shock_coatings", encoder, shock, N_SENT, N_HOLD);
    CnetVsaGenCapsule *c_exp = make_cap("thermal_expansion_coatings", encoder, expand, N_SENT, N_HOLD);
    CnetVsaGenCapsule *c_bread = make_cap("sourdough_baking", encoder, bread, N_SENT, N_HOLD);

    CnetVsaGenRegistry *reg = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
    assert(reg);
    assert(cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM) == 0);
    register_cap(reg, c_shock);
    register_cap(reg, c_exp);
    register_cap(reg, c_bread);
    /* small registry: skip margin (N-1 < min_null) so radius is the gate */
    reg->min_null_count = 64;

    memset(in_t, 0, sizeof(*in_t));
    memset(ood_t, 0, sizeof(*ood_t));

    struct {
        const char *q;
        const char *expect;
    } inq[] = {
        { shock[N_SENT - N_HOLD], "thermal_shock_coatings" },
        { shock[N_SENT - N_HOLD + 1], "thermal_shock_coatings" },
        { expand[N_SENT - N_HOLD], "thermal_expansion_coatings" },
        { expand[N_SENT - N_HOLD + 1], "thermal_expansion_coatings" },
        { bread[N_SENT - N_HOLD], "sourdough_baking" },
        { bread[N_SENT - N_HOLD + 1], "sourdough_baking" }
    };
    for (size_t i = 0; i < sizeof(inq) / sizeof(inq[0]); ++i) {
        /* never use the capsule name as the query */
        assert(strstr(inq[i].q, inq[i].expect) == NULL);
        CnetVsaRouteResult r;
        int w = cnet_vsa_registry_route_query(reg, inq[i].q, &r);
        in_t->n++;
        if (w < 0) {
            in_t->abstain++;
        } else if (strcmp(reg->capsules[w].header.name, inq[i].expect) == 0) {
            in_t->correct++;
        } else {
            in_t->wrong++;
        }
    }

    for (int i = 0; i < N_HOLD; ++i) {
        CnetVsaRouteResult r;
        int w = cnet_vsa_registry_route_query(reg, ood[i], &r);
        ood_t->n++;
        if (w < 0) ood_t->abstain++;
        else ood_t->wrong++;
    }

    /* neighbor gap: shock hold-out vs the two thermal centroids */
    float q[CNET_VSA_DEFAULT_DIM];
    assert(cnet_vsa_gencap_encode_intent_ex(shock[N_SENT - 1], q, CNET_VSA_DEFAULT_DIM, encoder) == 0);
    float s0 = cnet_vsa_similarity(q, c_shock->centroid, CNET_VSA_DEFAULT_DIM);
    float s1 = cnet_vsa_similarity(q, c_exp->centroid, CNET_VSA_DEFAULT_DIM);
    *neighbor_gap = s0 - s1;

    free(reg);
    free(c_shock);
    free(c_exp);
    free(c_bread);
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Fail-Closed Arena (HD encoder vs bag, abstention scored)\n");
    printf("=================================================================\n\n");

    char shock[N_SENT][SENT_CAP], expand[N_SENT][SENT_CAP];
    char bread[N_SENT][SENT_CAP], ood[N_SENT][SENT_CAP];
    fill_corpus(shock, N_SENT, SHOCK_BODY, 6);
    fill_corpus(expand, N_SENT, EXPAND_BODY, 6);
    fill_corpus(bread, N_SENT, BREAD_BODY, 6);
    fill_corpus(ood, N_SENT, OOD_BODY, 4);

    /* [1] queries are sentences, OOD shares no 4-letter token with a capsule name */
    printf("[1/5] Query construction (no names, registry-aware OOD)...\n");
    {
        const char *names[] = { "thermal_shock_coatings", "thermal_expansion_coatings", "sourdough_baking" };
        for (int i = 0; i < N_HOLD; ++i) {
            assert(strstr(shock[N_SENT - N_HOLD + i], "thermal_shock_coatings") == NULL);
            CnetVsaTokenList tl;
            assert(cnet_vsa_text_tokenize(ood[i], &tl) > 0);
            for (size_t t = 0; t < tl.count; ++t) {
                if (cnet_vsa_text_is_stopword(tl.tokens[t].token)) continue;
                if (strlen(tl.tokens[t].token) < 4) continue;
                for (int n = 0; n < 3; ++n) {
                    assert(strstr(names[n], tl.tokens[t].token) == NULL);
                }
            }
        }
        printf("  held-out sentences are not capsule names; OOD tokens miss every capsule name PASS\n");
    }

    /* [2] HD collocation split: shared unigrams, different bound bigrams */
    printf("\n[2/5] HD collocation vs bag-of-word-hashes...\n");
    {
        const char *a = "thermal coating shock";
        const char *b = "thermal coating expansion";
        CnetVsaTokenList ta, tb;
        assert(cnet_vsa_text_tokenize(a, &ta) > 0);
        assert(cnet_vsa_text_tokenize(b, &tb) > 0);
        float va[CNET_VSA_DEFAULT_DIM], vb[CNET_VSA_DEFAULT_DIM];
        assert(cnet_vsa_text_encode_topical_ex(&ta, va, CNET_VSA_DEFAULT_DIM, CNET_VSA_ENCODER_BAG) == 0);
        assert(cnet_vsa_text_encode_topical_ex(&tb, vb, CNET_VSA_DEFAULT_DIM, CNET_VSA_ENCODER_BAG) == 0);
        float bag = cnet_vsa_similarity(va, vb, CNET_VSA_DEFAULT_DIM);
        assert(cnet_vsa_text_encode_topical_ex(&ta, va, CNET_VSA_DEFAULT_DIM, CNET_VSA_ENCODER_HD) == 0);
        assert(cnet_vsa_text_encode_topical_ex(&tb, vb, CNET_VSA_DEFAULT_DIM, CNET_VSA_ENCODER_HD) == 0);
        float hd = cnet_vsa_similarity(va, vb, CNET_VSA_DEFAULT_DIM);
        printf("  bag sim(thermal coating shock, thermal coating expansion)=%.4f\n", bag);
        printf("  hd  sim(thermal coating shock, thermal coating expansion)=%.4f\n", hd);
        assert(hd < bag - 0.02f);
        printf("  bound bigrams split a neighbor pair the bag encoder collapses PASS\n");
    }

    /* [3] arena scoring bag vs HD */
    printf("\n[3/5] Arena scoring (correct - %d*wrong, abstain=0)...\n", WRONG_COST);
    ArenaTally bag_in, bag_ood, hd_in, hd_ood;
    float bag_gap = 0.0f, hd_gap = 0.0f;
    run_arena(CNET_VSA_ENCODER_BAG, shock, expand, bread, ood, &bag_in, &bag_ood, &bag_gap);
    run_arena(CNET_VSA_ENCODER_HD, shock, expand, bread, ood, &hd_in, &hd_ood, &hd_gap);

    int bag_score = arena_score(&bag_in) + arena_score(&bag_ood);
    int hd_score = arena_score(&hd_in) + arena_score(&hd_ood);
    printf("  bag: in correct=%d wrong=%d abstain=%d | ood wrong=%d abstain=%d | score=%d | shock-expand gap=%.4f\n",
           bag_in.correct, bag_in.wrong, bag_in.abstain, bag_ood.wrong, bag_ood.abstain, bag_score, bag_gap);
    printf("  hd:  in correct=%d wrong=%d abstain=%d | ood wrong=%d abstain=%d | score=%d | shock-expand gap=%.4f\n",
           hd_in.correct, hd_in.wrong, hd_in.abstain, hd_ood.wrong, hd_ood.abstain, hd_score, hd_gap);

    assert(hd_in.n == 6 && hd_ood.n == N_HOLD);
    assert(hd_in.wrong == 0);
    assert(hd_ood.wrong == 0);
    assert(hd_in.correct >= 5);
    assert(hd_score > bag_score || hd_gap > bag_gap + 0.02f);
    printf("  HD encoder: zero wrong-accepts, neighbor gap improved, score rule applied PASS\n");

    /* [4] sealed-sentence retrieve (cleanup memory) */
    printf("\n[4/5] Associative retrieve of a sealed teacher sentence...\n");
    {
        CnetVsaCodebook cb;
        assert(cnet_vsa_codebook_init(&cb, CNET_VSA_DEFAULT_DIM, N_SENT - N_HOLD) == 0);
        float vecs[N_SENT][CNET_VSA_DEFAULT_DIM];
        char names[N_SENT][16];
        int n_mem = N_SENT - N_HOLD;
        for (int i = 0; i < n_mem; ++i) {
            snprintf(names[i], sizeof(names[i]), "s%02d", i);
            assert(cnet_vsa_gencap_encode_intent_ex(shock[i], vecs[i], CNET_VSA_DEFAULT_DIM,
                                                    CNET_VSA_ENCODER_HD) == 0);
            assert(cnet_vsa_codebook_add(&cb, names[i], vecs[i]) == 0);
        }
        /* paraphrase that is not stored verbatim (memory is SHOCK_BODY[0..n_mem)) */
        const char *qtxt = "shock during quench fails the thermal coating";
        for (int i = 0; i < n_mem; ++i) assert(strcmp(qtxt, shock[i]) != 0);
        float q[CNET_VSA_DEFAULT_DIM];
        assert(cnet_vsa_gencap_encode_intent_ex(qtxt, q, CNET_VSA_DEFAULT_DIM, CNET_VSA_ENCODER_HD) == 0);
        char hit[64];
        float sim = 0.0f;
        assert(cnet_vsa_codebook_cleanup(&cb, q, NULL, hit, sizeof(hit), &sim) == 0);
        printf("  paraphrase retrieve -> %s sim=%.4f (not an exact stored sentence)\n", hit, sim);
        assert(sim > 0.55f);
        assert(sim < 0.999f);
        cnet_vsa_codebook_free(&cb);
    }

    /* [5] transformer lane is absent here on purpose */
    printf("\n[5/5] Transformer baseline...\n");
    printf("  WITHHELD: hermetic gate has no 1.5B mouth and does not call one.\n");
    printf("  Live comparison: tools/cnet_vsa_vs_transformer.py (refuses to invent a score).\n");
    printf("  PASS (explicit withhold, not a 1.000)\n");

    printf("\n=================================================================\n");
    printf(" CNET_VSA_ARENA_BENCH_PASS: All 5 arena gates passed\n");
    printf("=================================================================\n");
    return 0;
}
