/* Schema-2 frontend asset validation — see vd_frontend.h.
 *
 * This lives in its own translation unit, in plain C with no OpenCV, for one
 * reason: it is the parser, and a parser that can only be exercised through a
 * runner that needs OpenCV, VOC images and an imported capsule is a parser that
 * never gets fuzzed. tests/test_vd_frontend_parse.c mutates every field of a
 * known-good asset against this file alone, under ASan and UBSan.
 *
 * The discipline throughout: nothing is used before it is range-checked, every
 * product is checked for overflow before it is computed, and every fixed char
 * array is proven NUL-terminated before anything can print it.
 */
#include "vd_frontend.h"

#include <math.h>
#include <string.h>

/* A fixed char array used as a C string must contain a NUL inside its own
   bounds; `extractor` was printed with %s straight out of the asset. */
static int nul_terminated(const char *field, size_t cap) {
    size_t i;
    for (i = 0; i < cap; i++)
        if (field[i] == '\0') return 1;
    return 0;
}

static int in_range(uint32_t v, uint32_t lo, uint32_t hi) {
    return v >= lo && v <= hi;
}

const char *vd_frontend_validate(const void *asset, size_t len,
                                 VdFrontendHdr *out, size_t *float_count_out) {
    VdFrontendHdr h;
    size_t rows, floats, total;

    if (!asset) return "asset_null";
    if (len < sizeof h) return "asset_shorter_than_header";

    /* memcpy rather than a cast: the capsule hands back a malloc'd buffer, but
       nothing in the format guarantees alignment for a struct read. */
    memcpy(&h, asset, sizeof h);

    /* magic is exactly 8 bytes and is NOT a C string, so compare all 8. */
    if (memcmp(h.magic, VD_FRONTEND_MAGIC, 8) != 0) return "bad_magic";
    if (h.schema != VD_FRONTEND_SCHEMA) return "bad_schema";

    /* Strings before anything can print them. */
    if (!nul_terminated(h.class0, sizeof h.class0)) return "class0_unterminated";
    if (!nul_terminated(h.class1, sizeof h.class1)) return "class1_unterminated";
    if (!nul_terminated(h.extractor, sizeof h.extractor))
        return "extractor_unterminated";
    if (!nul_terminated(h.protocol, sizeof h.protocol))
        return "protocol_unterminated";
    if (!nul_terminated(h.artifact_root, sizeof h.artifact_root))
        return "artifact_root_unterminated";
    if (h.extractor[0] == '\0') return "extractor_empty";
    if (h.protocol[0] == '\0') return "protocol_empty";

    /* Dimensions. pca_dim drives a loop that writes into a fixed double[512];
       that buffer size is why VD_FRONTEND_MAX_PCA_DIM exists. */
    if (!in_range(h.hog_dim, 1u, VD_FRONTEND_MAX_HOG_DIM)) return "hog_dim_range";
    if (!in_range(h.pca_dim, 1u, VD_FRONTEND_MAX_PCA_DIM)) return "pca_dim_range";
    if (h.pca_dim > h.hog_dim) return "pca_dim_exceeds_hog_dim";
    if (!in_range(h.hog_side, 8u, VD_FRONTEND_MAX_HOG_SIDE)) return "hog_side_range";
    if (!in_range(h.ss_width, 16u, VD_FRONTEND_MAX_SS_WIDTH)) return "ss_width_range";
    if (!in_range(h.max_prop, 1u, VD_FRONTEND_MAX_PROPOSALS)) return "max_prop_range";
    if (!in_range(h.min_side, 1u, VD_FRONTEND_MAX_MIN_SIDE)) return "min_side_range";
    if (h.color > 1u) return "color_range";
    if (h.nms_iou_x100 > 100u) return "nms_iou_range";
    if (h.score_thr_x100 > 100u) return "score_thr_range";
    if (h.match_iou_x100 > 100u) return "match_iou_range";
    if (h.n_classes != 2u) return "n_classes_range";
    if (!in_range(h.gate_k, 1u, VD_FRONTEND_MAX_GATE_K)) return "gate_k_range";
    if (!isfinite(h.gate_tau) || h.gate_tau < 0.0) return "gate_tau_range";

    /* Checked arithmetic: floats = hog_dim + pca_dim*hog_dim, then
       total = sizeof(header) + floats*sizeof(float). The original computed the
       first product unchecked and compared the result to len, so a header that
       wrapped size_t could describe a tiny buffer as an enormous matrix. */
    if (h.pca_dim > (size_t)-1 / h.hog_dim) return "pca_matrix_overflow";
    rows = (size_t)h.pca_dim * (size_t)h.hog_dim;
    if (rows > (size_t)-1 - (size_t)h.hog_dim) return "float_count_overflow";
    floats = rows + (size_t)h.hog_dim;
    if (floats > ((size_t)-1 - sizeof h) / sizeof(float))
        return "asset_size_overflow";
    total = sizeof h + floats * sizeof(float);
    if (len != total) return "asset_size_mismatch";

    /* The BODY, not just the header. The PCA mean and eigenbasis are fed
       straight into cv::PCA::project and then into the certified head; a single
       NaN or Inf there makes every projected feature NaN, every score NaN, and
       the coverage gate's distance comparisons meaningless — a silently
       degraded guard rather than a refusal. Validated here, before OpenCV ever
       sees the bytes. memcpy per value: nothing in the format guarantees the
       body is aligned for float access. */
    {
        const unsigned char *body = (const unsigned char *)asset + sizeof h;
        size_t i;
        for (i = 0; i < floats; i++) {
            float v;
            memcpy(&v, body + i * sizeof(float), sizeof v);
            if (!isfinite((double)v))
                return i < (size_t)h.hog_dim ? "pca_mean_not_finite"
                                             : "pca_matrix_not_finite";
        }
    }

    if (out) *out = h;
    if (float_count_out) *float_count_out = floats;
    return NULL;
}

const char *vd_frontend_check_contract(const VdFrontendHdr *h,
                                       size_t btn_inputs, size_t btn_outputs) {
    if (!h) return "header_null";
    if ((size_t)h->pca_dim != btn_inputs) return "pca_dim_disagrees_with_head";
    /* The runner softmaxes o[0] and o[1]; a head with fewer outputs would be
       read past its end, and one with more is not the declared class map. */
    if (btn_outputs != (size_t)h->n_classes)
        return "head_outputs_disagree_with_classes";
    return NULL;
}

const char *vd_frontend_check_protocol(const VdFrontendHdr *h,
                                       const char *expected) {
    if (!h || !expected || !expected[0]) return "protocol_expectation_missing";
    if (!nul_terminated(h->protocol, sizeof h->protocol))
        return "protocol_unterminated";
    if (strcmp(h->protocol, expected) != 0) return "protocol_mismatch";
    return NULL;
}
