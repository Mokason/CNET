/* Frontend asset: everything a fresh runtime needs to turn a JPEG into the
 * feature vectors this specialist's head was certified over.
 *
 * The head alone is not a specialist. Without the exact proposal rule, the
 * exact descriptor and the exact projection that produced its training
 * features, imported weights compute a confident function of the wrong input.
 * This blob is what makes the capsule the whole specialist, and it travels as
 * the schema-2 sidecar bound by the capsule manifest.
 */
#ifndef VD_FRONTEND_H
#define VD_FRONTEND_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VD_FRONTEND_MAGIC  "VDFRONT1"
#define VD_FRONTEND_SCHEMA 1

/* Fixed-size header; the PCA mean and eigenbasis follow as float32, mean first
   (hog_dim) then pca_dim rows of hog_dim. */
typedef struct {
    char     magic[8];
    uint32_t schema;
    uint32_t hog_side, color, hog_dim, pca_dim;
    uint32_t ss_width, max_prop, min_side;
    uint32_t nms_iou_x100, score_thr_x100, match_iou_x100;
    uint32_t gate_k;
    double   gate_tau;
    uint32_t n_classes;
    char     class0[32], class1[32];
    char     extractor[96];    /* proposal + descriptor + projection identity */
    char     protocol[64];
    char     artifact_root[72];
    uint32_t reserved;
} VdFrontendHdr;

/* ---- bounds -------------------------------------------------------------
 * The runner projects into a fixed `double x[512]`, casts asset-controlled
 * dimensions to OpenCV `int`, and prints fixed char arrays as C strings. None
 * of that was checked: the only validation was magic, schema, and one size
 * expression computed with unchecked multiplication. A checksum-valid but
 * maliciously authored local capsule could therefore overflow the stack, read
 * out of bounds, or ask OpenCV for an astronomical allocation.
 *
 * VD_FRONTEND_MAX_PCA_DIM is the size of that fixed buffer and is the reason
 * the cap exists; the rest are sanity ceilings far above any real V2 asset
 * (hog_dim 1764, pca_dim 256, hog_side 64, ss_width 300, max_prop 300).
 */
#define VD_FRONTEND_MAX_PCA_DIM   512u
#define VD_FRONTEND_MAX_HOG_DIM   1048576u
#define VD_FRONTEND_MAX_HOG_SIDE  4096u
#define VD_FRONTEND_MAX_SS_WIDTH  16384u
#define VD_FRONTEND_MAX_PROPOSALS 1000000u
#define VD_FRONTEND_MAX_MIN_SIDE  4096u
#define VD_FRONTEND_MAX_GATE_K    65536u

/* Validate a schema-2 asset completely BEFORE any field is used.
 *
 * On success returns NULL and, when `out` is non-NULL, fills it with the
 * validated header. On failure returns a short static reason string and leaves
 * *out untouched. `float_count_out` receives hog_dim + pca_dim*hog_dim, already
 * proven not to overflow and to match `len` exactly.
 *
 * Total: it reads at most `len` bytes and never trusts a field it has not yet
 * range-checked. */
const char *vd_frontend_validate(const void *asset, size_t len,
                                 VdFrontendHdr *out, size_t *float_count_out);

/* The asset must describe the head that arrived with it. `btn_inputs` is the
 * imported unit's input width and `btn_outputs` its output width; a frontend
 * that projects into a different dimension than the head consumes is a
 * confident function of the wrong input. Returns NULL when they agree. */
const char *vd_frontend_check_contract(const VdFrontendHdr *h,
                                       size_t btn_inputs, size_t btn_outputs);

/* The asset must name the protocol the runtime was built for. Returns NULL
 * when `h->protocol` equals `expected`. */
const char *vd_frontend_check_protocol(const VdFrontendHdr *h,
                                       const char *expected);

#ifdef __cplusplus
}
#endif

#endif
