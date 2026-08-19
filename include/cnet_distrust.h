#ifndef CNET_DISTRUST_H
#define CNET_DISTRUST_H

/*
 * Distrust loop — act on seal_trust REFUSE without smuggling untrusted claims.
 *
 * Law (plans/cnet_distrust_loop.md):
 *   reroute(target) ⇒ seal_trust_decide(target) ∈ {ALLOW, EXPLORE}
 *                     OR target is explicitly Tier-C residual (uncertified by law).
 *   Never chain out of distrust into another cold domain and call it handled.
 *   Scope-out is inside an admit fence + re-exam probe — not permanent freeze.
 *   Residual never auto-CERTs.
 *
 * Autonomy tick: one unattended step miss → goal → admit (teach only) →
 * seal decide. Gated structure from measured failure; not free evolve.
 *
 * Gates: make distrust_loop → DISTRUST_LOOP_PASS
 *        make autonomy_tick  → AUTONOMY_TICK_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "cnet_seal_trust.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Reroute ------------------------------------------------------------ */

typedef enum CnetDistrustReroute {
    CNET_DISTRUST_REROUTE_ERROR = -1,
    CNET_DISTRUST_REROUTE_BLOCKED = 0,   /* target cold/REFUSE — not handled */
    CNET_DISTRUST_REROUTE_ALLOW = 1,     /* target seal ALLOW */
    CNET_DISTRUST_REROUTE_EXPLORE = 2,   /* target seal EXPLORE */
    CNET_DISTRUST_REROUTE_RESIDUAL = 3   /* explicit Tier-C residual path */
} CnetDistrustReroute;

/*
 * After source domain decides REFUSE (caller already measured that), may work
 * be handed to target_domain?
 *
 * residual_explicit != 0 → Tier-C residual path (uncertified by law; OK).
 * Else target must get ALLOW or EXPLORE from seal_trust_decide.
 * Source need not be REFUSE for residual; for certified-path reroute, source
 * should be REFUSE (we still check target).
 *
 * journal_path optional: append JSONL edge with kinds.
 */
CNET_API CnetDistrustReroute cnet_distrust_reroute(
    CnetSealTrust *st, const char *source_domain, const char *target_domain,
    int residual_explicit, const char *journal_path);

/* ---- Scope-out (admit fence + re-exam probe) --------------------------- */

#define CNET_DISTRUST_SCOPE_MAX 64
#define CNET_DISTRUST_DOM_MAX 64

typedef struct CnetDistrustScopeRow {
    char domain[CNET_DISTRUST_DOM_MAX];
    int admitted;              /* 1 if admit token accepted */
    uint64_t probe_every_n;    /* re-exam every N decides; must be >0 if admitted */
    uint64_t decides_since_probe;
    int active;                /* 1 while scoped out */
} CnetDistrustScopeRow;

typedef struct CnetDistrustScopeTable {
    CnetDistrustScopeRow rows[CNET_DISTRUST_SCOPE_MAX];
    size_t n;
    char path[512];
} CnetDistrustScopeTable;

CNET_API void cnet_distrust_scope_init(CnetDistrustScopeTable *t,
                                       const char *path);

/*
 * Register durable scope-out. Requires non-empty admit_token (admit fence)
 * AND probe_every_n >= 1. Without both → refuse (no permanent freeze).
 * Returns 0 ok, <0 error.
 */
CNET_API int cnet_distrust_scope_out(CnetDistrustScopeTable *t,
                                     const char *domain, const char *admit_token,
                                     uint64_t probe_every_n);

/* 1 if domain may take a normal serve decide; 0 if scoped and not probe turn.
 * On probe turn, still returns 1 and resets decides_since_probe. */
CNET_API int cnet_distrust_scope_may_decide(CnetDistrustScopeTable *t,
                                            const char *domain);

/* After a probe outcome under scope: if correct and enough recoveries,
 * reverse scope-out. recover_streak_needed default 3 in tests. */
CNET_API int cnet_distrust_scope_note_probe(CnetDistrustScopeTable *t,
                                            const char *domain, int correct,
                                            uint64_t recover_streak_needed);

CNET_API int cnet_distrust_scope_is_active(const CnetDistrustScopeTable *t,
                                           const char *domain);

/* ---- Autonomy tick (miss → goal → admit → seal) ------------------------ */

typedef struct CnetAutonomyTickResult {
    char domain[CNET_DISTRUST_DOM_MAX];
    int miss_rows_appended;
    int goals_queued;
    int admitted; /* 1 only after teach/has_out path — never residual prose */
    int residual_rejected; /* 1 if freeform residual was refused for CERT */
    CnetSealTrustDecision seal_decision;
    char detail[128];
} CnetAutonomyTickResult;

/*
 * One unattended step in workdir:
 *   miss.jsonl, bricks/pending_goals.txt, seal_ledger, seal_journal
 *
 * turn: teach/query/harvest string (may be NULL to only drain existing miss).
 * Never auto-CERTs residual text: freeform without teach shape → residual_rejected.
 * Admit: only when teach form has out (structured); then seed_prior is NOT used
 * alone — live outcomes are noted from the teach pairs for seal_trust.
 *
 * Returns 0 ok (including "nothing to do"), <0 hard error.
 */
CNET_API int cnet_autonomy_tick(const char *workdir, const char *turn,
                                CnetSealTrust *st /* optional; opens local if NULL */,
                                CnetAutonomyTickResult *out);

#ifdef __cplusplus
}
#endif

#endif /* CNET_DISTRUST_H */
