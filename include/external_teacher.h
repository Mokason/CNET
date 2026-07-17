#ifndef CNET_EXTERNAL_TEACHER_H
#define CNET_EXTERNAL_TEACHER_H

/* External / foreign-framework teacher bridge.
 *
 * Whisper, CLIP, HF, ONNX, HTTP APIs, or hermetic fakes all bind the same
 * way: a port-shaped oracle callback + non-zero identity. CNET never treats
 * the foreign stack as planner authority — only as a label source for
 * acquire / train / certify / specialist_admit.
 *
 * Gate: make multimodal_v0 → MULTIMODAL_V0_PASS.
 */

#include <stddef.h>
#include <stdint.h>

#include "acquire.h"
#include "cnet_export.h"
#include "nn.h"
#include "router.h"
#include "specialist.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_EXT_TEACHER_ABI 1u
#define CNET_EXT_NAME_MAX 64
#define CNET_EXT_KIND_MAX 32

typedef enum {
    CNET_MODALITY_TEXT = 0,
    CNET_MODALITY_VOICE = 1,
    CNET_MODALITY_VISION = 2
} CnetModality;

typedef struct {
    uint32_t abi_version;
    uint32_t struct_size;
    CnetModality modality;
    char name[CNET_EXT_NAME_MAX];
    char kind[CNET_EXT_KIND_MAX]; /* "callback", "table", "subprocess", ... */
    Port input_port;
    Port output_port;
    CnetOracleFn fn;
    void *ctx;
    CnetOracleIdentity identity;
    uint64_t behavior_digest;
    int bound;
} ExternalTeacher;

/* Zero *t and stamp abi/size. Returns 0. */
CNET_API int external_teacher_init(ExternalTeacher *t);

/* Bind a callback teacher. identity->artifact_digest must be nonzero.
   kind is recorded as "callback". Returns 0, or <0. */
CNET_API int external_teacher_bind_callback(
    ExternalTeacher *t,
    CnetModality modality,
    const char *name,
    Port input_port,
    Port output_port,
    CnetOracleFn fn,
    void *ctx,
    const CnetOracleIdentity *identity,
    uint64_t behavior_digest);

/* Offline table teacher: for each row, input[i*in_dim..(i+1)*in_dim) maps to
   target[i*out_dim..). Lookup is exact one-hot / argmax on first one-hot field
   when input is ONEHOT; otherwise nearest L2 match. Copies tables (caller may
   free). identity required. kind="table". Returns 0, or <0. */
CNET_API int external_teacher_bind_table(
    ExternalTeacher *t,
    CnetModality modality,
    const char *name,
    Port input_port,
    Port output_port,
    const double *inputs,
    const double *targets,
    size_t n_rows,
    const CnetOracleIdentity *identity,
    uint64_t behavior_digest);

/* Subprocess teacher: each oracle call writes one line to the child stdin:
 *   IN <in_dim> d0 d1 ... d{n-1}
 * and expects one line on stdout:
 *   OUT <out_dim> d0 d1 ... d{m-1}
 * `cmdline` is executed via /bin/sh -c (shell metacharacters allowed).
 * Child is started lazily on first call and kept open until unbind.
 * kind="subprocess". identity required. Returns 0, or <0. */
CNET_API int external_teacher_bind_subprocess(
    ExternalTeacher *t,
    CnetModality modality,
    const char *name,
    Port input_port,
    Port output_port,
    const char *cmdline,
    const CnetOracleIdentity *identity,
    uint64_t behavior_digest);

/* FNV-1a 64 of a file's bytes (0 if unreadable). Used for artifact digests. */
CNET_API uint64_t external_teacher_file_digest(const char *path);

/* Fill an OracleEntry for acquire/gap-lane (borrows t's fn/ctx/identity). */
CNET_API int external_teacher_to_oracle(const ExternalTeacher *t,
                                        OracleEntry *out);

/* Register into an OracleRegistry (name from teacher). Returns 0, or <0. */
CNET_API int external_teacher_register(OracleRegistry *oracles,
                                       ExternalTeacher *t);

/* Wrap as specialist oracle adapter + admit with contract. slot storage is
   caller-owned (HarnessOracleSlot-compatible layout not required — uses
   internal Temporary wrap). Returns 0, or <0. */
CNET_API int external_teacher_admit_oracle(
    ExternalTeacher *t,
    PrimitiveRegistry *reg,
    const Contract *contract,
    BinaryTransformNetwork *adapter_out, /* may be NULL if caller only wants reg */
    Specialist *spec_out);               /* may be NULL */

/* Mine a finite table from the teacher over n_rows fixed inputs, train a
   student BTN, build a contract from canonical targets, admit through
   specialist_admit. On success *student_out is heap-allocated and owned by
   the caller (registry borrows it). Returns 0, 1 if refused, <0 on error. */
CNET_API int external_teacher_mine_admit(
    ExternalTeacher *t,
    PrimitiveRegistry *reg,
    const double *probe_inputs,   /* n_rows * in_dim */
    const double *canonical_targets, /* n_rows * out_dim (for contract) */
    size_t n_rows,
    size_t init_hidden,
    size_t max_hidden,
    size_t max_epochs,
    unsigned int seed,
    const char *unit_name,
    BinaryTransformNetwork **student_out);

CNET_API void external_teacher_unbind(ExternalTeacher *t);

CNET_API const char *cnet_modality_name(CnetModality m);

#ifdef __cplusplus
}
#endif

#endif /* CNET_EXTERNAL_TEACHER_H */
