#ifndef CNET_JSON_TOOLCALL_H
#define CNET_JSON_TOOLCALL_H

/* Closed-set JSON tool-call spine (v0).
 *
 * Host (.NET / MCP) owns free-form JSON parse/emit.
 * CNET owns a certified closed-set classifier:
 *   features(json keywords) → ONEHOT tool id
 *
 * Tools (match CceHost Agent):
 *   0 calculator  1 memory_store  2 memory_recall
 *   3 file_read   4 cnet_recall   5 final
 *
 * Gate: make json_toolcall → JSON_TOOLCALL_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "external_teacher.h"
#include "nn.h"
#include "router.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_JTC_N_TOOL 6
#define CNET_JTC_N_FEAT 16
#define CNET_JTC_UNIT_NAME "json_toolcall_v0"

/* Canonical tool names (index = class id). */
CNET_API const char *const *cnet_jtc_tool_names(void);

/* Keyword features (host and hermetic teacher share this alphabet). */
CNET_API const char *const *cnet_jtc_feature_names(void);

/* Encode a JSON-ish string into CNET_JTC_N_FEAT binary doubles (0/1).
   Case-insensitive substring match on feature keywords. Returns 0. */
CNET_API int cnet_jtc_encode(const char *json_text, double *feat_out);

/* Argmax of ONEHOT tool vector → tool id, or -1. */
CNET_API int cnet_jtc_decode_tool(const double *tool_onehot);

/* Hermetic teacher: features → tool one-hot (stand-in for a fixed schema oracle). */
CNET_API int cnet_jtc_hermetic_teacher(const double *in, double *out, void *ctx);

CNET_API Port cnet_jtc_input_port(void);  /* tag jtc_feat  RAW[16] */
CNET_API Port cnet_jtc_output_port(void); /* tag json_tool ONEHOT[6] */

/* Mine all 6 tool exemplars + admit certified student.
   identity_digest_salt nonzero preferred. Returns 0 on success. */
CNET_API int cnet_jtc_v0_mine_admit(
    PrimitiveRegistry *reg,
    uint64_t identity_digest_salt,
    BinaryTransformNetwork **student_out,
    ExternalTeacher *teacher_out /* optional */);

/* Canonical example JSON strings per tool (for mines + host tests). */
CNET_API const char *cnet_jtc_example_json(int tool_id);

#ifdef __cplusplus
}
#endif

#endif /* CNET_JSON_TOOLCALL_H */
