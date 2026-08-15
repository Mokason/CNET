/* Unit-tested swap law for certified capsules / chunks.
 *
 * Live doors (this slice): registry_add_certified (same-name incumbent)
 * and library_evolve / library_admit_candidate call cnet_swap_admit.
 * src/cnet_swap.c is in LIBRARY, so cnet.so contains the law.
 * Until origin/master merges this branch, do not claim master obeys it.
 *
 * A new brick may REPLACE an old one only if it dominates on the old
 * coverage AND every existing CERT composition that used the old brick
 * still passes hop guards after substitution. n_comps==0 is not a
 * composition proof: dominate then still ADD-ALONGSIDE
 * (no_compositions_to_prove), never REPLACE. Otherwise ADD-ALONGSIDE
 * (Progressive Nets / MoCL freeze): keep the old brick. Do not delete it.
 *
 * An explicit replace that would break a CERT composition is REFUSED.
 *
 * Soft learned routers are not a swap signal.
 * Compression is not a swap signal.
 * Teacher / residual (adapter BTNs) never admit.
 *
 * contract_better_if remains a same-contract margin/reliability compare.
 * Same-name registry_add_certified no longer better-or-reject: it asks
 * this law. library_evolve still skips an exact-same digest / already-
 * known contract when n_comps==0; a dominate+hold candidate is admitted
 * through this law instead of silent exact-dedup.
 */
#ifndef CNET_SWAP_H
#define CNET_SWAP_H

#include <stddef.h>

#include "cnet_export.h"
#include "nn.h"
#include "router.h"
#include "contract/contract.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CNET_SWAP_REFUSE = 0,
    CNET_SWAP_REPLACE = 1,
    CNET_SWAP_ADD_ALONGSIDE = 2
} CnetSwapVerdict;

/* Certified coverage table. Exact double rows, same membership rule as
   HybridCoverage (memcmp). targets may be NULL (membership only). */
typedef struct {
    const double *inputs;   /* n_rows * in_dim */
    const double *targets;  /* n_rows * out_dim, or NULL */
    size_t n_rows;
    size_t in_dim;
    size_t out_dim;
} CnetSwapCoverage;

/* One CERT composition that used the old brick. */
typedef struct {
    RoutePlan plan;
    const double *inputs;   /* n_rows * in_dim */
    size_t n_rows;
    size_t in_dim;
} CnetSwapComposition;

typedef struct {
    CnetSwapVerdict verdict;
    const char *reason;
    int dominated;
    int compositions_hold;
} CnetSwapReport;

/* 1 if every old row's input (and target, when both sides have targets)
   appears in new. 0 if not. -1 on bad args.
   Empty old coverage is not a subset (nothing to dominate). */
int cnet_swap_coverage_subset(const CnetSwapCoverage *old_cov,
                              const CnetSwapCoverage *new_cov);

/* 1 if new_btn replays every old coverage row (btn_certify on those rows). */
int cnet_swap_matches_old_rows(const BinaryTransformNetwork *new_btn,
                               const CnetSwapCoverage *old_cov);

/* Dominate = old coverage is a subset of new, and/or new matches all old
   rows. When both a new table and a new btn+targets are given, both must
   hold. Empty old coverage cannot dominate. Returns 1 / 0 / -1. */
int cnet_swap_dominates(const BinaryTransformNetwork *new_btn,
                        const CnetSwapCoverage *old_cov,
                        const CnetSwapCoverage *new_cov);

/* Substitute new_btn for old_btn (pointer or old_name) in each composition
   and run route_execute_guarded. 1 if every row returns 0. 0 if any hop
   guard refuses or the plan fails. n_comps==0 is not a composition proof
   (returns 0). */
int cnet_swap_compositions_hold(const BinaryTransformNetwork *old_btn,
                                const char *old_name,
                                const BinaryTransformNetwork *new_btn,
                                const CnetSwapComposition *comps,
                                size_t n_comps,
                                const DagNodeGuard *guard);

/* Coverage hop-guard helper: refuse unless `input` is an exact row of ctx.
   ctx is CnetSwapCoverage*. unit/btn are unused (one table per guard).
   Returns 0 to allow, 1 to refuse. */
int cnet_swap_hop_allow(const char *unit, const BinaryTransformNetwork *btn,
                        const double *input, size_t in_len, void *ctx);

/* Fill report. Live doors call admit, which calls this.
   Adapter / teacher-residual new_btn => REFUSE.
   n_comps==0 is not a composition proof: ADD_ALONGSIDE
   (no_compositions_to_prove), never REPLACE.
   dominate && compositions_hold => REPLACE, else ADD_ALONGSIDE.
   Returns 0, or -1 on bad args (report REFUSE). */
int cnet_swap_decide(const BinaryTransformNetwork *old_btn,
                     const char *old_name,
                     const BinaryTransformNetwork *new_btn,
                     const CnetSwapCoverage *old_cov,
                     const CnetSwapCoverage *new_cov,
                     const CnetSwapComposition *comps, size_t n_comps,
                     const DagNodeGuard *guard,
                     CnetSwapReport *report);

/* Apply decide: REPLACE the old_name entry, or ADD-ALONGSIDE under
   alongside_name (old brick stays). New must certify new_c.
   Adapter new_btn is refused (teacher/residual never admits).
   Returns 0 if replace or add-alongside applied, -1 on refuse/error. */
int cnet_swap_admit(PrimitiveRegistry *reg,
                    const char *old_name,
                    BinaryTransformNetwork *new_btn,
                    const char *alongside_name,
                    const Contract *new_c,
                    const CnetSwapCoverage *old_cov,
                    const CnetSwapCoverage *new_cov,
                    const CnetSwapComposition *comps, size_t n_comps,
                    const DagNodeGuard *guard,
                    CnetSwapReport *report);

/* Explicit replace only. REFUSE (nothing added, old stays) if the law
   does not allow replace — including a swap that would break a CERT
   composition. Returns 0 if replaced, -1 if refused/error. */
int cnet_swap_replace(PrimitiveRegistry *reg,
                      const char *old_name,
                      BinaryTransformNetwork *new_btn,
                      const Contract *new_c,
                      const CnetSwapCoverage *old_cov,
                      const CnetSwapCoverage *new_cov,
                      const CnetSwapComposition *comps, size_t n_comps,
                      const DagNodeGuard *guard,
                      CnetSwapReport *report);


/* ---- live-door helpers (do not fork the law) -----------------------------
   Bind borrowed CERT compositions for the next registry_add_certified /
   library_admit_candidate call. NULL/0 = n_comps==0 (no proof).
   Single-threaded, like the rest of the BTN stack. */
void cnet_swap_bind_compositions(const CnetSwapComposition *comps, size_t n_comps,
                                 const DagNodeGuard *guard);
void cnet_swap_unbind_compositions(void);
void cnet_swap_bound_compositions(const CnetSwapComposition **comps, size_t *n_comps,
                                  const DagNodeGuard **guard);

/* Incoming contract as a coverage table (exemplars + port totals).
   This is new_cov only. Do not pass it as old_cov. */
int cnet_swap_cov_from_contract(CnetSwapCoverage *cov, const Contract *c);

/* Borrow the entry's persisted incumbent certification table as old_cov.
   Returns 0, or -1 if the entry has no persisted coverage. */
int cnet_swap_old_cov_from_entry(CnetSwapCoverage *cov, const RegistryEntry *e);

/* Heap-owned unique alongside name (old_name_v2, _v3, ...). Borrowed by
   the registry; do not free. NULL if none available. */
const char *cnet_swap_alongside_name(const PrimitiveRegistry *reg,
                                     const char *old_name);

/* Same-name hook used by registry_add_certified. Weak-linked from
   contract.c so contract-only tests still link; LIBRARY / cnet.so
   provide the strong symbol. */
int cnet_swap_registry_hook(PrimitiveRegistry *reg,
                            BinaryTransformNetwork *new_btn,
                            const char *name,
                            const Contract *new_c);

#ifdef __cplusplus
}
#endif

#endif /* CNET_SWAP_H */
