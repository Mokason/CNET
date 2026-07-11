#ifndef CNET_SPECIALIST_HEALTH_H
#define CNET_SPECIALIST_HEALTH_H

/* Runtime health optimizer: one opt-in maintenance pass over a registry of
 * Specialists that FIXES what evidence says is broken and IMPROVES what
 * evidence says is ready — using only the existing certified paths. The
 * optimizer holds no authority of its own: it cannot invent validity, freeze
 * anything without a passing btn_certify, or retrain without a verified
 * target. It is a policy loop over the chokepoints the runtime already
 * trusts, plus an exact machine-readable report.
 *
 * One pass, in fix-then-improve order:
 *   1. audit    — registry_audit_certified: demote any certified entry whose
 *                 weights no longer match the certification digest.
 *   2. label    — give parked faults verified targets, from the two sources
 *                 the meltdown design names: the CONTRACT itself (a fault
 *                 input that appears in the exemplar table has a known
 *                 target) and a TEACHER (another primitive with matching
 *                 ports that handles the input cleanly).
 *   3. heal     — registry_heal per RESET entry: retrain on contract ∪
 *                 labeled faults, re-certify; FROZEN only on a passing
 *                 certify. Runtime adapters (CCE/Oracle kinds) refuse matrix
 *                 retraining by design, so they stay RESET here — their
 *                 repair path is re-acquisition (HEALTH gaps), not backprop.
 *   4. promote  — lifecycle_promote_provisional: evidence moves FUZZY
 *                 entries to PROVISIONAL (never to FROZEN; that needs proof).
 *   5. shadows  — shadow_promote_if_ready: hot-swap a shadow candidate that
 *                 out-scores its incumbent AND certifies.
 *
 * Zero-initialized config = total no-op (the zero-init-is-legacy house
 * rule); cfg == NULL selects the maintenance defaults below. Heal and shadow
 * promotion additionally need a contract source; with contracts == NULL
 * those steps are skipped and reported as such.
 * Acceptance gate: make specialist_health (SPECIALIST_HEALTH_PASS).
 */

#include <stddef.h>

#include "cnet_export.h"
#include "router.h"
#include "contract/contract.h"
#include "specialist.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolve the certified contract for a transform name. Queried with the
   entry's own name for healing and with the ACTIVE name for shadow
   promotion (a shadow certifies against the incumbent's contract). Return
   NULL for "no contract available"; the step skips that entry. */
typedef const Contract *(*SpecialistContractLookup)(const char *name,
                                                    void *ctx);

typedef struct {
    int audit;                    /* run the certificate-to-weights audit */
    int label_from_contract;      /* target source A: the contract itself */
    int label_via_teacher;        /* target source B: a clean teacher */
    int heal;                     /* retrain + re-certify RESET entries */
    size_t heal_max_epochs;       /* fixed-epoch retrain budget (4000) */
    int promote;                  /* evidence promotion FUZZY -> PROVISIONAL */
    double promote_threshold;     /* reliability floor (0.9) */
    size_t promote_min_evidence;  /* outcomes needed to promote (16) */
    int promote_shadows;          /* evidence-gated shadow hot-swap */
    size_t shadow_min_evidence;   /* outcomes a shadow needs (16) */
    SpecialistContractLookup contracts;  /* NULL = skip heal + shadows */
    void *contracts_ctx;
} SpecialistHealthConfig;

typedef struct {
    size_t entries;               /* registry size when the pass started */
    size_t demoted_by_audit;      /* stale/tampered certificates demoted */
    size_t labeled_from_contract; /* faults labeled from exemplar tables */
    size_t labeled_via_teacher;   /* faults labeled by a teacher primitive */
    size_t heal_attempted;        /* RESET entries a heal was tried on */
    size_t healed;                /* restored FROZEN via passing certify */
    size_t promoted_provisional;  /* FUZZY -> PROVISIONAL on evidence */
    size_t shadows_promoted;      /* hot-swapped incumbents */
    size_t reset_remaining;       /* still RESET when the pass ended */
    size_t trust[4];              /* end-of-pass histogram, SpecialistTrust */
} SpecialistHealthReport;

/* The maintenance preset (everything on, standard thresholds). contracts
   stays NULL — supply one to enable heal and shadow promotion. */
CNET_API void specialist_health_config_defaults(SpecialistHealthConfig *cfg);

/* Run one health pass. cfg == NULL uses the defaults; report may be NULL.
   Returns 0 on a completed pass (including an all-zero no-op), -1 on bad
   args. A pass over a healthy registry changes nothing and reports zeros —
   that no-op guarantee is part of the gate. */
CNET_API int specialist_health_pass(PrimitiveRegistry *reg,
                                    const SpecialistHealthConfig *cfg,
                                    SpecialistHealthReport *report);

#ifdef __cplusplus
}
#endif

#endif /* CNET_SPECIALIST_HEALTH_H */
