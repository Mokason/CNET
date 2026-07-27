/* Portable isolated-knowledge capsule.
 *
 * CNET's near-term target is ASI in the sense of **Artificial Specialized
 * Intelligence** — explicitly NOT AGI and NOT "artificial superintelligence".
 * Broad semantic grounding exists only to understand user intent; competence
 * comes from accumulating isolated, certified, portable Micro Tensor Kernels
 * and routing between them. Accumulation is not reasoning, and nothing here
 * should be read as a general-capability claim.
 *
 * A capsule is the transfer unit for one such kernel. It is NOT an MTK
 * `.tskill`/CMSK cartridge (include/cce/cce_mtk.h): those are weight deltas
 * patched onto a specific host model's named tensor sites, carrying no typed
 * contract, no certification and no coverage. A capsule is the certified side
 * of CNET, and the two are not interchangeable.
 *
 * It binds, and refuses to import without:
 *   - stable identity + version   unit name + contract behaviour digest
 *   - typed contract              input/goal Ports and exemplar count
 *   - payload                     a single-unit CNB (per-blob CNU1 seal)
 *   - runtime compatibility       CNB container format version
 *   - certification evidence      behaviour digest checked after materialise
 *   - coverage / abstention       the certified input rows travel WITH it, plus
 *                                 OPTIONAL targets. HybridCoverage treats labels
 *                                 as optional by design (a record restored from
 *                                 the sidecar carries inputs only and still
 *                                 gates), so cov_out==0 is permitted and means
 *                                 "rows, no labels". A NONZERO cov_out must equal
 *                                 the unit's checked output-port dimension.
 *   - provenance                  whatever the source base recorded, bound and
 *                                 verified against the payload. It is NOT
 *                                 mandatory: an empty provenance exports and
 *                                 imports fine, so this proves "unchanged in
 *                                 transit", never "came from a trusted party".
 *   - integrity                   FNV-1a over the payload AND over every
 *                                 security-relevant manifest field
 *
 * PACKAGE ATOMICITY — exactly what is and is not guaranteed:
 *   NOT guaranteed: the two files publish separately, so a capsule directory
 *     CAN be observed half-written. There is no atomic directory publication.
 *   Guaranteed: a half-written package is REJECTED. Import requires both files,
 *     a manifest checksum over every security-relevant field, and a payload
 *     checksum, so an incomplete package fails closed.
 *   Guaranteed per file: each is written to an exclusive O_CREAT|O_EXCL|
 *     O_NOFOLLOW temp and renamed, so no reader sees a partially written file
 *     and a planted symlink at either temp path cannot be followed.
 *
 * TRUST BOUNDARY: a capsule is a LOCAL transfer object. Its checksums are
 * unkeyed, so they detect ACCIDENT — truncation, bit-rot, a partial write, a
 * mismatched build — and nothing else. Anyone who can rewrite a capsule can
 * recompute them. This is not authenticity; signing is out of scope.
 *
 * The coverage binding is the load-bearing part. cnb_export_subset already
 * moved units between bases, but coverage lives in a <base>.coverage sidecar,
 * so a transferred mined unit arrived UNGATED on the target and would answer
 * outside its certified domain — the exact confident-wrong failure coverage
 * exists to prevent. A capsule moves the unit and its gate as one object.
 */
#ifndef CNET_CAPSULE_H
#define CNET_CAPSULE_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "base.h"
#include "hybrid_ai.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_CAPSULE_SCHEMA 1
/* Schema 2 == schema 1 plus ONE manifest-bound sidecar blob, bound exactly the
   way unit.cnb already is (declared bytes + FNV, inside the region the trailing
   manifest_fnv already covers). It exists because a visual specialist's head is
   useless without the frontend that produced its features: the PCA basis, the
   HOG and Selective Search configuration and identity, the class map and the
   thresholds. Carrying those is what makes the capsule the whole specialist
   rather than a set of weights.
   Compatibility is fail-closed by schema number: a runtime that does not
   understand assets rejects 2 outright rather than importing a head without its
   frontend. This is an extension of the one capsule format, not a second one. */
#define CNET_CAPSULE_SCHEMA_ASSET 2
#define CNET_CAPSULE_ASSET_FILE "frontend.cvfa"
#define CNET_CAPSULE_MAX_ASSET (16UL * 1024UL * 1024UL)
#define CNET_CAPSULE_REASON_MAX 160

typedef struct {
    char unit[96];
    unsigned long long behavior_digest;
    size_t exemplars;
    size_t coverage_rows;
    size_t payload_bytes;
    unsigned cnb_version;
    char provenance[128]; /* "" when the source base recorded none */
    unsigned schema;          /* 1 = no asset, 2 = asset-bearing */
    unsigned asset_schema;    /* 0 when no asset */
    size_t asset_bytes;       /* 0 when no asset */
    unsigned long long asset_fnv;
    char reject_reason[CNET_CAPSULE_REASON_MAX]; /* "" on success */
} CnetCapsuleReport;

/* Write dir/{unit.cnb,manifest.cknow}. cov may be NULL, in which case the
 * capsule carries coverage_rows=0 — correct only for a unit certified over its
 * whole domain. A capsule that DOES carry coverage refuses to import with a
 * NULL registry rather than quietly shipping an ungated unit. */
CNET_API int cnet_capsule_export(const CnetBase *src, const HybridAi *cov,
                                 const char *unit, const char *dir,
                                 CnetCapsuleReport *rep);

/* Verify and import. Fails CLOSED: on any integrity, compatibility or
 * contract mismatch nothing is written to dst/cov and rep->reject_reason says
 * which check refused it. Returns 0 on success, negative on rejection. */
CNET_API int cnet_capsule_import(CnetBase *dst, HybridAi *cov, const char *dir,
                                 CnetCapsuleReport *rep);

/* Schema-2 export: as above, plus one sidecar blob written to
   dir/CNET_CAPSULE_ASSET_FILE and bound by the manifest. asset may be NULL, in
   which case this behaves exactly like cnet_capsule_export and emits schema 1. */
CNET_API int cnet_capsule_export_asset(const CnetBase *src, const HybridAi *cov,
                                       const char *unit, const char *dir,
                                       const void *asset, size_t asset_len,
                                       unsigned asset_schema,
                                       CnetCapsuleReport *rep);

/* Schema-2 import. On success *asset_out is a malloc'd copy of the verified
   blob (caller frees) and *asset_len_out its length; both are set to NULL/0 for
   a schema-1 capsule. Fails closed identically to cnet_capsule_import, and a
   caller that passes NULL for asset_out is refused an asset-bearing capsule
   rather than silently given a head with no frontend. */
CNET_API int cnet_capsule_import_asset(CnetBase *dst, HybridAi *cov, const char *dir,
                                       void **asset_out, size_t *asset_len_out,
                                       unsigned *asset_schema_out,
                                       CnetCapsuleReport *rep);

#ifdef __cplusplus
}
#endif
#endif
