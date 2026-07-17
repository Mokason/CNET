#ifndef CNET_MODALITY_VOICE_H
#define CNET_MODALITY_VOICE_H

/* Voice v0: closed-set speech commands mined from an external teacher.
 *
 * Default alphabet (speech_commands subset):
 *   yes no up down left right on off stop go  (10 classes)
 * Features: fixed-dim frontend vector (hermetic fake or real Whisper embed
 * mapped into the same shape by the teacher callback).
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

#define CNET_VOICE_N_CMD 10
#define CNET_VOICE_FEAT_DIM 16

/* Canonical command strings (index = class id). */
CNET_API const char *const *cnet_voice_command_names(void);

/* Build deterministic hermetic "frontend" features for command class c
   (simulates a frozen encoder). Writes CNET_VOICE_FEAT_DIM doubles. */
CNET_API void cnet_voice_hermetic_features(int class_id, double *out_feat);

/* Hermetic teacher: argmax-free — maps features → one-hot command by matching
   the hermetic frontend (foreign-framework stand-in). */
CNET_API int cnet_voice_hermetic_teacher(const double *in, double *out,
                                         void *ctx);

/* Ports for voice v0 (speech_feat binary-ish vector → speech_cmd one-hot). */
CNET_API Port cnet_voice_input_port(void);
CNET_API Port cnet_voice_output_port(void);

/* End-to-end v0: bind hermetic teacher, mine all 10 commands, admit student.
   identity_digest_salt seeds artifact identity (nonzero). Returns 0 on success. */
CNET_API int cnet_voice_v0_mine_admit(
    PrimitiveRegistry *reg,
    uint64_t identity_digest_salt,
    BinaryTransformNetwork **student_out,
    ExternalTeacher *teacher_out /* optional; unbound by caller if non-NULL */);

/* Bind a real/foreign voice teacher as a subprocess oracle.
 * cmdline e.g. "python3 tools/voice_teacher.py --mode hermetic"
 * or whisper: "python3 tools/voice_teacher.py --mode whisper --model tiny"
 * Artifact digest defaults to FNV of the script path when digest_salt==0 and
 * script path is parsed from cmdline; else digest_salt (must be nonzero).
 * Returns 0, or <0. */
CNET_API int cnet_voice_bind_subprocess(
    ExternalTeacher *t,
    const char *name,
    const char *cmdline,
    uint64_t identity_digest_salt);

/* Env-driven bind: CNET_VOICE_TEACHER_CMD required.
 * Optional CNET_VOICE_TEACHER_DIGEST (hex/decimal uint64).
 * Returns 0 on success, -2 if env unset, <0 on other errors. */
CNET_API int cnet_voice_bind_from_env(ExternalTeacher *t);

/* Mine+admit using a bound teacher (callback or subprocess). Probes are
 * hermetic frontend features; teacher labels each row. unit_name default
 * "voice_cmd_v0". Returns 0 on success. */
CNET_API int cnet_voice_mine_admit_teacher(
    ExternalTeacher *t,
    PrimitiveRegistry *reg,
    const char *unit_name,
    BinaryTransformNetwork **student_out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_MODALITY_VOICE_H */
