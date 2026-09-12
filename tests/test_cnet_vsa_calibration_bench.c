/* CNET-VSA per-capsule radius calibration + registry margin gate bench.
 *
 * RED marker: CNET_VSA_CALIBRATION_BENCH_PASS
 *
 * Proves:
 *  1. A capsule calibrated against held-out in-domain probes and cross-domain
 *     negatives receives a radius that meets both targets, with a receipt.
 *  2. A capsule whose negatives are indistinguishable from its corpus is
 *     refused at seal time (not separable) and cannot be saved.
 *  3. The calibration receipt is covered by the digest (tamper -> load fails).
 *  4. Version-1 capsules (no receipt) still load; re-sealing upgrades them.
 *  5. The registry margin gate refuses a query whose best match is only a
 *     random maximum over many unrelated capsules, and still routes a
 *     genuine in-domain query, with the decision explained.
 *  6. Version-3 wide binary topical block: receipt, digest coverage, width
 *     check, legacy v2 loading, binary-space routing and float fallback.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <math.h>
#include <unistd.h>
#include <sys/types.h>
#include "cnet_vsa.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_gen_capsule.h"

/* ---- synthetic corpora ------------------------------------------------- */

static const char *POOL_A[] = {
    "wavefront", "lds", "coalescing", "hip", "kernel", "occupancy", "vgpr", "sgpr",
    "workgroup", "barrier", "rocm", "gfx", "dispatch", "latency", "bandwidth", "cache"
};
static const char *POOL_B[] = {
    "sourdough", "levain", "crumb", "hydration", "autolyse", "proof", "oven", "crust",
    "flour", "gluten", "starter", "bake", "scoring", "steam", "banneton", "yeast"
};
static const char *POOL_C[] = {
    "arbitration", "clause", "warranty", "indemnity", "liability", "tribunal", "breach",
    "remedy", "counsel", "statute", "jurisdiction", "damages", "contract", "notice", "party", "waiver"
};
static const char *POOL_Z[] = {
    "nebula", "quasar", "pulsar", "magnetar", "redshift", "parsec", "supernova", "accretion",
    "photosphere", "corona", "exoplanet", "spectrograph", "telescope", "occultation", "albedo", "perihelion"
};

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng & 0xffffffffu);
}

static void make_sentence(const char **pool, int pool_n, int words, char *out, size_t cap) {
    size_t off = 0;
    for (int i = 0; i < words && off + 24 < cap; ++i) {
        const char *w = pool[rnd() % (uint32_t)pool_n];
        int n = snprintf(out + off, cap - off, "%s%s", (i ? " " : ""), w);
        if (n < 0) break;
        off += (size_t)n;
    }
    snprintf(out + off, cap - off, ".");
}

#define N_SENT 40
#define N_PROBE 12
#define N_NEG 80
#define SENT_CAP 256

typedef struct {
    char sent[N_SENT][SENT_CAP];
    const char *sent_p[N_SENT];
    char probe[N_PROBE][SENT_CAP];
    const char *probe_p[N_PROBE];
} Corpus;

static void build_corpus(Corpus *c, const char **pool, int pool_n) {
    for (int i = 0; i < N_SENT; ++i) {
        make_sentence(pool, pool_n, 8 + (int)(rnd() % 5), c->sent[i], SENT_CAP);
        c->sent_p[i] = c->sent[i];
    }
    for (int i = 0; i < N_PROBE; ++i) {
        make_sentence(pool, pool_n, 3 + (int)(rnd() % 3), c->probe[i], SENT_CAP);
        c->probe_p[i] = c->probe[i];
    }
}

static CnetVsaGenCapsule *build_capsule(const char *name, const Corpus *c) {
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    assert(cap);
    assert(cnet_vsa_gencap_init(cap, name, "TEST", CNET_VSA_DEFAULT_DIM) == 0);
    for (int i = 0; i < N_SENT; ++i) assert(cnet_vsa_gencap_ingest(cap, c->sent[i]) > 0);
    return cap;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Per-Capsule Calibration & Margin Gate Benchmark        \n");
    printf("=================================================================\n\n");

    Corpus *A = (Corpus *)calloc(1, sizeof(Corpus));
    Corpus *B = (Corpus *)calloc(1, sizeof(Corpus));
    Corpus *C = (Corpus *)calloc(1, sizeof(Corpus));
    Corpus *Z = (Corpus *)calloc(1, sizeof(Corpus));
    assert(A && B && C && Z);
    build_corpus(A, POOL_A, 16);
    build_corpus(B, POOL_B, 16);
    build_corpus(C, POOL_C, 16);
    build_corpus(Z, POOL_Z, 16);

    /* negatives for A = sentences of B and C */
    const char *neg_for_a[N_NEG];
    for (int i = 0; i < N_SENT; ++i) { neg_for_a[i] = B->sent[i]; neg_for_a[N_SENT + i] = C->sent[i]; }

    /* [1/5] calibrated seal meets both targets */
    printf("[1/5] Calibrating capsule A against held-out probes and cross-domain negatives...\n");
    CnetVsaGenCapsule *capA = build_capsule("cap_a_gpu", A);
    float target_in = 0.90f, target_neg = 0.95f;
    int rc = cnet_vsa_gencap_calibrate(capA, A->sent_p, N_SENT, A->probe_p, N_PROBE,
                                       neg_for_a, N_NEG, target_in, target_neg);
    printf("  calibrate rc=%d radius=%.4f r_in=%.4f r_neg=%.4f in_accept=%.3f neg_reject=%.3f (n_in=%u n_neg=%u)\n",
           rc, capA->safe_radius, capA->calib.radius_in, capA->calib.radius_neg,
           capA->calib.in_accept_rate, capA->calib.neg_reject_rate,
           capA->calib.in_domain_count, capA->calib.negative_count);
    assert(rc == 0);
    assert(capA->calib.calibrated == 1);
    assert(capA->safe_radius > 0.0f && capA->safe_radius < 1.0f);
    assert(capA->calib.radius_in <= capA->calib.radius_neg);
    assert(capA->calib.in_accept_rate >= target_in - 1e-6f);
    assert(capA->calib.neg_reject_rate >= target_neg - 1e-6f);
    assert(capA->calib.in_domain_count == N_SENT + N_PROBE);
    assert(capA->calib.negative_count == N_NEG);
    assert(cnet_vsa_gencap_seal(capA) == 0);
    assert(capA->version == CNET_VSA_GENCAP_VERSION);
    assert(capA->certified == 1);
    /* the calibrated radius must be tighter than the legacy 0.90 default */
    assert(capA->safe_radius < 0.90f);
    printf("  Sealed calibrated capsule A: radius %.4f (legacy default was 0.900) PASS\n", capA->safe_radius);

    /* held-out probe from Z must be refused by the capsule's own scope gate */
    {
        float qz[CNET_VSA_DEFAULT_DIM], qa[CNET_VSA_DEFAULT_DIM], dz = 0, da = 0;
        assert(cnet_vsa_gencap_encode_intent(Z->probe[0], qz, CNET_VSA_DEFAULT_DIM) == 0);
        assert(cnet_vsa_gencap_encode_intent(A->probe[0], qa, CNET_VSA_DEFAULT_DIM) == 0);
        assert(cnet_vsa_gencap_verify_scope(capA, qz, &dz) == 0);
        assert(cnet_vsa_gencap_verify_scope(capA, qa, &da) == 1);
        printf("  Scope gate: in-domain probe dist=%.4f accepted, alien probe dist=%.4f refused PASS\n", da, dz);
    }

    /* [2/5] not separable -> refuse seal and save */
    printf("\n[2/5] Refusing a capsule whose negatives are its own domain (not separable)...\n");
    CnetVsaGenCapsule *capD = build_capsule("cap_d_dup", B);
    const char *neg_same[N_SENT];
    for (int i = 0; i < N_SENT; ++i) neg_same[i] = B->sent[i];
    rc = cnet_vsa_gencap_calibrate(capD, B->sent_p, N_SENT, B->probe_p, N_PROBE,
                                   neg_same, N_SENT, target_in, target_neg);
    printf("  calibrate rc=%d r_in=%.4f r_neg=%.4f separation=%.4f\n",
           rc, capD->calib.radius_in, capD->calib.radius_neg, capD->calib.separation);
    assert(rc == CNET_VSA_GENCAP_NOT_SEPARABLE);
    assert(capD->calib.calibrated == 0);
    assert(capD->calib.separation < 0.0f);
    assert(cnet_vsa_gencap_seal(capD) == CNET_VSA_GENCAP_NOT_SEPARABLE);
    assert(capD->certified == 0);
    assert(cnet_vsa_gencap_save(capD, "/tmp/cnet_vsa_calib_should_not_exist.gencap") != 0);
    assert(access("/tmp/cnet_vsa_calib_should_not_exist.gencap", F_OK) != 0);
    printf("  Non-separable capsule refused at seal and at save PASS\n");

    /* insufficient evidence must also refuse */
    CnetVsaGenCapsule *capE = build_capsule("cap_e_thin", C);
    rc = cnet_vsa_gencap_calibrate(capE, C->sent_p, N_SENT, NULL, 0, neg_for_a, 3, target_in, target_neg);
    assert(rc == CNET_VSA_GENCAP_INSUFFICIENT_EVIDENCE);
    assert(capE->calib.calibrated == 0);
    printf("  Calibration with 3 negatives refused as insufficient evidence PASS\n");

    /* [3/5] receipt is inside the digest */
    printf("\n[3/5] Round-trip and tamper detection of the calibration receipt...\n");
    const char *pathA = "/tmp/cnet_vsa_calib_a.gencap";
    assert(cnet_vsa_gencap_save(capA, pathA) == 0);
    CnetVsaGenCapsule *capA2 = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
    assert(capA2);
    assert(cnet_vsa_gencap_load(capA2, pathA) == 0);
    assert(capA2->version == CNET_VSA_GENCAP_VERSION);
    assert(capA2->calib.calibrated == 1);
    assert(fabsf(capA2->safe_radius - capA->safe_radius) < 1e-7f);
    assert(fabsf(capA2->calib.neg_reject_rate - capA->calib.neg_reject_rate) < 1e-7f);
    {
        /* flip the recorded negative reject rate on disk */
        FILE *fp = fopen(pathA, "r+b");
        assert(fp);
        float fake = 0.123f;
        assert(fseek(fp, (long)offsetof(CnetVsaGenCapsule, calib) +
                         (long)offsetof(CnetVsaGencapCalibration, neg_reject_rate), SEEK_SET) == 0);
        assert(fwrite(&fake, sizeof(fake), 1, fp) == 1);
        fclose(fp);
        assert(cnet_vsa_gencap_load(capA2, pathA) == -5);
        assert(capA2->certified == 0);
    }
    remove(pathA);
    printf("  Receipt round-trips; tampering the receipt fails closed (rc=-5) PASS\n");

    /* [4/5] v1 compatibility */
    printf("\n[4/5] Loading a version-1 capsule (no receipt) and upgrading it...\n");
    {
        const char *pathV1 = "/tmp/cnet_vsa_calib_v1.gencap";
        CnetVsaGenCapsule *capV1 = build_capsule("cap_v1_legacy", C);
        assert(cnet_vsa_gencap_seal_legacy_v1(capV1) == 0);
        assert(capV1->version == 1);
        FILE *fp = fopen(pathV1, "wb");
        assert(fp);
        assert(fwrite(capV1, CNET_VSA_GENCAP_V1_SIZE, 1, fp) == 1);
        fclose(fp);
        CnetVsaGenCapsule *capL = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
        assert(capL);
        assert(cnet_vsa_gencap_load(capL, pathV1) == 0);
        assert(capL->version == 1);
        assert(capL->certified == 1);
        assert(capL->calib.calibrated == 0);
        assert(fabsf(capL->safe_radius - 0.90f) < 1e-6f);
        /* a sealed capsule is immutable: calibrating it in place is refused,
         * and a legacy load cannot be re-saved without a reseal */
        const char *neg_for_c[N_NEG];
        for (int i = 0; i < N_SENT; ++i) { neg_for_c[i] = A->sent[i]; neg_for_c[N_SENT + i] = B->sent[i]; }
        assert(cnet_vsa_gencap_calibrate(capL, C->sent_p, N_SENT, C->probe_p, N_PROBE,
                                         neg_for_c, N_NEG, target_in, target_neg) == -1);
        assert(cnet_vsa_gencap_save(capL, "/tmp/cnet_vsa_calib_v1_resave.gencap") == -6);
        /* upgrade = rebuild from the retained corpus with calibration, then overwrite */
        CnetVsaGenCapsule *capU = build_capsule("cap_v1_legacy", C);
        assert(cnet_vsa_gencap_calibrate(capU, C->sent_p, N_SENT, C->probe_p, N_PROBE,
                                         neg_for_c, N_NEG, target_in, target_neg) == 0);
        assert(cnet_vsa_gencap_seal(capU) == 0);
        assert(capU->version == CNET_VSA_GENCAP_VERSION);
        assert(cnet_vsa_gencap_save(capU, pathV1) == 0);
        assert(cnet_vsa_gencap_load(capL, pathV1) == 0);
        assert(capL->version == CNET_VSA_GENCAP_VERSION);
        assert(capL->calib.calibrated == 1);
        assert(capL->safe_radius < 0.90f);
        remove(pathV1);
        free(capV1); free(capL); free(capU);
        printf("  v1 file loads (radius 0.900, uncalibrated); in-place mutation refused; rebuild upgrades to v2 PASS\n");
    }

    /* [5/5] registry margin gate */
    printf("\n[5/5] Registry margin gate over %d unrelated capsules...\n", 400);
    {
        CnetVsaGenRegistry *reg = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
        assert(reg);
        assert(cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM) == 0);

        /* add A (calibrated) by header */
        CnetVsaCapsuleHeader h;
        memset(&h, 0, sizeof(h));
        memcpy(&h, capA, sizeof(h));
        assert(cnet_vsa_registry_add_header(reg, &h, NULL) == 0);

        /* add 400 legacy-radius capsules with random-word centroids */
        static const char *ALL_POOLS[3][16];
        memcpy(ALL_POOLS[0], POOL_B, sizeof(POOL_B));
        memcpy(ALL_POOLS[1], POOL_C, sizeof(POOL_C));
        memcpy(ALL_POOLS[2], POOL_Z, sizeof(POOL_Z));
        for (int k = 0; k < 400; ++k) {
            CnetVsaCapsuleHeader n;
            memset(&n, 0, sizeof(n));
            n.magic = CNET_VSA_GENCAP_MAGIC;
            n.version = CNET_VSA_GENCAP_VERSION;
            snprintf(n.name, sizeof(n.name), "noise_%03d", k);
            snprintf(n.domain, sizeof(n.domain), "NOISE");
            n.certified = 1;
            n.safe_radius = 0.90f; /* legacy default: accepts nearly anything on its own */
            /* centroid = bundle of 12 random pseudo-words unique to this capsule */
            float acc[CNET_VSA_DEFAULT_DIM] = {0};
            for (int w = 0; w < 12; ++w) {
                char word[32];
                snprintf(word, sizeof(word), "nz%dw%u", k, rnd());
                float v[CNET_VSA_DEFAULT_DIM];
                cnet_vsa_text_token_vec(word, v, CNET_VSA_DEFAULT_DIM);
                for (int d = 0; d < CNET_VSA_DEFAULT_DIM; ++d) acc[d] += v[d];
            }
            cnet_vsa_normalize(acc, CNET_VSA_DEFAULT_DIM);
            memcpy(n.centroid, acc, sizeof(acc));
            assert(cnet_vsa_registry_add_header(reg, &n, NULL) == 0);
        }
        assert(reg->count == 401);

        /* in-domain query routes to A with explanation */
        float q[CNET_VSA_DEFAULT_DIM];
        CnetVsaRouteResult r;
        assert(cnet_vsa_gencap_encode_intent(A->probe[1], q, reg->dim) == 0);
        int w = cnet_vsa_registry_route_ex(reg, q, &r);
        printf("  in-domain: winner=%d status=%d best_sim=%.4f dist=%.4f z=%.2f z_min=%.2f gap=%.4f null(mu=%.4f sd=%.4f)\n",
               w, r.status, r.best_sim, r.best_dist, r.z, r.z_min, r.gap, r.null_mean, r.null_std);
        assert(w == 0);
        assert(r.status == CNET_VSA_ROUTE_ACCEPT);
        assert(r.radius_ok && r.margin_ok);
        assert(r.z >= r.z_min);

        /* off-domain query: legacy radius-only gate would accept the random max; margin gate refuses */
        int legacy_accepts = 0, margin_refuses = 0;
        for (int i = 0; i < N_PROBE; ++i) {
            assert(cnet_vsa_gencap_encode_intent(Z->probe[i], q, reg->dim) == 0);
            w = cnet_vsa_registry_route_ex(reg, q, &r);
            if (r.best_idx >= 0 && r.best_dist <= reg->capsules[r.best_idx].header.safe_radius) legacy_accepts++;
            if (w < 0) margin_refuses++;
            if (i < 3)
                printf("  alien[%d]: closest=%s dist=%.4f z=%.2f z_min=%.2f status=%d\n",
                       i, reg->capsules[r.best_idx].header.name, r.best_dist, r.z, r.z_min, r.status);
        }
        printf("  alien probes: legacy radius-only gate accepted %d/%d, margin gate refused %d/%d\n",
               legacy_accepts, N_PROBE, margin_refuses, N_PROBE);
        assert(legacy_accepts == N_PROBE); /* this is the failure mode being fixed */
        assert(margin_refuses == N_PROBE);

        /* wrapper keeps the legacy signature */
        int bi = -1; float bd = 1.0f;
        assert(cnet_vsa_gencap_encode_intent(A->probe[2], q, reg->dim) == 0);
        assert(cnet_vsa_registry_route(reg, q, &bi, &bd) == 0);
        assert(bi == 0);
        free(reg);
        printf("  Margin gate refuses random maxima and keeps in-domain routing PASS\n");
    }

    /* negatives for B = sentences of A and C (used by several v3 checks) */
    const char *neg_for_b[N_NEG];
    for (int i = 0; i < N_SENT; ++i) { neg_for_b[i] = A->sent[i]; neg_for_b[N_SENT + i] = C->sent[i]; }

    /* [6/6] version 3: wide binary topical block */
    printf("\n[6/6] Version-3 wide int8 topical block: receipt, tamper, legacy v2, wide routing...\n");
    {
        assert(capA->version == CNET_VSA_GENCAP_VERSION);
        assert(capA->topical.present == 1);
        assert(capA->topical.width == CNET_VSA_TOPICAL_DIM);
        assert(capA->topical.kind == CNET_VSA_TOPICAL_KIND_Q8);
        assert(capA->topical.q8_norm > 0.0f);
        assert(capA->topical.calibrated == 1);
        assert(capA->topical.safe_radius > 0.0f && capA->topical.safe_radius < 1.0f);
        assert(capA->topical.in_accept_rate >= target_in - 1e-6f);
        assert(capA->topical.neg_reject_rate >= target_neg - 1e-6f);
        printf("  wide receipt: radius=%.4f r_in=%.4f r_neg=%.4f in_accept=%.3f neg_reject=%.3f\n",
               capA->topical.safe_radius, capA->topical.radius_in, capA->topical.radius_neg,
               capA->topical.in_accept_rate, capA->topical.neg_reject_rate);
        /* binary scope gate agrees with the float gate on the same probes */
        int8_t qa[CNET_VSA_TOPICAL_DIM], qz[CNET_VSA_TOPICAL_DIM];
        assert(cnet_vsa_gencap_encode_intent_q8(A->probe[0], qa, cnet_vsa_gencap_encoder_id(capA)) == 0);
        assert(cnet_vsa_gencap_encode_intent_q8(Z->probe[0], qz, cnet_vsa_gencap_encoder_id(capA)) == 0);
        float da = 0, dz = 0;
        assert(cnet_vsa_gencap_verify_scope_q8(capA, qa, &da) == 1);
        assert(cnet_vsa_gencap_verify_scope_q8(capA, qz, &dz) == 0);
        printf("  wide scope: in-domain dist=%.4f accepted, alien dist=%.4f refused PASS\n", da, dz);

        /* file size is the persisted prefix, not sizeof */
        const char *p3 = "/tmp/cnet_vsa_calib_v3.gencap";
        assert(cnet_vsa_gencap_save(capA, p3) == 0);
        {
            FILE *fp = fopen(p3, "rb"); assert(fp);
            fseek(fp, 0, SEEK_END); long len = ftell(fp); fclose(fp);
            assert((size_t)len == CNET_VSA_GENCAP_V3_SIZE);
            assert((size_t)len < sizeof(CnetVsaGenCapsule));
            printf("  v3 file is %ld bytes (persisted prefix; struct is %zu with the build-time accumulator) PASS\n",
                   len, sizeof(CnetVsaGenCapsule));
        }
        CnetVsaGenCapsule *capR = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule));
        assert(capR);
        assert(cnet_vsa_gencap_load(capR, p3) == 0);
        assert(capR->topical.present == 1);
        assert(memcmp(capR->topical.q8, capA->topical.q8, sizeof(capA->topical.q8)) == 0);
        assert(fabsf(capR->topical.safe_radius - capA->topical.safe_radius) < 1e-7f);
        /* tamper one byte of the topical centroid on disk: digest must catch it */
        {
            FILE *fp = fopen(p3, "r+b"); assert(fp);
            long off = (long)offsetof(CnetVsaGenCapsule, topical) + (long)offsetof(CnetVsaTopicalBlock, q8) + 17;
            int8_t w = (int8_t)(capA->topical.q8[17] ^ 0x1);
            assert(fseek(fp, off, SEEK_SET) == 0 && fwrite(&w, sizeof(w), 1, fp) == 1);
            fclose(fp);
            assert(cnet_vsa_gencap_load(capR, p3) == -5);
            assert(capR->certified == 0);
        }
        /* wrong width in the block must fail closed before the digest */
        {
            assert(cnet_vsa_gencap_save(capA, p3) == 0);
            FILE *fp = fopen(p3, "r+b"); assert(fp);
            long off = (long)offsetof(CnetVsaGenCapsule, topical) + (long)offsetof(CnetVsaTopicalBlock, width);
            uint32_t bad = 8192;
            assert(fseek(fp, off, SEEK_SET) == 0 && fwrite(&bad, sizeof(bad), 1, fp) == 1);
            fclose(fp);
            assert(cnet_vsa_gencap_load(capR, p3) == -4);
        }
        remove(p3);
        printf("  v3 round-trip ok; flipped centroid byte -> rc=-5; wrong width -> rc=-4 PASS\n");

        /* legacy v2 file: loads, has no topical block, registers into the float space */
        const char *p2 = "/tmp/cnet_vsa_calib_v2.gencap";
        CnetVsaGenCapsule *capV2 = build_capsule("cap_v2_legacy", C);
        assert(cnet_vsa_gencap_seal_legacy_v2(capV2) == 0);
        assert(capV2->version == CNET_VSA_GENCAP_VERSION_V2);
        {
            FILE *fp = fopen(p2, "wb"); assert(fp);
            assert(fwrite(capV2, CNET_VSA_GENCAP_V2_SIZE, 1, fp) == 1);
            fclose(fp);
        }
        assert(cnet_vsa_gencap_load(capR, p2) == 0);
        assert(capR->version == CNET_VSA_GENCAP_VERSION_V2);
        assert(capR->topical.present == 0);
        assert(capR->calib.reserved0 == cnet_vsa_gencap_encoder_id(capV2));
        printf("  v2 file loads (no topical block, encoder id kept) PASS\n");

        /* a loaded capsule has no wide evidence: sealing it must be refused, so
         * an upgrade can never publish an empty centroid under a real receipt */
        assert(cnet_vsa_gencap_seal(capR) == -1);
        assert(capR->version == CNET_VSA_GENCAP_VERSION_V2);
        assert(cnet_vsa_gencap_save(capR, "/tmp/cnet_vsa_calib_v2_resave.gencap") == -6);
        assert(access("/tmp/cnet_vsa_calib_v2_resave.gencap", F_OK) != 0);
        printf("  sealing a loaded capsule refused (-1); legacy resave refused (-6) PASS\n");

        /* ingest after a calibration would make the receipt describe a centroid
         * it never measured: refused */
        {
            CnetVsaGenCapsule *capI = build_capsule("cap_i_stale", B);
            assert(cnet_vsa_gencap_calibrate(capI, B->sent_p, N_SENT, B->probe_p, N_PROBE, neg_for_b, N_NEG, target_in, target_neg) == 0);
            assert(cnet_vsa_gencap_ingest(capI, A->sent[0]) == -1);
            assert(cnet_vsa_gencap_seal(capI) == 0);
            free(capI);
            printf("  ingest after calibration refused (-1) PASS\n");
        }
        /* calibrating against a different sentence list of the same length:
         * leave-one-out would subtract vectors never accumulated: refused */
        {
            CnetVsaGenCapsule *capJ = build_capsule("cap_j_ident", B);
            Corpus *B2 = (Corpus *)calloc(1, sizeof(Corpus));
            assert(B2);
            build_corpus(B2, POOL_B, 16);   /* same domain, different sentences */
            assert(cnet_vsa_gencap_calibrate(capJ, B2->sent_p, N_SENT, B->probe_p, N_PROBE, neg_for_b, N_NEG, target_in, target_neg) == -1);
            assert(capJ->calib.calibrated == 0);
            free(B2); free(capJ);
            printf("  calibration with a same-length foreign sentence list refused (-1) PASS\n");
        }
        /* registry admission verifies the digest: one flipped centroid byte must
         * keep the file out of the registry, not just out of dispatch */
        {
            const char *pc = "/tmp/cnet_vsa_calib_corrupt.gencap";
            assert(cnet_vsa_gencap_save(capA, pc) == 0);
            FILE *fp = fopen(pc, "r+b"); assert(fp);
            long off = (long)offsetof(CnetVsaGenCapsule, centroid) + 4 * 7;
            float bad = capA->centroid[7] + 0.5f;
            assert(fseek(fp, off, SEEK_SET) == 0 && fwrite(&bad, sizeof(bad), 1, fp) == 1);
            fclose(fp);
            CnetVsaGenRegistry *regc = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
            assert(regc);
            assert(cnet_vsa_registry_init(regc, CNET_VSA_DEFAULT_DIM) == 0);
            assert(cnet_vsa_registry_add_capsule(regc, pc) == -5);
            assert(regc->count == 0);
            remove(pc);
            free(regc);
            printf("  corrupt centroid refused at registry admission (-5) PASS\n");
        }
        /* a v3 file truncated to the v2 length must be refused by the registry
         * loader, not registered as a float-only entry */
        {
            const char *pt = "/tmp/cnet_vsa_calib_v3_trunc.gencap";
            assert(cnet_vsa_gencap_save(capA, pt) == 0);
            FILE *fp = fopen(pt, "r+b"); assert(fp);
            assert(ftruncate(fileno(fp), (off_t)CNET_VSA_GENCAP_V2_SIZE) == 0);
            fclose(fp);
            CnetVsaGenRegistry *regt = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
            assert(regt);
            assert(cnet_vsa_registry_init(regt, CNET_VSA_DEFAULT_DIM) == 0);
            assert(cnet_vsa_registry_add_capsule(regt, pt) == -3);
            assert(regt->count == 0);
            assert(cnet_vsa_gencap_load(capR, pt) == -3);
            remove(pt);
            free(regt);
            printf("  truncated v3 file refused by registry (-3) and loader (-3) PASS\n");
        }

        /* binary routing: three calibrated v3 capsules -> binary space; add a
         * header-only entry -> falls back to float for everyone */
        const char *neg_for_c2[N_NEG];
        for (int i = 0; i < N_SENT; ++i) {
            neg_for_c2[i] = A->sent[i]; neg_for_c2[N_SENT + i] = B->sent[i];
        }
        CnetVsaGenCapsule *capB = build_capsule("cap_b_bread", B);
        CnetVsaGenCapsule *capC = build_capsule("cap_c_law", C);
        assert(cnet_vsa_gencap_calibrate(capB, B->sent_p, N_SENT, B->probe_p, N_PROBE, neg_for_b, N_NEG, target_in, target_neg) == 0);
        assert(cnet_vsa_gencap_calibrate(capC, C->sent_p, N_SENT, C->probe_p, N_PROBE, neg_for_c2, N_NEG, target_in, target_neg) == 0);
        assert(cnet_vsa_gencap_seal(capB) == 0);
        assert(cnet_vsa_gencap_seal(capC) == 0);

        CnetVsaGenRegistry *reg3 = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
        assert(reg3);
        assert(cnet_vsa_registry_init(reg3, CNET_VSA_DEFAULT_DIM) == 0);
        assert(cnet_vsa_registry_add_capsule_mem(reg3, capA, NULL) == 0);
        assert(cnet_vsa_registry_add_capsule_mem(reg3, capB, NULL) == 0);
        assert(cnet_vsa_registry_add_capsule_mem(reg3, capC, NULL) == 0);
        /* nine more v3 capsules from unique pseudo-word pools so the margin gate
         * (needs >= 8 others) is exercised in the wide space, not just the radius */
        CnetVsaGenCapsule *noise[9];
        for (int k = 0; k < 9; ++k) {
            const char *pool[16];
            static char pool_buf[9][16][24];
            for (int w = 0; w < 16; ++w) { snprintf(pool_buf[k][w], 24, "zq%dw%u", k, rnd()); pool[w] = pool_buf[k][w]; }
            Corpus *N = (Corpus *)calloc(1, sizeof(Corpus));
            assert(N);
            build_corpus(N, pool, 16);
            char nm[32]; snprintf(nm, sizeof(nm), "noise_%d", k);
            noise[k] = build_capsule(nm, N);
            assert(cnet_vsa_gencap_seal(noise[k]) == 0);   /* uncalibrated: wide ceiling 0.90 */
            assert(noise[k]->topical.present == 1);
            assert(cnet_vsa_registry_add_capsule_mem(reg3, noise[k], NULL) == 0);
            free(N);
        }
        assert(reg3->count == 12);
        assert(cnet_vsa_registry_binary_space(reg3) == 1);
        CnetVsaRouteResult rr;
        int w;
        int ok_in = 0, ok_alien = 0, alien_margin = 0, alien_radius = 0;
        for (int i = 0; i < N_PROBE; ++i) {
            w = cnet_vsa_registry_route_query(reg3, A->probe[i], &rr);
            assert(rr.space == CNET_VSA_ROUTE_SPACE_WIDE);
            assert(rr.margin_checked == 1);
            if (w == 0) { ok_in++; assert(rr.z >= rr.z_min); }
            w = cnet_vsa_registry_route_query(reg3, B->probe[i], &rr);
            if (w == 1) ok_in++;
            w = cnet_vsa_registry_route_query(reg3, Z->probe[i], &rr);
            assert(rr.margin_checked == 1);
            if (w < 0) {
                ok_alien++;
                if (rr.status == CNET_VSA_ROUTE_REFUSE_MARGIN) alien_margin++;
                else if (rr.status == CNET_VSA_ROUTE_REFUSE_RADIUS) alien_radius++;
            }
        }
        printf("  wide routing over 12 v3 capsules: %d/%d in-domain probes routed home (margin gate checked), "
               "%d/%d alien probes refused (%d by radius, %d by margin)\n",
               ok_in, 2 * N_PROBE, ok_alien, N_PROBE, alien_radius, alien_margin);
        assert(ok_in >= 2 * N_PROBE - 2);
        assert(ok_alien == N_PROBE);
        for (int k = 0; k < 9; ++k) free(noise[k]);
        /* one legacy entry demotes the whole registry to the float space */
        CnetVsaCapsuleHeader hv2;
        memcpy(&hv2, capV2, sizeof(hv2));
        assert(cnet_vsa_registry_add_header(reg3, &hv2, NULL) == 0);
        assert(cnet_vsa_registry_binary_space(reg3) == 0);
        w = cnet_vsa_registry_route_query(reg3, A->probe[3], &rr);
        assert(rr.space == CNET_VSA_ROUTE_SPACE_FLOAT);
        printf("  mixed registry (one v2 header) routes in the float space PASS\n");
        remove(p2);
        free(reg3); free(capR); free(capV2); free(capB); free(capC);
    }

    free(capA); free(capA2); free(capD); free(capE);
    free(A); free(B); free(C); free(Z);

    printf("\n=================================================================\n");
    printf(" CNET_VSA_CALIBRATION_BENCH_PASS: All 6 validation gates passed\n");
    printf("=================================================================\n");
    return 0;
}
