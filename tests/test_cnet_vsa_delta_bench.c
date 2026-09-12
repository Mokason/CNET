/* Delta-rule capsule memory gate. Marker: CNET_VSA_DELTA_BENCH_PASS
 *  1. Capacity: d/2 random unit keys with random values. After 4 passes at
 *     beta 1 every read ranks its own value first (top-1 recall d/2 of d/2)
 *     and after 8 passes the reads are within cosine 0.99 of the stored
 *     values (measured: 8 passes reach 0.994 at d/2; at n = d the system is
 *     near-singular and cosine converges slowly, top-1 stays 100%). The
 *     Hebbian bundle of the same pairs cannot rank all of them first.
 *  2. From a sealed v4 capsule: the derived memory's next-word recall on the
 *     capsule's own sentences is within 10% of the explicit table scan (which
 *     is the data itself) and far above the Hebbian bundle.
 *  3. Generation with mem=delta produces text and refuses off-domain exactly
 *     like the default path (the gate is unchanged). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <sys/stat.h>
#include "cnet_vsa.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_delta.h"
#include "cnet_vsa_gen_capsule.h"

static uint64_t g = 0x2545F4914F6CDD1DULL;
static uint32_t rnd(void) { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return (uint32_t)g; }
static void randvec(float *v, int d) { double n = 0; for (int i = 0; i < d; ++i) { v[i] = (rnd() & 1) ? 1.0f : -1.0f; n += 1.0; } for (int i = 0; i < d; ++i) v[i] /= (float)sqrt(n); }
static float cosv(const float *a, const float *b, int d) { double x = 0, na = 0, nb = 0; for (int i = 0; i < d; ++i) { x += a[i] * b[i]; na += a[i] * a[i]; nb += b[i] * b[i]; } return (float)(x / (sqrt(na) * sqrt(nb) + 1e-12)); }

static const char *W[] = { "kiln", "glaze", "firing", "cone", "clay", "bisque", "reduction", "oxidation", "silica", "feldspar", "flux", "crazing", "shivering", "pinholing", "body", "slip", "engobe", "ash", "celadon", "tenmoku" };

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Delta-Rule Capsule Memory Bench\n");
    printf("=================================================================\n\n");
    const int D = CNET_VSA_DEFAULT_DIM;

    printf("[1/3] Exact recall of %d random pairs in a %dx%d delta memory vs the Hebbian bundle\n", D / 2, D, D);
    {
        CnetVsaDeltaMemory m; assert(cnet_vsa_delta_init(&m, D) == 0);
        int n = D / 2;
        float *K = malloc(sizeof(float) * n * D), *V = malloc(sizeof(float) * n * D); assert(K && V);
        float bundle[CNET_VSA_DEFAULT_DIM]; memset(bundle, 0, sizeof(bundle));
        for (int i = 0; i < n; ++i) { randvec(K + i * D, D); randvec(V + i * D, D); }
        for (int i = 0; i < n; ++i) { float b[CNET_VSA_DEFAULT_DIM]; cnet_vsa_bind(b, K + i * D, V + i * D, D); for (int d = 0; d < D; ++d) bundle[d] += b[d]; }
        int top1_4 = 0, ok_delta = 0, ok_bundle = 0; float min_delta = 2.0f, mean_bundle = 0.0f;
        for (int pass = 0; pass < 8; ++pass) {
            for (int i = 0; i < n; ++i) cnet_vsa_delta_write(&m, K + i * D, V + i * D, 1.0f);
            if (pass == 3) {
                for (int i = 0; i < n; ++i) { float out[CNET_VSA_DEFAULT_DIM]; cnet_vsa_delta_read(&m, K + i * D, out);
                    float c = cosv(out, V + i * D, D); int best = 1;
                    for (int j = 0; j < n && best; ++j) if (j != i && cosv(out, V + j * D, D) >= c) best = 0;
                    top1_4 += best; }
            }
        }
        for (int i = 0; i < n; ++i) {
            float out[CNET_VSA_DEFAULT_DIM]; cnet_vsa_delta_read(&m, K + i * D, out);
            float c = cosv(out, V + i * D, D); if (c < min_delta) min_delta = c; ok_delta += c > 0.99f;
            float u[CNET_VSA_DEFAULT_DIM]; cnet_vsa_unbind(u, bundle, K + i * D, D);
            /* the bundle's read must at least pick its own value over the others to count */
            float cb = cosv(u, V + i * D, D); mean_bundle += cb; int best = 1;
            for (int j = 0; j < n && best; ++j) if (j != i && cosv(u, V + j * D, D) >= cb) best = 0;
            ok_bundle += best;
        }
        printf("  delta: top-1 recall %d/%d after 4 passes; %d/%d reads within cosine 0.99 after 8 (min %.4f). bundle: %d/%d reads rank their own value first (mean cosine %.3f)\n",
               top1_4, n, ok_delta, n, min_delta, ok_bundle, n, mean_bundle / n);
        assert(top1_4 == n);
        assert(ok_delta == n);
        assert(ok_bundle < n);   /* crosstalk: the Hebbian bundle cannot hold d/2 pairs cleanly */
        cnet_vsa_delta_free(&m); free(K); free(V);
    }
    printf("  PASS\n");

    printf("\n[2/3] Derived from a sealed v4 capsule: recall vs the table scan\n");
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule)); assert(cap);
    {
        assert(cnet_vsa_gencap_init(cap, "delta_topic", "TEST", D) == 0);
        assert(cnet_vsa_gencap_set_encoder(cap, CNET_VSA_ENCODER_STEM) == 0);
        char sent[256];
        for (int i = 0; i < 60; ++i) {
            /* 8-word sentences over a 20-word vocabulary: enough repeated contexts to make recall non-trivial */
            snprintf(sent, sizeof(sent), "the %s %s controls the %s during %s and %s firing.", W[rnd() % 20], W[rnd() % 20], W[rnd() % 20], W[rnd() % 20], W[rnd() % 20]);
            assert(cnet_vsa_gencap_ingest(cap, sent) > 0);
        }
        assert(cnet_vsa_gencap_seal(cap) == 0 && cap->passages.count == 60);
        CnetVsaDeltaMemory m; memset(&m, 0, sizeof(m));
        int pairs = cnet_vsa_delta_build_from_capsule(&m, cap, 4, 0.5f);
        assert(pairs > 100);
        const char *own[CNET_VSA_PASSAGE_MAX];
        for (uint32_t i = 0; i < cap->passages.count; ++i) own[i] = cap->passages.text + cap->passages.offset[i];
        CnetVsaDeltaRecall r;
        assert(cnet_vsa_delta_recall(&m, cap, own, cap->passages.count, &r) > 0);
        printf("  %d pairs, built in %.1f ms; recall top-1: delta %.1f%%  table %.1f%%  bundle %.1f%%; read us: delta %.1f  table %.1f  bundle %.1f\n",
               pairs, m.build_ms, 100.0 * r.delta_top1 / r.positions, 100.0 * r.table_top1 / r.positions, 100.0 * r.bundle_top1 / r.positions,
               r.delta_read_us, r.table_read_us, r.bundle_read_us);
        /* in-sample the explicit table is the data itself and cannot be beaten; the derived memory must stay
         * within 10% of it and must beat the Hebbian bundle by a wide margin (production: 83.7 / 84.2 / 2.2) */
        assert(10 * r.delta_top1 >= 9 * r.table_top1);
        assert(r.delta_top1 > r.bundle_top1);
        cnet_vsa_delta_free(&m);
    }
    printf("  PASS\n");

    printf("\n[3/3] Generation with the delta memory: text in domain, refusal out of domain, same gate\n");
    {
        char out[1024]; int toks = 0;
        float iv[CNET_VSA_DEFAULT_DIM];
        assert(cnet_vsa_gencap_encode_intent_ex("glaze firing kiln cone reduction", iv, D, CNET_VSA_ENCODER_STEM) == 0);
        int rc = cnet_vsa_gencap_generate_mem(cap, "the", iv, NULL, 0.45f, 20, CNET_VSA_GEN_MEM_DELTA, out, sizeof(out), &toks);
        assert(rc == 0 && toks >= 5);
        printf("  delta-only: \"%.90s\" (%d tokens)\n", out, toks);
        rc = cnet_vsa_gencap_generate_mem(cap, "the", iv, NULL, 0.45f, 20, CNET_VSA_GEN_MEM_DEFAULT | CNET_VSA_GEN_MEM_DELTA, out, sizeof(out), &toks);
        assert(rc == 0 && toks >= 5);
        assert(cnet_vsa_gencap_encode_intent_ex("sourdough levain crumb hydration", iv, D, CNET_VSA_ENCODER_STEM) == 0);
        rc = cnet_vsa_gencap_generate_mem(cap, "the", iv, NULL, 0.45f, 20, CNET_VSA_GEN_MEM_DELTA, out, sizeof(out), &toks);
        assert(rc == -2 && toks == 0);
        printf("  off-domain prompt refused (rc=%d) with the delta memory selected PASS\n", rc);
    }
    free(cap);
    printf("\n=================================================================\n");
    printf(" CNET_VSA_DELTA_BENCH_PASS\n");
    printf("=================================================================\n");
    return 0;
}
