#ifndef CNET_WEIGHT_CONVERT_H
#define CNET_WEIGHT_CONVERT_H

/* Weight conversion door: open weights nominate a finite typed table;
   the table certifies; the teacher leaves. Native C only.

   Slice 2: the leased labeler reads a named tensor through the real
   cce_gguf stack (cce_gguf_load / cce_gguf_tensor_bytes / cce_gguf_load_f32).
   Residual is a mouth (next-token) and is not the labeler.

   First domain: u8_inc16 — increment on 0..15 (16 combos, no RAW).
   Fixture GGUF is not Bonsai conversion. No auto-CERT. Residual never
   speaks. Naive tensor remap is refused. WordLM is not grown.

   Path: attest (SHA-256) → lease bind → cce_gguf_load (no FP16 workspace)
   → plan_table_build (PlanTeacherFn labeler) → native BTN student
   → full-domain spec-reproduction ≥ 0.95 (train=verify) → unbind.
   Student may be CERT only through the existing certify doors. */

#include <stddef.h>
#include <stdint.h>

#include "acquire.h"
#include "nn.h"
#include "plan_table.h"

#ifdef __cplusplus
extern "C" {
#endif

struct cce_gguf; /* opaque; real reader lives in cce/cce_gguf.h */

#define CNET_WEIGHT_DOMAIN_U8_INC16 "u8_inc16"
#define CNET_WEIGHT_U8_INC16_COMBOS 16
#define CNET_WEIGHT_SPEC_BAR 0.95
#define CNET_WEIGHT_LUT_TENSOR "u8_inc16.lut"
#define CNET_WEIGHT_ORACLE_NAME "u8_inc16_oracle"
#define CNET_WEIGHT_TOOLCHAIN_ATOM "cnet_weight_convert_v1"

typedef struct {
    char path[512];
    struct cce_gguf *gguf; /* owned; from cce_gguf_load, not parse_lut */
    float lut_store[16];   /* typed decode of the named LUT tensor */
    const float *lut;
    size_t lut_n;
    size_t file_len;
    int fixture; /* 1 = synthetic fixture, not a real host GGUF */
    int reader_cce_gguf; /* 1 after cce_gguf_load */
    int lut_via_load_f32;
    int lut_via_tensor_bytes;
    size_t gguf_reads; /* silent tensor reads; not residual speak */
    unsigned char sha256[32];
    char sha256_hex[65];
    int attested;
} CnetWeightFile;

typedef struct {
    CnetWeightFile file;
    OracleRegistry *oracles; /* borrowed */
    char oracle_name[ACQUIRE_NAME_MAX];
    uint64_t lease;
    int bound;
    size_t teacher_calls; /* mouth-shaped; silent LUT path leaves this 0 */
    size_t residual_speak; /* residual_gguf_oracle / label_batch; stays 0 */
    int abort_on; /* -1 none; else abort when decoded input equals this */
} CnetWeightOracle;

typedef struct {
    size_t kept;
    size_t aborts;
    size_t verified;
    size_t missed;
    double spec_rate;
    int teacher_unbound;
    int fixture;
    int reader_cce_gguf;
    int certified; /* always 0: this door does not auto-CERT */
    char domain[32];
    size_t combos;
    size_t residual_speak;
    size_t gguf_reads;
} CnetWeightConvertReport;

Port cnet_weight_u8_inc16_in_port(void);
Port cnet_weight_u8_inc16_out_port(void);

/* Write a tiny synthetic GGUF that implements u8_inc16 as one F32 LUT.
   The file is a fixture; the reader is cce_gguf, not a toy parser.
   This is not Bonsai / host-model conversion. */
int cnet_weight_write_u8_inc16_fixture(const char *path);

/* SHA-256 the local file into identity (artifact_sha256 + toolchain). */
int cnet_weight_attest(const char *path, CnetOracleIdentity *id);

/* Open via cce_gguf_load; read u8_inc16.lut with tensor_bytes or load_f32.
   No private parse_lut. No FP16 workspace. */
int cnet_weight_mmap(CnetWeightFile *f, const char *path);
void cnet_weight_unmap(CnetWeightFile *f);

/* Register + lease the opened file as a teachable oracle. Unattested
   files are refused when policy.require_attested_to_teach is set. */
int cnet_weight_bind(CnetWeightOracle *wo, OracleRegistry *o,
                     CnetWeightFile *f, const char *name);
int cnet_weight_unbind(CnetWeightOracle *wo);

/* Strict PlanTeacherFn: typed u8 in → typed u8 out via a named GGUF
   tensor. Abort ≠ guess. Does not call residual_gguf_oracle. */
int cnet_weight_labeler(void *ctx, const double *in, size_t in_total,
                        double *out, size_t out_total);

/* plan_table_build → native BTN student → ≥0.95 full domain → unbind.
   Does not call specialist_admit / certify. Residual does not speak. */
int cnet_weight_convert(CnetWeightOracle *wo,
                        BinaryTransformNetwork *student,
                        CnetWeightConvertReport *rep);

/* Serve the student only. Refuses if the teacher is still leased.
   Never invokes the labeler and never touches the GGUF. */
int cnet_weight_serve(const CnetWeightOracle *wo,
                      BinaryTransformNetwork *student,
                      const double *in, double *out);

/* Naive W_q/W_k/W_v/MLP remap is not a conversion. Always refuses. */
int cnet_weight_tensor_remap_admit(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CNET_WEIGHT_CONVERT_H */
