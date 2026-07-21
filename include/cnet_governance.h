/* CNET governance policy (G2) — human constitution, machine enforcement bounds.
 * Policy is data: janitor/lane may act only inside these ranges.
 * No freeform model rewrites this file at runtime.
 */
#ifndef CNET_GOVERNANCE_H
#define CNET_GOVERNANCE_H

#include "cnet_export.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_GOV_PATH_MAX 512
#define CNET_GOV_VERSION 1

typedef struct {
    int version;
    /* Soft caps (warn in report; hard refuse only when refuse_over_cap=1) */
    size_t max_units;
    size_t warn_units;           /* warn when units >= this */
    double low_rel_floor;
    size_t low_rel_min_evidence;
    int note_low_rel;            /* 1 = write LOW_RELIABILITY gaps */
    int snapshot_on_run;         /* 1 = pin CNB copy each janitor run */
    size_t max_snapshots;        /* rotate oldest beyond this (0 = unlimited) */
    int refuse_if_stop;          /* 1 = honor <base>.stop (default) */
    int refuse_over_cap;         /* 1 = exit non-zero if units > max_units */
    size_t max_waiting_oracle_warn;
    char pin_dir[CNET_GOV_PATH_MAX];
    char policy_path[CNET_GOV_PATH_MAX];
} CnetGovernancePolicy;

/* Defaults for personal-AI soul base. */
CNET_API void cnet_gov_policy_defaults(CnetGovernancePolicy *p);

/* Load key=value file (and/or overlay env CNET_GOV_*). Missing file = defaults.
 * Returns 0 on success. */
CNET_API int cnet_gov_policy_load(CnetGovernancePolicy *p, const char *path);

/* Write current policy as key=value (human editable). */
CNET_API int cnet_gov_policy_save(const CnetGovernancePolicy *p,
                                  const char *path);

/* Apply env overlays: CNET_GOV_MAX_UNITS, CNET_GOV_LOW_REL, etc. */
CNET_API void cnet_gov_policy_apply_env(CnetGovernancePolicy *p);

/* Snapshot base into pin_dir as pin_<stamp>_<basename>.cnb
 * Updates pin_dir/CURRENT pointer file. Rotates if max_snapshots>0.
 * Returns 0 on success; out_path optional. */
CNET_API int cnet_gov_pin_snapshot(const CnetGovernancePolicy *p,
                                   const char *base_path, char *out_path,
                                   size_t out_cap);

/* Read pin_dir/CURRENT into out. */
CNET_API int cnet_gov_current_pin(const CnetGovernancePolicy *p, char *out,
                                  size_t out_cap);

/* Restore: copy pin_path over base_path. Caller must stop learner first.
 * Writes base_path.restore.log. Returns 0 on success. */
CNET_API int cnet_gov_restore_pin(const char *pin_path, const char *base_path);

#ifdef __cplusplus
}
#endif
#endif
