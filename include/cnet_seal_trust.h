#ifndef CNET_SEAL_TRUST_H
#define CNET_SEAL_TRUST_H

/*
 * Domain seal-trust calibration (middle path).
 *
 * Per-domain ledger of seal outcomes → Wilson lower confidence bound (LCB).
 * Serve may claim Tier-A "certified" authority only when LCB clears the bar.
 * Cold start (n=0) and sparse n yield LCB≈0 → refuse (ignorance == refuse).
 *
 * Mechanism family: threshold a risk estimate and refuse when the bound is
 * too high — same *object* as Conformal Risk Control (Angelopoulos, Bates
 * et al.). We use a Wilson binomial LCB on delayed seal outcomes (not full
 * CRC conformal scores). Cite CRC for the threshold-refusal idea; do not
 * claim a novel statistical invention. Local prior art also includes
 * acquire.c's coverage_accuracy_lower_bound + min_accuracy_bound.
 *
 * Exploration: when the decision would refuse, a small forced seal rate keeps
 * the estimator alive under feedback (refusal freezes the interval otherwise).
 * Explore seals are a distinct KIND from trust seals (journal + report flag).
 *
 * Cold-start: at min_lcb=0.95, z=1.96, need ~73 consecutive correct outcomes
 * before LCB clears (one error → ~110). Prefer seed_prior from acquire/certify
 * holdout counts over waiting for explore-only maturation.
 *
 * Scope: domain-level trust. Does NOT solve in-domain aliasing tails
 * (see sevenseg ens_agree_wrong). Claim: domain LCB refuse-on-ignorance.
 *
 * Gate: make seal_trust → SEAL_TRUST_PASS
 */

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_SEAL_TRUST_DOMAIN_MAX 64
#define CNET_SEAL_TRUST_ROWS_MAX 256
#define CNET_SEAL_TRUST_PATH_MAX 512

typedef enum CnetSealTrustDecision {
    CNET_SEAL_TRUST_ERROR = -1,
    CNET_SEAL_TRUST_REFUSE = 0,  /* LCB < bar or cold — do not claim Tier A */
    CNET_SEAL_TRUST_ALLOW = 1,   /* LCB >= bar — trust seal */
    CNET_SEAL_TRUST_EXPLORE = 2  /* would refuse; forced seal to keep estimator */
} CnetSealTrustDecision;

/* Per-seal artifact kind — never collapse explore into "certified trust". */
typedef enum CnetSealKind {
    CNET_SEAL_KIND_NONE = 0,
    CNET_SEAL_KIND_TRUST = 1,
    CNET_SEAL_KIND_EXPLORE = 2,
    CNET_SEAL_KIND_PRIOR = 3 /* seed_prior from holdout/certify (not live seal) */
} CnetSealKind;

typedef struct CnetSealTrustConfig {
    double min_lcb;              /* Wilson lower-bound floor (default 0.95) */
    double wilson_z;             /* default 1.96 */
    double explore_rate;         /* steady-state explore when refusing (0.05) */
    double explore_warmup_rate;  /* higher rate while seals < warmup_until_n */
    uint64_t explore_warmup_until_n; /* default 40 outcomes */
    /* Live outcomes required before ALLOW, even if prior seeds LCB over bar.
     * Blocks free-verifier smuggle: offline certify ≠ serve-time trust.
     * prior_seeded never counts toward this. Default 10. */
    uint64_t min_live_outcomes;
    int enabled;                 /* 0 = always ALLOW (operator override) */
} CnetSealTrustConfig;

typedef struct CnetSealDomainRow {
    char domain[CNET_SEAL_TRUST_DOMAIN_MAX];
    uint64_t seals;         /* total outcomes = prior_seeded + live */
    uint64_t correct;       /* among seals */
    uint64_t explores;      /* explore-kind seals issued */
    uint64_t trust_seals;   /* trust-kind seals issued (LCB cleared) */
    uint64_t prior_seeded;  /* outcomes injected via seed_prior */
    uint64_t live_outcomes; /* note_outcome count (serve/explore delayed truth) */
    uint64_t pending;       /* sealed, outcome not yet known */
    uint64_t refuses;       /* decide→REFUSE count (telemetry) */
} CnetSealDomainRow;

typedef struct CnetSealTrust {
    CnetSealTrustConfig cfg;
    CnetSealDomainRow rows[CNET_SEAL_TRUST_ROWS_MAX];
    size_t n_rows;
    char path[CNET_SEAL_TRUST_PATH_MAX];
    char journal_path[CNET_SEAL_TRUST_PATH_MAX]; /* per-seal kind log */
    int loaded;
    uint64_t decide_seq; /* monotonic for deterministic explore schedule */
} CnetSealTrust;

CNET_API void cnet_seal_trust_config_defaults(CnetSealTrustConfig *c);

/* Open empty ledger; optional path for save/load. */
CNET_API int cnet_seal_trust_open(CnetSealTrust *st, const char *path,
                                  const CnetSealTrustConfig *cfg);
CNET_API void cnet_seal_trust_close(CnetSealTrust *st);

/* Optional append-only journal of seal/outcome events with kind labels. */
CNET_API void cnet_seal_trust_set_journal(CnetSealTrust *st, const char *path);

CNET_API int cnet_seal_trust_load(CnetSealTrust *st, const char *path);
CNET_API int cnet_seal_trust_save(const CnetSealTrust *st, const char *path);

/* Wilson LCB for a domain (0 if n=0). */
CNET_API double cnet_seal_trust_lcb(const CnetSealTrust *st, const char *domain);

/* Smallest n with n consecutive correct outcomes such that LCB >= min_lcb. */
CNET_API size_t cnet_seal_trust_min_n_all_correct(double min_lcb, double z);

/*
 * Decide whether Tier-A seal authority may be claimed for domain.
 * domain empty/NULL → REFUSE when enabled (no anonymous trust).
 */
CNET_API CnetSealTrustDecision cnet_seal_trust_decide(CnetSealTrust *st,
                                                      const char *domain);

/* Record that a seal was issued (pending outcome). is_explore=1 if EXPLORE.
 * Writes journal kind=trust|explore when journal set. */
CNET_API int cnet_seal_trust_note_seal(CnetSealTrust *st, const char *domain,
                                       int is_explore);

/* Delayed ground truth: one outcome for a prior seal. correct=0/1. */
CNET_API int cnet_seal_trust_note_outcome(CnetSealTrust *st, const char *domain,
                                          int correct);

/*
 * Seed ledger from prior i.i.d.-ish outcomes (acquire holdout / certify).
 * Adds to seals/correct without pending. Tagged prior_seeded.
 * Does NOT claim those seals were live serve seals.
 * Does NOT alone clear ALLOW: decide() also requires min_live_outcomes
 * from note_outcome (serve/explore delayed truth).
 * Returns 0 ok, <0 error.
 */
CNET_API int cnet_seal_trust_seed_prior(CnetSealTrust *st, const char *domain,
                                        size_t correct, size_t n);

/* Lookup row (NULL if unknown). */
CNET_API const CnetSealDomainRow *cnet_seal_trust_find(const CnetSealTrust *st,
                                                       const char *domain);

/* Process-global helper for personal_ai.
 * CNET_SEAL_TRUST=1 enables (default off).
 * CNET_SEAL_TRUST_LEDGER, CNET_SEAL_TRUST_JOURNAL,
 * CNET_SEAL_TRUST_MIN_LCB, CNET_SEAL_TRUST_EXPLORE,
 * CNET_SEAL_TRUST_WARMUP_EXPLORE, CNET_SEAL_TRUST_WARMUP_N,
 * CNET_SEAL_TRUST_MIN_LIVE (default 10; prior alone cannot ALLOW). */
CNET_API CnetSealTrust *cnet_seal_trust_global(void);
CNET_API void cnet_seal_trust_global_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* CNET_SEAL_TRUST_H */
