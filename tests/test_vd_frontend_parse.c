/* test_vd_frontend_parse — the schema-2 frontend asset parser must not trust
 * anything it has not checked.
 *
 * WHY THIS EXISTS. `vd_runner.cpp` validated the magic, the schema, and one
 * size expression, then used every other asset-controlled field directly:
 *
 *   - `pca_dim` drove a loop writing into a fixed `double x[512]` with no
 *     bound, so an asset declaring 4096 wrote 3584 doubles past the frame;
 *   - `hog_dim * pca_dim` was multiplied with no overflow check and cast to
 *     OpenCV `int`, so a wrapped product could describe a tiny buffer as an
 *     enormous matrix, or ask OpenCV for an astronomical allocation;
 *   - `extractor` and the other fixed char arrays were printed with `%s`
 *     without any proof of NUL termination;
 *   - nothing required the frontend's projection width to match the head that
 *     arrived with it, so a checksum-valid capsule could compute a confident
 *     function of the wrong input.
 *
 * Capsule FNV detects accident, not authorship, so none of this was caught by
 * the checksum. The validator is now a standalone C translation unit precisely
 * so it can be mutated exhaustively here, with no OpenCV and no VOC data.
 *
 * Coverage: one structured mutation per field, plus a deterministic byte-fuzz
 * over the whole header. Nothing is written to disk at all.
 */
#include "../tools/vision_detection/vd_frontend.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the pre-fix parser, kept executable --------------------------------
   Transcribed from tools/vision_detection/vd_runner.cpp at 3edac49: magic,
   schema, and one size expression computed with unchecked multiplication.
   Nothing else was validated before use.

   CNET_VD_FRONTEND_LEGACY=1 runs every mutation below through THIS instead of
   the hardened validator, which is the RED reproduction and stays reproducible
   forever rather than living in a scratch directory. */
static const char *validate_legacy(const void *asset, size_t len,
                                   VdFrontendHdr *out, size_t *float_count_out) {
    VdFrontendHdr h;
    size_t nfloat;
    if (!asset || len < sizeof h) return "asset_short";
    memcpy(&h, asset, sizeof h);
    if (memcmp(h.magic, VD_FRONTEND_MAGIC, 8) != 0 ||
        h.schema != VD_FRONTEND_SCHEMA)
        return "frontend_schema";
    nfloat = (size_t)h.hog_dim + (size_t)h.pca_dim * (size_t)h.hog_dim;
    if (len != sizeof h + nfloat * sizeof(float)) return "asset_size";
    if (out) *out = h;
    if (float_count_out) *float_count_out = nfloat;
    return NULL;
}

typedef const char *(*ValidateFn)(const void *, size_t, VdFrontendHdr *,
                                  size_t *);
static ValidateFn validate = vd_frontend_validate;
static int legacy_mode;

static int failures;
static int checks;

static void check(int condition, const char *message) {
    checks++;
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

/* The real V2 shape, so the control is not a toy. */
#define GOOD_HOG_DIM 1764u
#define GOOD_PCA_DIM 256u

static size_t good_len(void) {
    size_t floats = (size_t)GOOD_HOG_DIM +
                    (size_t)GOOD_PCA_DIM * (size_t)GOOD_HOG_DIM;
    return sizeof(VdFrontendHdr) + floats * sizeof(float);
}

static unsigned char *make_good(size_t *len_out) {
    VdFrontendHdr h;
    size_t len = good_len();
    unsigned char *buf = (unsigned char *)calloc(1, len);
    if (!buf) return NULL;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, VD_FRONTEND_MAGIC, 8);
    h.schema = VD_FRONTEND_SCHEMA;
    h.hog_side = 64; h.color = 1;
    h.hog_dim = GOOD_HOG_DIM; h.pca_dim = GOOD_PCA_DIM;
    h.ss_width = 300; h.max_prop = 300; h.min_side = 16;
    h.nms_iou_x100 = 30; h.score_thr_x100 = 50; h.match_iou_x100 = 50;
    h.gate_k = 8; h.gate_tau = 0.125;
    h.n_classes = 2;
    snprintf(h.class0, sizeof h.class0, "background");
    snprintf(h.class1, sizeof h.class1, "car");
    snprintf(h.extractor, sizeof h.extractor,
             "SelectiveSearchFast/ximgproc;HOG64x64c9b16s8;PCA256");
    snprintf(h.protocol, sizeof h.protocol, "cnet_vision_v2_20260727");
    snprintf(h.artifact_root, sizeof h.artifact_root, "vision_v2");
    memcpy(buf, &h, sizeof h);
    *len_out = len;
    return buf;
}

/* Apply `mutate` to a copy of the good asset and require a refusal. */
static void reject(void (*mutate)(VdFrontendHdr *h, size_t *len),
                   const char *want_reason, const char *description) {
    size_t len = 0;
    unsigned char *buf = make_good(&len);
    VdFrontendHdr h, out;
    const char *why;
    char message[256];

    if (!buf) {
        check(0, "fixture allocation");
        return;
    }
    memcpy(&h, buf, sizeof h);
    mutate(&h, &len);
    memcpy(buf, &h, sizeof h);

    why = validate(buf, len, &out, NULL);
    snprintf(message, sizeof message, "%s is refused", description);
    check(why != NULL, message);
    if (why && want_reason && !legacy_mode) {
        snprintf(message, sizeof message, "%s is refused as %s (saw %s)",
                 description, want_reason, why);
        check(strcmp(why, want_reason) == 0, message);
    }
    free(buf);
}

/* ---- structured mutations ------------------------------------------------ */

static void m_magic(VdFrontendHdr *h, size_t *len) { (void)len; h->magic[3] ^= 0x40; }
static void m_schema(VdFrontendHdr *h, size_t *len) { (void)len; h->schema = 99; }
static void m_class0(VdFrontendHdr *h, size_t *len) {
    (void)len; memset(h->class0, 'A', sizeof h->class0);
}
static void m_class1(VdFrontendHdr *h, size_t *len) {
    (void)len; memset(h->class1, 'B', sizeof h->class1);
}
static void m_extractor(VdFrontendHdr *h, size_t *len) {
    (void)len; memset(h->extractor, 'X', sizeof h->extractor);
}
static void m_protocol_unterminated(VdFrontendHdr *h, size_t *len) {
    (void)len; memset(h->protocol, 'P', sizeof h->protocol);
}
static void m_artifact_root(VdFrontendHdr *h, size_t *len) {
    (void)len; memset(h->artifact_root, 'R', sizeof h->artifact_root);
}
static void m_extractor_empty(VdFrontendHdr *h, size_t *len) {
    (void)len; h->extractor[0] = '\0';
}
static void m_protocol_empty(VdFrontendHdr *h, size_t *len) {
    (void)len; h->protocol[0] = '\0';
}
static void m_hog_dim_zero(VdFrontendHdr *h, size_t *len) {
    (void)len; h->hog_dim = 0;
}
static void m_pca_dim_zero(VdFrontendHdr *h, size_t *len) {
    (void)len; h->pca_dim = 0;
}
/* THE stack overflow: 4096 doubles into a fixed double[512]. */
static void m_pca_dim_overflows_buffer(VdFrontendHdr *h, size_t *len) {
    h->pca_dim = 4096;
    *len = sizeof *h + ((size_t)h->hog_dim +
                        (size_t)h->pca_dim * (size_t)h->hog_dim) * sizeof(float);
}
static void m_pca_dim_exceeds_hog(VdFrontendHdr *h, size_t *len) {
    h->hog_dim = 64;
    h->pca_dim = 128;
    *len = sizeof *h + ((size_t)h->hog_dim +
                        (size_t)h->pca_dim * (size_t)h->hog_dim) * sizeof(float);
}
static void m_hog_side(VdFrontendHdr *h, size_t *len) {
    (void)len; h->hog_side = 4;
}
static void m_hog_side_huge(VdFrontendHdr *h, size_t *len) {
    (void)len; h->hog_side = 0xFFFFFFFFu;
}
static void m_ss_width(VdFrontendHdr *h, size_t *len) {
    (void)len; h->ss_width = 0xFFFFFFFFu;
}
static void m_max_prop_zero(VdFrontendHdr *h, size_t *len) {
    (void)len; h->max_prop = 0;
}
static void m_max_prop_huge(VdFrontendHdr *h, size_t *len) {
    (void)len; h->max_prop = 0xFFFFFFFFu;
}
static void m_min_side_zero(VdFrontendHdr *h, size_t *len) {
    (void)len; h->min_side = 0;
}
static void m_color(VdFrontendHdr *h, size_t *len) { (void)len; h->color = 7; }
static void m_nms(VdFrontendHdr *h, size_t *len) { (void)len; h->nms_iou_x100 = 250; }
static void m_score_thr(VdFrontendHdr *h, size_t *len) {
    (void)len; h->score_thr_x100 = 4000;
}
static void m_match_iou(VdFrontendHdr *h, size_t *len) {
    (void)len; h->match_iou_x100 = 101;
}
static void m_classes(VdFrontendHdr *h, size_t *len) { (void)len; h->n_classes = 9; }
static void m_gate_k_zero(VdFrontendHdr *h, size_t *len) { (void)len; h->gate_k = 0; }
static void m_gate_k_huge(VdFrontendHdr *h, size_t *len) {
    (void)len; h->gate_k = 0xFFFFFFFFu;
}
static void m_gate_tau_nan(VdFrontendHdr *h, size_t *len) {
    (void)len;
    /* Build a NaN without <math.h> constants so the mutation is exactly bytes. */
    uint64_t bits = 0x7FF8000000000000ULL;
    memcpy(&h->gate_tau, &bits, sizeof bits);
}
static void m_gate_tau_negative(VdFrontendHdr *h, size_t *len) {
    (void)len; h->gate_tau = -1.0;
}
/* The unchecked product: hog_dim * pca_dim chosen to wrap size_t. */
static void m_matrix_overflow(VdFrontendHdr *h, size_t *len) {
    (void)len;
    h->hog_dim = 0xFFFFFFFFu;
    h->pca_dim = VD_FRONTEND_MAX_PCA_DIM;
}
static void m_size_mismatch(VdFrontendHdr *h, size_t *len) {
    (void)h; *len -= sizeof(float);
}
static void m_size_too_long(VdFrontendHdr *h, size_t *len) {
    (void)h; *len += sizeof(float);
}

int main(void) {
    const char *legacy = getenv("CNET_VD_FRONTEND_LEGACY");
    size_t len = 0, floats = 0;
    unsigned char *good = make_good(&len);
    VdFrontendHdr parsed;
    const char *why;

    if (legacy && legacy[0] == '1' && legacy[1] == '\0') {
        legacy_mode = 1;
        validate = validate_legacy;
        printf("VD_FRONTEND_LEGACY_MODE parser=pre_fix_vd_runner_3edac49\n");
    }

    if (!good) {
        fprintf(stderr, "FAIL: cannot build the control asset\n");
        return 1;
    }

    /* --- control: a real-shaped asset validates ------------------------- */
    why = validate(good, len, &parsed, &floats);
    check(why == NULL, "the control asset validates");
    if (why) fprintf(stderr, "  control refused as %s\n", why);
    check(parsed.pca_dim == GOOD_PCA_DIM, "pca_dim survives validation");
    check(floats == (size_t)GOOD_HOG_DIM +
                        (size_t)GOOD_PCA_DIM * (size_t)GOOD_HOG_DIM,
          "the float count is reported");
    if (!legacy_mode) {
        /* The pre-fix runner had no head/protocol agreement check at all --
           there is nothing to reproduce in legacy mode, which is the point. */
        check(vd_frontend_check_contract(&parsed, GOOD_PCA_DIM, 2) == NULL,
              "a head of the declared width is accepted");
        check(vd_frontend_check_protocol(&parsed,
                                         "cnet_vision_v2_20260727") == NULL,
              "the declared protocol is accepted");
        check(vd_frontend_check_contract(&parsed, GOOD_PCA_DIM + 1, 2) != NULL,
              "a head of the wrong width is refused");
        check(vd_frontend_check_contract(&parsed, GOOD_PCA_DIM, 3) != NULL,
              "a head with the wrong output count is refused");
        check(vd_frontend_check_protocol(&parsed, "cnet_vision_v3") != NULL,
              "an asset from another protocol is refused");
        check(vd_frontend_check_protocol(&parsed, "") != NULL,
              "an empty protocol expectation is refused");
    }

    /* --- short and null buffers ------------------------------------------ */
    check(validate(NULL, len, &parsed, NULL) != NULL,
          "a null asset is refused");
    check(validate(good, 0, &parsed, NULL) != NULL,
          "a zero-length asset is refused");
    check(validate(good, sizeof(VdFrontendHdr) - 1, &parsed,
                   NULL) != NULL,
          "an asset shorter than its header is refused");
    {
        size_t cut;
        for (cut = 1; cut < sizeof(VdFrontendHdr); cut += 17)
            check(validate(good, cut, &parsed, NULL) != NULL,
                  "every truncation inside the header is refused");
    }

    /* --- structured mutations -------------------------------------------- */
    reject(m_magic, "bad_magic", "a flipped magic bit");
    reject(m_schema, "bad_schema", "an unknown schema");
    reject(m_class0, "class0_unterminated", "an unterminated class0");
    reject(m_class1, "class1_unterminated", "an unterminated class1");
    reject(m_extractor, "extractor_unterminated", "an unterminated extractor");
    reject(m_protocol_unterminated, "protocol_unterminated",
           "an unterminated protocol");
    reject(m_artifact_root, "artifact_root_unterminated",
           "an unterminated artifact_root");
    reject(m_extractor_empty, "extractor_empty", "an empty extractor");
    reject(m_protocol_empty, "protocol_empty", "an empty protocol");
    reject(m_hog_dim_zero, "hog_dim_range", "a zero hog_dim");
    reject(m_pca_dim_zero, "pca_dim_range", "a zero pca_dim");
    reject(m_pca_dim_overflows_buffer, "pca_dim_range",
           "a pca_dim past the fixed projection buffer");
    reject(m_pca_dim_exceeds_hog, "pca_dim_exceeds_hog_dim",
           "a pca_dim larger than hog_dim");
    reject(m_hog_side, "hog_side_range", "a degenerate hog_side");
    reject(m_hog_side_huge, "hog_side_range", "an enormous hog_side");
    reject(m_ss_width, "ss_width_range", "an enormous ss_width");
    reject(m_max_prop_zero, "max_prop_range", "a zero proposal budget");
    reject(m_max_prop_huge, "max_prop_range", "an enormous proposal budget");
    reject(m_min_side_zero, "min_side_range", "a zero min_side");
    reject(m_color, "color_range", "an out-of-range colour flag");
    reject(m_nms, "nms_iou_range", "an out-of-range NMS IoU");
    reject(m_score_thr, "score_thr_range", "an out-of-range score threshold");
    reject(m_match_iou, "match_iou_range", "an out-of-range match IoU");
    reject(m_classes, "n_classes_range", "an unexpected class count");
    reject(m_gate_k_zero, "gate_k_range", "a zero gate k");
    reject(m_gate_k_huge, "gate_k_range", "an enormous gate k");
    reject(m_gate_tau_nan, "gate_tau_range", "a NaN gate tau");
    reject(m_gate_tau_negative, "gate_tau_range", "a negative gate tau");
    reject(m_matrix_overflow, NULL, "a PCA matrix that overflows size_t");
    reject(m_size_mismatch, "asset_size_mismatch", "a body one float short");
    reject(m_size_too_long, "asset_size_mismatch", "a body one float long");

    /* --- deterministic byte fuzz over the header ------------------------- */
    {
        uint64_t state = 20260730u;
        unsigned iteration;
        unsigned rounds = legacy_mode ? 200u : 20000u;
        size_t accepted = 0;
        for (iteration = 0; iteration < rounds; iteration++) {
            size_t flen = 0;
            unsigned char *buf = make_good(&flen);
            unsigned bytes, b;
            if (!buf) break;
            /* Splitmix64: reproducible, and every failing seed is replayable. */
            for (b = 0, bytes = 1 + (unsigned)(iteration % 5u); b < bytes; b++) {
                size_t at;
                state += 0x9E3779B97F4A7C15ULL;
                {
                    uint64_t z = state;
                    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
                    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
                    z ^= z >> 31;
                    at = (size_t)(z % sizeof(VdFrontendHdr));
                    buf[at] = (unsigned char)(z >> 32);
                }
            }
            /* The only requirement is that it never reads out of bounds or
               trusts a field it has not checked; ASan and UBSan are the real
               assertions here. An accepted mutant must still be self-consistent. */
            if (validate(buf, flen, &parsed, &floats) == NULL) {
                accepted++;
                check(parsed.pca_dim <= VD_FRONTEND_MAX_PCA_DIM,
                      "a fuzz-accepted asset never exceeds the projection buffer");
                check(sizeof(VdFrontendHdr) + floats * sizeof(float) == flen,
                      "a fuzz-accepted asset's declared size matches its length");
            }
            free(buf);
        }
        printf("VD_FRONTEND_FUZZ iterations=%u accepted=%zu\n", rounds,
               accepted);
    }

    free(good);
    if (failures) {
        printf("VD_FRONTEND_PARSE_FAIL checks=%d failures=%d\n", checks,
               failures);
        return 1;
    }
    if (legacy_mode) {
        /* Reaching here would mean the pre-fix parser refused every mutation,
           i.e. there was never a defect. It did not. */
        printf("VD_FRONTEND_LEGACY_UNEXPECTEDLY_CLEAN checks=%d\n", checks);
        return 1;
    }
    printf("VD_FRONTEND_PARSE_PASS checks=%d mutations=31 fuzz=20000\n", checks);
    return 0;
}
