#ifndef CNET_MODALITY_VISION_H
#define CNET_MODALITY_VISION_H

/* Vision v0: fixed-label visual classes mined from an external teacher.
 *
 * Classes: circle, square, triangle, line  (4)
 * Canvas: 8x8 binary pixels (64 features) rendered hermetically.
 * Output: ONEHOT[4] (hard) or soft evidence via teacher top-1 + margin.
 *
 * Gate: make multimodal_v0.
 */

#include <stddef.h>

#include "cnet_export.h"
#include "external_teacher.h"
#include "nn.h"
#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VISION_N_CLASS 4
#define CNET_VISION_SIDE 8
#define CNET_VISION_PIXELS (CNET_VISION_SIDE * CNET_VISION_SIDE)

CNET_API const char *const *cnet_vision_class_names(void);

/* Render class_id into an 8x8 binary image (row-major doubles 0/1). */
CNET_API void cnet_vision_hermetic_render(int class_id, double *pixels64);

/* Hermetic teacher: classifies a 64-d image → one-hot[4] by matching renders
   (stand-in for CLIP/ViT over a fixed label set). */
CNET_API int cnet_vision_hermetic_teacher(const double *in, double *out,
                                          void *ctx);

CNET_API Port cnet_vision_input_port(void);
CNET_API Port cnet_vision_output_port(void);

/* Optional EVIDENCE-shaped output port (top-k support width = n_class). */
CNET_API Port cnet_vision_evidence_port(void);

/* Mine all 4 classes from hermetic teacher and admit. */
CNET_API int cnet_vision_v0_mine_admit(
    PrimitiveRegistry *reg,
    uint64_t identity_digest_salt,
    BinaryTransformNetwork **student_out,
    ExternalTeacher *teacher_out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_MODALITY_VISION_H */
