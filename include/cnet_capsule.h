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
 *   - coverage / abstention       the certified input domain travels WITH it
 *   - provenance + integrity      teacher provenance + FNV-1a over the payload
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
#define CNET_CAPSULE_REASON_MAX 160

typedef struct {
    char unit[96];
    unsigned long long behavior_digest;
    size_t exemplars;
    size_t coverage_rows;
    size_t payload_bytes;
    unsigned cnb_version;
    char reject_reason[CNET_CAPSULE_REASON_MAX]; /* "" on success */
} CnetCapsuleReport;

/* Write dir/{unit.cnb,manifest.cknow}. cov may be NULL (a unit with no
 * coverage record exports with coverage_rows=0 and imports ungated, which is
 * correct only for units certified over their whole domain). Returns 0 on
 * success, negative on error. */
CNET_API int cnet_capsule_export(const CnetBase *src, const HybridAi *cov,
                                 const char *unit, const char *dir,
                                 CnetCapsuleReport *rep);

/* Verify and import. Fails CLOSED: on any integrity, compatibility or
 * contract mismatch nothing is written to dst/cov and rep->reject_reason says
 * which check refused it. Returns 0 on success, negative on rejection. */
CNET_API int cnet_capsule_import(CnetBase *dst, HybridAi *cov, const char *dir,
                                 CnetCapsuleReport *rep);

#ifdef __cplusplus
}
#endif
#endif
