#ifndef CNET_CORE_HOST_H
#define CNET_CORE_HOST_H
#include "cnet_core_candidate.h"
#include "cnet_core_selector.h"
#include "cnet_capsule_core.h"
typedef struct CnetCoreHost CnetCoreHost;
typedef struct CnetCoreLease CnetCoreLease;
typedef struct {char request[128];unsigned expected;int verified;} CnetCoreShadowCase;
typedef struct {
    size_t graphs,reachable,completed,unreachable,abstained,shadow_cases,growth_obligations;
    double brier;
} CnetCoreGateReport;
/* Local, opt-in owner API. Start with the existing deterministic runtime.
 * All host operations serialize the legacy BTN/certification stack globally.
 * Do not concurrently call raw legacy core/contract APIs outside this wrapper.
 * Max 4 loaded generations, 64 pinned leases, one staged candidate. No files
 * or live services are mutated. Directory arguments are trusted owner paths.
 * Destroy refuses while requests remain pinned; the owner must stop all new
 * operations before close. Unpin a lease only after its calls finish. */
CnetCoreHost *cnet_core_host_open(const char *registry);
int cnet_core_host_close(CnetCoreHost *host);
CnetCoreLease *cnet_core_host_pin(CnetCoreHost *host);
void cnet_core_host_unpin(CnetCoreLease *lease);
uint64_t cnet_core_host_generation(const CnetCoreLease *lease);
int cnet_core_host_ask(CnetCoreLease *lease,const char *request,CnetCapsuleCoreReply *reply);
int cnet_core_host_stage(CnetCoreHost *host,int dirfd,const char *name,const char *registry,uint64_t *id);
/* Canonical evidence digest binds every graph field and expected shadow label.
 * Labels/fixtures must come from the trusted owner/evaluator, never the trainer.
 * 2..2048 graphs and 2..256 distinct shadow cases, both positive and negative.
 * check independently computes graph reachability, checks raw-score abstention,
 * .95/.95 per-size floors, Brier<=.01, every proposed path, all shadow labels,
 * deterministic agreement and complete existing capsule growth replay.
 * Evidence hashes provide integrity, NOT authentication/approval. */
int cnet_core_gate_digest(const CnetSelectorGraph *graphs,size_t n,
    const CnetCoreShadowCase *shadow,size_t count,char sha256[65]);
int cnet_core_host_check(CnetCoreHost *host,uint64_t id,const CnetSelectorGraph *graphs,size_t n,
    const CnetCoreShadowCase *shadow,size_t count,CnetCoreGateReport *report);
/* Explicit owner action, only after successful check against the CURRENT active
 * generation. Activation keeps one rollback generation; pinned older requests
 * retain both their model and inventory. Rollback invalidates staged approval.
 * These calls are not exposed to worker processes. */
int cnet_core_host_activate(CnetCoreHost *host,uint64_t id);
int cnet_core_host_rollback(CnetCoreHost *host);
int cnet_core_host_discard(CnetCoreHost *host,uint64_t id);
#endif
