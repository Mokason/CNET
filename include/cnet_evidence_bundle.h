#ifndef CNET_EVIDENCE_BUNDLE_H
#define CNET_EVIDENCE_BUNDLE_H

/* Evidence bundle for every learned unit.
 *
 * One sealed claim (weights + contract in CNU1) is not enough for audit /
 * rollback. Each learned unit also carries an evidence bundle:
 *
 *   contract_digest       — contract_content_digest (spec identity)
 *   dataset_hash          — FNV over exemplar input||output tables
 *   artifact_digest       — FNV over sealed CNU1 blob (CnbBlob.digest)
 *   artifact_sha256       — collision-resistant blob hash
 *   behavior_digest       — contract_btn_digest (weight/behavior identity)
 *   toolchain_digest      — teacher/toolchain stamp (0 = unattested)
 *   runtime_libs_digest   — linked DSOs + glibc (0 = unattested)
 *   reliability samples   — successes/failures + Laplace reliability
 *   counterfactual_stability — [0,1] report score, or <0 if unknown
 *   rollback_target       — prior unit name + prior artifact digest
 *
 * Persistence: JSONL sidecar next to the base (default "<base>.evidence.jsonl"),
 * replace-by-unit-name. Report-only for serving; never grants admission.
 *
 * Gate: make evidence_bundle → EVIDENCE_BUNDLE_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "base.h"
#include "nn.h"
#include "contract/contract.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_EVIDENCE_BUNDLE_VERSION 1
#define CNET_EVIDENCE_NAME_MAX CNB_NAME_MAX

typedef struct {
    int version;
    char unit_name[CNET_EVIDENCE_NAME_MAX];
    char provenance[CNET_EVIDENCE_NAME_MAX]; /* teacher descriptor, or "" */

    uint64_t behavior_digest;
    uint64_t contract_digest;
    uint64_t dataset_hash;
    uint64_t artifact_digest;
    unsigned char artifact_sha256[32];

    uint64_t toolchain_digest;
    uint64_t runtime_libs_digest;

    unsigned long reliability_successes;
    unsigned long reliability_failures;
    double reliability;                 /* Laplace in [0,1] */

    float counterfactual_stability;     /* [0,1], or <0 if unknown */

    char rollback_unit[CNET_EVIDENCE_NAME_MAX];
    uint64_t rollback_artifact_digest;

    uint64_t recipe_fp;                 /* optional acquisition recipe */
    int complete;                       /* 1 if required digests non-zero */
} CnetEvidenceBundle;

/* Optional fields when recording (any may be zero/NULL/unknown). */
typedef struct {
    uint64_t toolchain_digest;
    float counterfactual_stability;     /* <0 → store as unknown */
    const char *rollback_unit;          /* NULL/"" → none */
    uint64_t rollback_artifact_digest;
    uint64_t recipe_fp;
} CnetEvidenceOpts;

/* ---- build / verify (pure over in-memory base) ---- */

/* Fill *out from a unit already in the base (loads contract + blob).
 * opts may be NULL. Returns 0, or -1 if unit missing / materialize fails. */
CNET_API int cnet_evidence_bundle_from_base(const CnetBase *b,
                                            const char *unit_name,
                                            const CnetEvidenceOpts *opts,
                                            CnetEvidenceBundle *out);

/* Recompute digests from a live btn+contract (no base blob required).
 * artifact_* left zero unless blob_bytes provided. */
CNET_API int cnet_evidence_bundle_from_parts(
    const char *unit_name,
    const BinaryTransformNetwork *btn,
    const Contract *c,
    const unsigned char *blob_bytes, size_t blob_len,
    const CnetEvidenceOpts *opts,
    CnetEvidenceBundle *out);

/* 1 if required digests present (behavior, contract, dataset, artifact). */
CNET_API int cnet_evidence_bundle_is_complete(const CnetEvidenceBundle *b);

/* Verify bundle against live base: re-materialize and compare digests.
 * Returns 0 match, -1 missing/mismatch, -2 bad args. */
CNET_API int cnet_evidence_bundle_verify(const CnetBase *base,
                                         const CnetEvidenceBundle *b);

/* ---- JSON / store ---- */

CNET_API int cnet_evidence_bundle_format_json(const CnetEvidenceBundle *b,
                                             char *buf, size_t cap);

/* Default store path: "<base_path>.evidence.jsonl". out must hold >= base+20. */
CNET_API int cnet_evidence_store_path_for_base(const char *base_path,
                                              char *out, size_t cap);

/* Env CNET_EVIDENCE_STORE, or NULL. */
CNET_API const char *cnet_evidence_store_path_from_env(void);

/* Load all bundles from path into a heap array (*out_n). Caller frees *out. */
CNET_API int cnet_evidence_store_load(const char *path,
                                      CnetEvidenceBundle **out,
                                      size_t *out_n);

/* Upsert one bundle by unit_name; rewrite full file. Creates path. */
CNET_API int cnet_evidence_store_put(const char *path,
                                     const CnetEvidenceBundle *bundle);

/* Find by unit_name in store. Returns 0 and fills *out, or -1. */
CNET_API int cnet_evidence_store_get(const char *path, const char *unit_name,
                                     CnetEvidenceBundle *out);

/* Record for a unit in base into store_path (or env/default if path NULL).
 * Returns 0, or -1. */
CNET_API int cnet_evidence_record(const CnetBase *base, const char *unit_name,
                                  const char *store_path,
                                  const CnetEvidenceOpts *opts);

#ifdef __cplusplus
}
#endif

#endif /* CNET_EVIDENCE_BUNDLE_H */
