/* CNET ASI improve stack — library OS, defer/conformal, routing, episodes, firewall/eval.
 * Pure C, fail-closed. Does not lower CERT floors. Teacher/runtime law unchanged.
 */
#ifndef CNET_ASI_IMPROVE_H
#define CNET_ASI_IMPROVE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_ASI_NAME_MAX 64
#define CNET_ASI_CAP_MAX 256
#define CNET_ASI_MAX_SKILLS 256
#define CNET_ASI_MAX_NOT_FOR 8
#define CNET_ASI_MAX_EDGES 512
#define CNET_ASI_MAX_EP 256
#define CNET_ASI_REPORT_MAX 1024

/* ---- resolve reasons (fail-closed) ---- */
typedef enum {
    CNET_ASI_OK = 0,
    CNET_ASI_RECALL_MISS = 1,
    CNET_ASI_EXEC_BLOCKED = 2,
    CNET_ASI_CONFORMAL_ABSTAIN = 3,
    CNET_ASI_DEFER_ABSTAIN = 4,
    CNET_ASI_ROLE_BLOCKED = 5,
    CNET_ASI_ERR = -1
} CnetAsiStatus;

typedef enum {
    CNET_ASI_EDGE_DEPENDS = 1,
    CNET_ASI_EDGE_ALT = 2,
    CNET_ASI_EDGE_REFINE = 3
} CnetAsiEdgeKind;

typedef enum {
    CNET_ASI_KIND_SPECIALIST = 0,
    CNET_ASI_KIND_SHARED = 1
} CnetAsiSkillKind;

typedef struct {
    char name[CNET_ASI_NAME_MAX];
    char capability[CNET_ASI_CAP_MAX]; /* positive capability page */
    char not_for[CNET_ASI_MAX_NOT_FOR][CNET_ASI_NAME_MAX];
    int n_not_for;
    uint32_t precond_mask; /* world bits required (all must be set) */
    uint32_t privilege;    /* 0 = least privilege (preferred) */
    int kind;              /* specialist vs shared */
    int active;
    double conf_q;         /* conformal residual threshold; <0 = disabled */
    double last_residual;
    uint64_t n_select;
    uint64_t n_serve_ok;
    uint64_t n_block_exec;
    uint64_t n_block_conf;
} CnetAsiSkill;

typedef struct {
    char from[CNET_ASI_NAME_MAX];
    char to[CNET_ASI_NAME_MAX];
    int kind;
    int active;
} CnetAsiEdge;

typedef struct {
    char skill[CNET_ASI_NAME_MAX];
    uint32_t world_mask;
    int ok;
    double residual;
    uint64_t tick;
    int active;
} CnetAsiEpisode;

typedef struct {
    CnetAsiSkill skills[CNET_ASI_MAX_SKILLS];
    size_t n_skills;
    CnetAsiEdge edges[CNET_ASI_MAX_EDGES];
    size_t n_edges;
    CnetAsiEpisode ep[CNET_ASI_MAX_EP];
    size_t ep_i; /* ring next write */
    size_t ep_n; /* filled count */
    uint64_t tick;

    /* resolve / debt stats */
    uint64_t n_resolve_ok;
    uint64_t n_recall_miss;
    uint64_t n_exec_block;
    uint64_t n_conf_abstain;
    uint64_t n_defer_abstain;
    uint64_t n_role_block;
    uint64_t n_debt_orphan_edge;
    uint64_t n_debt_dup_cap;
    uint64_t n_debt_empty_cap;
    uint64_t n_debt_broken_not_for;
} CnetAsiLib;

/* ---- lifecycle ---- */
void cnet_asi_init(CnetAsiLib *L);
void cnet_asi_reset_stats(CnetAsiLib *L);

/* ---- Slice A: library OS ---- */
int cnet_asi_add_skill(CnetAsiLib *L, const char *name, const char *capability,
                       uint32_t precond_mask, uint32_t privilege, int kind);
int cnet_asi_add_not_for(CnetAsiLib *L, const char *name, const char *neighbor);
int cnet_asi_set_conf_q(CnetAsiLib *L, const char *name, double conf_q);
int cnet_asi_add_edge(CnetAsiLib *L, const char *from, const char *to, int kind);

/* Three-stage resolve:
 *  1) recall by exact name OR capability substring match (case-sensitive)
 *  2) executability: (world_mask & precond) == precond
 *  3) among survivors pick lowest privilege, then stable name order
 * Optional residual: if conf_q >= 0 and residual > conf_q → CONFORMAL_ABSTAIN
 * query may be skill name or capability keyword.
 */
int cnet_asi_resolve(CnetAsiLib *L, const char *query, uint32_t world_mask,
                     double residual, const char **out_name);

/* Janitor: scan debt; writes short report; returns total debt count. */
int cnet_asi_janitor(CnetAsiLib *L, char *report, size_t cap);

/* Composition helpers: count outgoing edges of kind; 0 if unknown skill. */
int cnet_asi_edge_count(const CnetAsiLib *L, const char *from, int kind);

/* ---- Slice B: multi-expert defer (L2D-lite) ----
 * scores[i] higher better; eligible[i]=1 if expert may answer.
 * Pick argmax if score_best - score_second >= min_margin; else abstain (-1).
 * Returns pick index or -1.
 */
int cnet_asi_defer_pick(const double *scores, const int *eligible, int n,
                        double min_margin);

/* ---- Slice C: elbow top-M + specialization ----
 * scores descending not required; selects up to max_m indices with score within
 * gap of best, stopping early when successive gap (sorted) exceeds elbow_gap.
 * Returns k written to idx_out.
 */
int cnet_asi_elbow_topm(const double *scores, int n, int max_m, double elbow_gap,
                        int *idx_out);

/* Herfindahl-style specialization on select counts: 1 = one expert all mass,
 * ~1/n = uniform. n_active counts positive counts only if >0. */
double cnet_asi_specialization_hhi(const uint64_t *counts, int n);

/* Shared vs specialist counts currently registered. */
void cnet_asi_kind_counts(const CnetAsiLib *L, int *n_shared, int *n_spec);

/* ---- Slice D: episodic memory (never serves; evidence only) ---- */
int cnet_asi_episode_log(CnetAsiLib *L, const char *skill, uint32_t world_mask,
                         int ok, double residual);
/* Find most recent episode for skill; returns 1 if found. */
int cnet_asi_episode_last(const CnetAsiLib *L, const char *skill,
                          CnetAsiEpisode *out);
/* Continual regression: for each skill, if any past ok episode exists and
 * latest is not ok → count regression. */
int cnet_asi_continual_regressions(const CnetAsiLib *L);

/* ---- Slice E: role firewall + anytime-valid bake-off ---- */
/* Roles and effect classes are closed ints set by product. */
int cnet_asi_role_allow(int role, int effect_class, const int *allow_matrix,
                        int n_roles, int n_effects);
/* matrix row-major: allow_matrix[role * n_effects + effect] */

/* Hoeffding-style anytime compare of two Bernoulli win rates.
 * wins_a/b among n paired trials (exactly one winner each trial, or ties ignored
 * by only counting decisive trials in n).
 * Returns 1 if A certified better, -1 if B, 0 if continue. alpha in (0,1). */
int cnet_asi_av_compare(int wins_a, int wins_b, int n_decisive, double alpha);

#ifdef __cplusplus
}
#endif

#endif
