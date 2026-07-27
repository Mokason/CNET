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

#include <stdint.h>

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

#endif
