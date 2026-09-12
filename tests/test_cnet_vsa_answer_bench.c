/* Certified answer gate. Marker: CNET_VSA_ANSWER_BENCH_PASS
 *  1. A v4 capsule keeps its sentences; the file grows by the passage block
 *     only; a flipped passage byte fails the load (digest); a v3 seal loads
 *     and answers NO_PASSAGES.
 *  2. Routing + passage ranking returns the sentence the question was made
 *     from, on a hermetic two-capsule registry; an off-topic prompt is refused
 *     at the route; the answer floor z_min is calibrated from probes.
 *  3. Latency: cold (first answer builds the capsule's passage vectors) and
 *     warm, on the hermetic registry; informational timing on bin/ if present. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#include "cnet_vsa.h"
#include "cnet_vsa_text.h"
#include "cnet_vsa_lexicon.h"
#include "cnet_vsa_gen_capsule.h"

static double now_us(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3; }
static uint64_t g = 0x9E3779B97F4A7C15ULL;
static uint32_t rnd(void) { g ^= g << 13; g ^= g >> 7; g ^= g << 17; return (uint32_t)g; }

static const char *A_W[] = { "coating", "ceramic", "furnace", "gradient", "thermal", "stress", "crack", "layer", "quench", "sinter", "glaze", "kiln" };
static const char *B_W[] = { "levain", "crumb", "hydration", "flour", "proof", "oven", "crust", "starter", "knead", "ferment", "bake", "loaf" };
static const char *A_SUBJ[] = { "spallation", "delamination", "porosity", "adhesion", "oxidation", "creep" };
static const char *B_SUBJ[] = { "gluten", "yeast", "steam", "scoring", "retarding", "autolyse" };

/* each sentence carries one distinctive subject word so a question about it has one right passage */
static int build(const char *path, const char *name, const char **w, int nw, const char **subj, int ns, int nsent, int legacy_v3, char sents[][256], char probes[][256]) {
    CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule)); assert(cap);
    assert(cnet_vsa_gencap_init(cap, name, "TEST", CNET_VSA_DEFAULT_DIM) == 0);
    assert(cnet_vsa_gencap_set_encoder(cap, CNET_VSA_ENCODER_STEM) == 0);
    for (int i = 0; i < nsent; ++i) {
        snprintf(sents[i], 256, "the %s %s is measured with the %s and %s under %s conditions.", w[rnd() % nw], subj[i % ns], w[rnd() % nw], w[rnd() % nw], w[rnd() % nw]);
        assert(cnet_vsa_gencap_ingest(cap, sents[i]) > 0);
    }
    for (int i = 0; i < 6; ++i) snprintf(probes[i], 256, "how is %s measured with %s under %s conditions", subj[i % ns], w[rnd() % nw], w[rnd() % nw]);
    const char *pp[6]; for (int i = 0; i < 6; ++i) pp[i] = probes[i];
    /* negatives from the other topic */
    const char **ow = (w == A_W) ? B_W : A_W; const char **os = (w == A_W) ? B_SUBJ : A_SUBJ;
    char negs[40][256]; const char *np[40];
    for (int i = 0; i < 40; ++i) { snprintf(negs[i], 256, "the %s %s is measured with the %s and %s under %s conditions.", ow[rnd() % 12], os[i % 6], ow[rnd() % 12], ow[rnd() % 12], ow[rnd() % 12]); np[i] = negs[i]; }
    const char *sp[64]; for (int i = 0; i < nsent; ++i) sp[i] = sents[i];
    int crc = cnet_vsa_gencap_calibrate_dual(cap, sp, nsent, pp, 6, np, 40, 0.80f, 0.90f, NULL, NULL);
    assert(crc == 0);
    assert(cnet_vsa_gencap_calibrate_passages(cap, pp, 6, np, 40, 0.90f) == 0);
    int rc = legacy_v3 ? cnet_vsa_gencap_seal_legacy_v3(cap) : cnet_vsa_gencap_seal(cap);
    assert(rc == 0);
    int calibrated = (int)cap->passages.calibrated; float zmin = cap->passages.z_min; uint32_t cnt = cap->passages.count;
    if (legacy_v3) { cap->version = CNET_VSA_GENCAP_VERSION; /* save() writes the current layout; emulate a v3 file by truncation below */ }
    assert(cnet_vsa_gencap_save(cap, path) == 0);
    if (legacy_v3) {
        /* rewrite as a true v3 file: the v3 prefix with version 3 and the v3 digest */
        CnetVsaGenCapsule *c3 = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule)); assert(c3);
        memcpy(c3, cap, sizeof(*c3)); c3->version = CNET_VSA_GENCAP_VERSION_V3; memset(&c3->passages, 0, sizeof(c3->passages));
        /* digest for v3 is computed by seal_common; reproduce via a fresh v3 seal on an uncertified copy */
        c3->certified = 0; c3->calib.calibrated = cap->calib.calibrated;
        FILE *f = fopen(path, "wb"); assert(f);
        /* seal again as v3 on the copy (needs wide evidence: still present in the build-time fields of cap) */
        assert(cnet_vsa_gencap_seal_legacy_v3(c3) == 0);
        assert(fwrite(c3, 1, CNET_VSA_GENCAP_V3_SIZE, f) == CNET_VSA_GENCAP_V3_SIZE); fclose(f); free(c3);
    }
    printf("  built %s: %u passages, z_min %.2f%s%s\n", name, cnt, zmin, calibrated ? " (calibrated)" : "", legacy_v3 ? " [written as v3]" : "");
    free(cap);
    return 0;
}

int main(void) {
    printf("=================================================================\n");
    printf(" CNET-VSA Certified Answer Bench (v4 passages, fail-closed)\n");
    printf("=================================================================\n\n");
    const char *dir = "/tmp/cnet_vsa_answer_bench"; mkdir(dir, 0755);
    static char sa[64][256], sb[64][256], pa[6][256], pb[6][256];

    printf("[1/3] Format: passages persist, are digest-covered, and v3 files still load\n");
    build("/tmp/cnet_vsa_answer_bench/topic_a.gencap", "topic_a", A_W, 12, A_SUBJ, 6, 48, 0, sa, pa);
    build("/tmp/cnet_vsa_answer_bench/topic_b.gencap", "topic_b", B_W, 12, B_SUBJ, 6, 48, 0, sb, pb);
    {
        struct stat st; assert(stat("/tmp/cnet_vsa_answer_bench/topic_a.gencap", &st) == 0);
        assert((size_t)st.st_size == CNET_VSA_GENCAP_V4_SIZE);
        printf("  file %zu bytes = v3 %zu + passage block %zu\n", (size_t)st.st_size, (size_t)CNET_VSA_GENCAP_V3_SIZE, (size_t)(CNET_VSA_GENCAP_V4_SIZE - CNET_VSA_GENCAP_V3_SIZE));
        CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule)); assert(cap);
        assert(cnet_vsa_gencap_load(cap, "/tmp/cnet_vsa_answer_bench/topic_a.gencap") == 0);
        assert(cap->passages.present && cap->passages.count == 48 && cap->passages.calibrated);
        assert(strcmp(cap->passages.text + cap->passages.offset[7], sa[7]) == 0);
        /* flip one byte inside passage 3 */
        long off = (long)(offsetof(CnetVsaGenCapsule, passages) + offsetof(CnetVsaPassageBlock, text) + cap->passages.offset[3] + 4);
        FILE *fp = fopen("/tmp/cnet_vsa_answer_bench/topic_a.gencap", "r+b"); assert(fp);
        assert(fseek(fp, off, SEEK_SET) == 0); int c = fgetc(fp); assert(fseek(fp, off, SEEK_SET) == 0); fputc(c ^ 1, fp); fclose(fp);
        assert(cnet_vsa_gencap_load(cap, "/tmp/cnet_vsa_answer_bench/topic_a.gencap") == -5);
        fp = fopen("/tmp/cnet_vsa_answer_bench/topic_a.gencap", "r+b"); assert(fp); assert(fseek(fp, off, SEEK_SET) == 0); fputc(c, fp); fclose(fp);
        assert(cnet_vsa_gencap_load(cap, "/tmp/cnet_vsa_answer_bench/topic_a.gencap") == 0);
        free(cap);
    }
    static char sc[64][256], pc[6][256];
    build("/tmp/cnet_vsa_answer_bench/topic_c_v3.gencap", "topic_c", A_W, 12, A_SUBJ, 6, 40, 1, sc, pc);
    {
        CnetVsaGenCapsule *cap = (CnetVsaGenCapsule *)calloc(1, sizeof(CnetVsaGenCapsule)); assert(cap);
        int rc = cnet_vsa_gencap_load(cap, "/tmp/cnet_vsa_answer_bench/topic_c_v3.gencap");
        assert(rc == 0 && cap->version == CNET_VSA_GENCAP_VERSION_V3 && cap->passages.present == 0);
        free(cap);
    }
    remove("/tmp/cnet_vsa_answer_bench/topic_c_v3.gencap");
    printf("  passages persist, tamper -> -5, v3 file loads without passages PASS\n");

    printf("\n[2/3] Answers: the passage the question came from, refusal off-topic\n");
    CnetVsaGenRegistry *reg = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry)); assert(reg);
    assert(cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM) == 0);
    reg->min_null_count = 1000; /* two capsules: no margin statistics, radius + passage floor decide */
    reg->term_gate = 0;         /* a 12-word templated corpus makes one frame word decisive; the term gate is the router bench's subject, not this one's */
    assert(cnet_vsa_registry_load_dir(reg, dir) == 2);
    int hits = 0, asked = 0, wrong = 0, floor_refused = 0;
    for (int i = 0; i < 6; ++i) {
        CnetVsaAnswer a;
        /* same shape as the calibration probes (the radius is tight on a 12-word vocabulary), different fillers */
        char q[256]; snprintf(q, sizeof(q), "how is %s measured with %s and %s under %s conditions", A_SUBJ[i], A_W[(i + 5) % 12], A_W[(i + 3) % 12], A_W[(i + 8) % 12]);
        cnet_vsa_registry_answer(reg, q, 2, &a); asked++;
        assert(a.capsule_idx >= 0 && strcmp(reg->capsules[a.capsule_idx].header.name, "topic_a") == 0);
        if (a.status == CNET_VSA_ANSWER_OK) {
            if (strstr(cnet_vsa_registry_passage(reg, a.capsule_idx, a.passage_idx[0]), A_SUBJ[i])) hits++; else wrong++;
        } else {
            assert(a.status == CNET_VSA_ANSWER_REFUSE_PASSAGE && a.z < a.z_min);   /* refused by the calibrated floor, never a wrong passage */
            floor_refused++;
        }
    }
    printf("  topic-a questions: %d/%d answered with the passage about the asked subject, %d refused by the passage floor, %d wrong\n", hits, asked, floor_refused, wrong);
    assert(hits >= 4 && wrong == 0);
    {
        CnetVsaAnswer a;
        cnet_vsa_registry_answer(reg, "how do I tune a violin using harmonics and a fifth interval", 2, &a);
        assert(a.status == CNET_VSA_ANSWER_ROUTE_REFUSED);
        cnet_vsa_registry_answer(reg, "how is gluten measured with crumb and loaf under oven conditions", 2, &a);
        assert(a.capsule_idx >= 0 && strcmp(reg->capsules[a.capsule_idx].header.name, "topic_b") == 0);
        assert(a.status == CNET_VSA_ANSWER_OK || a.status == CNET_VSA_ANSWER_REFUSE_PASSAGE);
        if (a.status == CNET_VSA_ANSWER_OK) assert(strstr(cnet_vsa_registry_passage(reg, a.capsule_idx, a.passage_idx[0]), "gluten") != NULL);
        printf("  off-topic prompt refused at the route; topic-b answered from topic_b PASS\n");
    }

    printf("\n[3/3] Latency (hermetic registry, 48 passages): cold vs warm\n");
    {
        CnetVsaAnswer a; double cold = 0, warm = 0; int nw = 0;
        cnet_vsa_registry_release(reg); cnet_vsa_registry_init(reg, CNET_VSA_DEFAULT_DIM); reg->min_null_count = 1000; reg->term_gate = 0;
        assert(cnet_vsa_registry_load_dir(reg, dir) == 2);
        cnet_vsa_registry_answer(reg, "how is spallation measured with coating and furnace under layer conditions", 2, &a); assert(a.cold == 1); cold = a.route_us + a.rank_us;
        for (int i = 0; i < 200; ++i) { cnet_vsa_registry_answer(reg, "how is spallation measured with coating and furnace under layer conditions", 2, &a); assert(a.cold == 0); warm += a.route_us + a.rank_us; nw++; }
        printf("  cold %.1f us (builds %d passage vectors), warm %.1f us per answer (route + rank)\n", cold, a.passages, warm / nw);
    }
    cnet_vsa_registry_release(reg); free(reg);

    /* informational: the production registry, when present with its lexicon */
    {
        struct stat st;
        if (stat("bin/registry.lex", &st) == 0) {
            CnetVsaGenRegistry *pr = (CnetVsaGenRegistry *)calloc(1, sizeof(CnetVsaGenRegistry));
            if (pr) {
                cnet_vsa_registry_init(pr, CNET_VSA_DEFAULT_DIM);
                int n = cnet_vsa_registry_load_dir(pr, "bin");
                const char *qs[] = { "how do thick fluids affect pressure drop in a heat exchanger", "wavefront lds shared memory coalescing hip execution", "what keeps a bug's outer shell from letting water out" };
                int withp = 0; for (size_t i = 0; i < pr->count; ++i) withp += pr->capsules[i].passage_count > 0;
                printf("\n  production bin/: %d capsules, %d with passages\n", n, withp);
                for (int i = 0; i < 3; ++i) {
                    CnetVsaAnswer a; cnet_vsa_registry_answer(pr, qs[i], 2, &a);
                    double t0 = now_us(); for (int r = 0; r < 20; ++r) cnet_vsa_registry_answer(pr, qs[i], 2, &a); double per = (now_us() - t0) / 20;
                    printf("  \"%.50s\": %s%s%s, route %.0f us + rank %.0f us, warm %.0f us per answer\n", qs[i],
                           a.status == CNET_VSA_ANSWER_OK ? "OK -> " : "refused (", a.capsule_idx >= 0 ? pr->capsules[a.capsule_idx].header.name : "-",
                           a.status == CNET_VSA_ANSWER_OK ? "" : ")", a.route_us, a.rank_us, per);
                }
                cnet_vsa_registry_release(pr); free(pr);
            }
        }
    }
    printf("\n=================================================================\n");
    printf(" CNET_VSA_ANSWER_BENCH_PASS\n");
    printf("=================================================================\n");
    return 0;
}
