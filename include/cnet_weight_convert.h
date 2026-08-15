#ifndef CNET_WEIGHT_CONVERT_H
#define CNET_WEIGHT_CONVERT_H

/* Weight conversion door: open weights nominate a finite typed table;
   the table certifies; the teacher leaves. Native C only.

   First domain: u8_inc16 — increment on 0..15 (16 combos, no RAW).
   Fixture GGUF is not Bonsai conversion. No auto-CERT. Residual never
   speaks. Naive tensor remap is refused. WordLM is not grown.

   Path: attest (SHA-256) → lease bind → mmap GGUF (no FP16 workspace)
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

#define CNET_WEIGHT_DOMAIN_U8_INC16 "u8_inc16"
#define CNET_WEIGHT_U8_INC16_COMBOS 16
#define CNET_WEIGHT_SPEC_BAR 0.95
#define CNET_WEIGHT_LUT_TENSOR "u8_inc16.lut"
#define CNET_WEIGHT_ORACLE_NAME "u8_inc16_oracle"
#define CNET_WEIGHT_TOOLCHAIN_ATOM "cnet_weight_convert_v1"

typedef struct {
    char path[512];
    void *map;
    size_t map_len;
    int fd;
    const float *lut; /* 16 F32 increment labels inside the mmap */
    size_t lut_n;
    int fixture; /* 1 = synthetic fixture, not a real host GGUF */
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
    size_t teacher_calls;
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
    int certified; /* always 0: this door does not auto-CERT */
    char domain[32];
    size_t combos;
} CnetWeightConvertReport;

Port cnet_weight_u8_inc16_in_port(void);
Port cnet_weight_u8_inc16_out_port(void);

/* Write a tiny synthetic GGUF that implements u8_inc16 as one F32 LUT.
   This is a fixture, not Bonsai / host-model conversion. */
int cnet_weight_write_u8_inc16_fixture(const char *path);

/* SHA-256 the local file into identity (artifact_sha256 + toolchain). */
int cnet_weight_attest(const char *path, CnetOracleIdentity *id);

/* mmap GGUF; find u8_inc16.lut as a zero-copy F32 view. No FP16 workspace. */
int cnet_weight_mmap(CnetWeightFile *f, const char *path);
void cnet_weight_unmap(CnetWeightFile *f);

/* Register + lease the mmap'd file as a teachable oracle. Unattested
   files are refused when policy.require_attested_to_teach is set. */
int cnet_weight_bind(CnetWeightOracle *wo, OracleRegistry *o,
                     CnetWeightFile *f, const char *name);
int cnet_weight_unbind(CnetWeightOracle *wo);

/* Strict PlanTeacherFn: LUT forward on the typed domain. Abort ≠ guess. */
int cnet_weight_labeler(void *ctx, const double *in, size_t in_total,
                        double *out, size_t out_total);

/* plan_table_build → native BTN student → ≥0.95 full domain → unbind.
   Does not call specialist_admit / certify. */
int cnet_weight_convert(CnetWeightOracle *wo,
                        BinaryTransformNetwork *student,
                        CnetWeightConvertReport *rep);

/* Serve the student only. Refuses if the teacher is still leased.
   Never invokes the labeler. */
int cnet_weight_serve(const CnetWeightOracle *wo,
                      BinaryTransformNetwork *student,
                      const double *in, double *out);

/* Naive W_q/W_k/W_v/MLP remap is not a conversion. Always refuses. */
int cnet_weight_tensor_remap_admit(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* CNET_WEIGHT_CONVERT_H */
