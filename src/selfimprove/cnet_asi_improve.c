#include "../../include/cnet_asi_improve.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int name_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static CnetAsiSkill *find_skill(CnetAsiLib *L, const char *name) {
    size_t i;
    if (!L || !name) return NULL;
    for (i = 0; i < L->n_skills; i++)
        if (L->skills[i].active && name_eq(L->skills[i].name, name))
            return &L->skills[i];
    return NULL;
}

void cnet_asi_init(CnetAsiLib *L) {
    if (!L) return;
    memset(L, 0, sizeof *L);
}

void cnet_asi_reset_stats(CnetAsiLib *L) {
    if (!L) return;
    L->n_resolve_ok = L->n_recall_miss = L->n_exec_block = 0;
    L->n_conf_abstain = L->n_defer_abstain = L->n_role_block = 0;
    L->n_debt_orphan_edge = L->n_debt_dup_cap = L->n_debt_empty_cap =
        L->n_debt_broken_not_for = 0;
}

int cnet_asi_add_skill(CnetAsiLib *L, const char *name, const char *capability,
                       uint32_t precond_mask, uint32_t privilege, int kind) {
    CnetAsiSkill *s;
    if (!L || !name || !name[0]) return -1;
    if (find_skill(L, name)) return -2;
    if (L->n_skills >= CNET_ASI_MAX_SKILLS) return -1;
    s = &L->skills[L->n_skills++];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    if (capability)
        snprintf(s->capability, sizeof s->capability, "%s", capability);
    s->precond_mask = precond_mask;
    s->privilege = privilege;
    s->kind = kind;
    s->active = 1;
    s->conf_q = -1.0; /* disabled */
    return 0;
}

int cnet_asi_add_not_for(CnetAsiLib *L, const char *name, const char *neighbor) {
    CnetAsiSkill *s = find_skill(L, name);
    if (!s || !neighbor || !neighbor[0]) return -1;
    if (s->n_not_for >= CNET_ASI_MAX_NOT_FOR) return -1;
    snprintf(s->not_for[s->n_not_for], sizeof s->not_for[0], "%s", neighbor);
    s->n_not_for++;
    return 0;
}

int cnet_asi_set_conf_q(CnetAsiLib *L, const char *name, double conf_q) {
    CnetAsiSkill *s = find_skill(L, name);
    if (!s) return -1;
    s->conf_q = conf_q;
    return 0;
}

int cnet_asi_add_edge(CnetAsiLib *L, const char *from, const char *to, int kind) {
    CnetAsiEdge *e;
    if (!L || !from || !to || !from[0] || !to[0]) return -1;
    if (L->n_edges >= CNET_ASI_MAX_EDGES) return -1;
    e = &L->edges[L->n_edges++];
    memset(e, 0, sizeof *e);
    snprintf(e->from, sizeof e->from, "%s", from);
    snprintf(e->to, sizeof e->to, "%s", to);
    e->kind = kind;
    e->active = 1;
    return 0;
}

int cnet_asi_edge_count(const CnetAsiLib *L, const char *from, int kind) {
    size_t i;
    int c = 0;
    if (!L || !from) return 0;
    for (i = 0; i < L->n_edges; i++)
        if (L->edges[i].active && name_eq(L->edges[i].from, from) &&
            L->edges[i].kind == kind)
            c++;
    return c;
}

static int skill_matches_query(const CnetAsiSkill *s, const char *query) {
    if (!s || !query || !query[0]) return 0;
    if (name_eq(s->name, query)) return 1;
    if (s->capability[0] && strstr(s->capability, query) != NULL) return 1;
    return 0;
}

static int exec_ok(const CnetAsiSkill *s, uint32_t world_mask) {
    return (world_mask & s->precond_mask) == s->precond_mask;
}

int cnet_asi_resolve(CnetAsiLib *L, const char *query, uint32_t world_mask,
                     double residual, const char **out_name) {
    size_t i;
    CnetAsiSkill *best = NULL;
    int any_match = 0, any_exec_fail = 0;

    if (out_name) *out_name = NULL;
    if (!L || !query || !query[0]) return CNET_ASI_ERR;
    L->tick++;

    for (i = 0; i < L->n_skills; i++) {
        CnetAsiSkill *s = &L->skills[i];
        if (!s->active) continue;
        if (!skill_matches_query(s, query)) continue;
        any_match = 1;
        if (!exec_ok(s, world_mask)) {
            any_exec_fail = 1;
            s->n_block_exec++;
            continue;
        }
        /* conformal gate if enabled and residual provided (>=0 means measured) */
        if (s->conf_q >= 0.0 && residual >= 0.0 && residual > s->conf_q) {
            s->n_block_conf++;
            s->last_residual = residual;
            continue; /* treat as not eligible; may become conf abstain */
        }
        if (!best || s->privilege < best->privilege ||
            (s->privilege == best->privilege && strcmp(s->name, best->name) < 0))
            best = s;
    }

    if (!any_match) {
        L->n_recall_miss++;
        return CNET_ASI_RECALL_MISS;
    }
    if (!best) {
        /* matched something but none executable / conformal-clear */
        /* Prefer exec_block if any exec fail without conformal-only path.
         * If all blocked by conformal only, CONFORMAL_ABSTAIN. */
        int conf_only = 0, exec_only = 0;
        for (i = 0; i < L->n_skills; i++) {
            CnetAsiSkill *s = &L->skills[i];
            if (!s->active || !skill_matches_query(s, query)) continue;
            if (!exec_ok(s, world_mask)) {
                exec_only = 1;
                continue;
            }
            if (s->conf_q >= 0.0 && residual >= 0.0 && residual > s->conf_q)
                conf_only = 1;
        }
        if (exec_only || any_exec_fail) {
            L->n_exec_block++;
            return CNET_ASI_EXEC_BLOCKED;
        }
        if (conf_only) {
            L->n_conf_abstain++;
            return CNET_ASI_CONFORMAL_ABSTAIN;
        }
        L->n_exec_block++;
        return CNET_ASI_EXEC_BLOCKED;
    }

    best->n_select++;
    best->n_serve_ok++;
    best->last_residual = residual;
    L->n_resolve_ok++;
    if (out_name) *out_name = best->name;
    return CNET_ASI_OK;
}

int cnet_asi_janitor(CnetAsiLib *L, char *report, size_t cap) {
    size_t i, j;
    int debt = 0;
    char buf[CNET_ASI_REPORT_MAX];
    size_t used = 0;

    if (!L) return -1;
    L->n_debt_orphan_edge = L->n_debt_dup_cap = L->n_debt_empty_cap =
        L->n_debt_broken_not_for = 0;
    buf[0] = 0;

    /* empty capability pages */
    for (i = 0; i < L->n_skills; i++) {
        if (!L->skills[i].active) continue;
        if (!L->skills[i].capability[0]) {
            L->n_debt_empty_cap++;
            debt++;
        }
    }

    /* duplicate capability pages (exact string) */
    for (i = 0; i < L->n_skills; i++) {
        if (!L->skills[i].active || !L->skills[i].capability[0]) continue;
        for (j = i + 1; j < L->n_skills; j++) {
            if (!L->skills[j].active) continue;
            if (strcmp(L->skills[i].capability, L->skills[j].capability) == 0) {
                L->n_debt_dup_cap++;
                debt++;
            }
        }
    }

    /* orphan / broken edges */
    for (i = 0; i < L->n_edges; i++) {
        if (!L->edges[i].active) continue;
        if (!find_skill(L, L->edges[i].from) || !find_skill(L, L->edges[i].to)) {
            L->n_debt_orphan_edge++;
            debt++;
        }
    }

    /* broken not_for neighbors */
    for (i = 0; i < L->n_skills; i++) {
        CnetAsiSkill *s = &L->skills[i];
        int k;
        if (!s->active) continue;
        for (k = 0; k < s->n_not_for; k++) {
            if (!find_skill(L, s->not_for[k])) {
                L->n_debt_broken_not_for++;
                debt++;
            }
        }
    }

    used = (size_t)snprintf(
        buf, sizeof buf,
        "debt=%d empty_cap=%llu dup_cap=%llu orphan_edge=%llu broken_not_for=%llu",
        debt, (unsigned long long)L->n_debt_empty_cap,
        (unsigned long long)L->n_debt_dup_cap,
        (unsigned long long)L->n_debt_orphan_edge,
        (unsigned long long)L->n_debt_broken_not_for);
    if (report && cap) {
        if (used >= cap) used = cap - 1;
        memcpy(report, buf, used);
        report[used] = 0;
    }
    return debt;
}

int cnet_asi_defer_pick(const double *scores, const int *eligible, int n,
                        double min_margin) {
    int i, best = -1, second = -1;
    if (!scores || !eligible || n <= 0) return -1;
    for (i = 0; i < n; i++) {
        if (!eligible[i]) continue;
        if (best < 0 || scores[i] > scores[best]) {
            second = best;
            best = i;
        } else if (second < 0 || scores[i] > scores[second]) {
            second = i;
        }
    }
    if (best < 0) return -1;
    if (second < 0) return best; /* sole expert */
    if (scores[best] - scores[second] + 1e-15 < min_margin) return -1;
    return best;
}

int cnet_asi_elbow_topm(const double *scores, int n, int max_m, double elbow_gap,
                        int *idx_out) {
    int i, k = 0, best_i = 0;
    int order[CNET_ASI_MAX_SKILLS];
    if (!scores || !idx_out || n <= 0 || max_m <= 0) return 0;
    if (n > CNET_ASI_MAX_SKILLS) n = CNET_ASI_MAX_SKILLS;
    if (max_m > n) max_m = n;

    for (i = 0; i < n; i++) order[i] = i;
    /* simple selection sort descending by score */
    for (i = 0; i < n; i++) {
        int j, bi = i;
        for (j = i + 1; j < n; j++)
            if (scores[order[j]] > scores[order[bi]]) bi = j;
        if (bi != i) {
            int tmp = order[i];
            order[i] = order[bi];
            order[bi] = tmp;
        }
    }
    best_i = order[0];
    idx_out[k++] = best_i;
    for (i = 1; i < n && k < max_m; i++) {
        double gap = scores[order[i - 1]] - scores[order[i]];
        if (gap > elbow_gap) break;
        /* also require within elbow of global best optional: keep successive */
        idx_out[k++] = order[i];
    }
    return k;
}

double cnet_asi_specialization_hhi(const uint64_t *counts, int n) {
    double sum = 0.0, h = 0.0;
    int i;
    if (!counts || n <= 0) return 0.0;
    for (i = 0; i < n; i++) sum += (double)counts[i];
    if (sum <= 0.0) return 0.0;
    for (i = 0; i < n; i++) {
        double p = (double)counts[i] / sum;
        h += p * p;
    }
    return h;
}

void cnet_asi_kind_counts(const CnetAsiLib *L, int *n_shared, int *n_spec) {
    size_t i;
    int sh = 0, sp = 0;
    if (!L) {
        if (n_shared) *n_shared = 0;
        if (n_spec) *n_spec = 0;
        return;
    }
    for (i = 0; i < L->n_skills; i++) {
        if (!L->skills[i].active) continue;
        if (L->skills[i].kind == CNET_ASI_KIND_SHARED)
            sh++;
        else
            sp++;
    }
    if (n_shared) *n_shared = sh;
    if (n_spec) *n_spec = sp;
}

int cnet_asi_episode_log(CnetAsiLib *L, const char *skill, uint32_t world_mask,
                         int ok, double residual) {
    CnetAsiEpisode *e;
    if (!L || !skill || !skill[0]) return -1;
    e = &L->ep[L->ep_i % CNET_ASI_MAX_EP];
    memset(e, 0, sizeof *e);
    snprintf(e->skill, sizeof e->skill, "%s", skill);
    e->world_mask = world_mask;
    e->ok = ok ? 1 : 0;
    e->residual = residual;
    e->tick = ++L->tick;
    e->active = 1;
    L->ep_i++;
    if (L->ep_n < CNET_ASI_MAX_EP) L->ep_n++;
    return 0;
}

int cnet_asi_episode_last(const CnetAsiLib *L, const char *skill,
                          CnetAsiEpisode *out) {
    size_t n, k;
    if (!L || !skill || !out) return 0;
    n = L->ep_n;
    if (n > CNET_ASI_MAX_EP) n = CNET_ASI_MAX_EP;
    for (k = 0; k < n; k++) {
        /* walk newest first */
        size_t idx = (L->ep_i + CNET_ASI_MAX_EP - 1 - k) % CNET_ASI_MAX_EP;
        const CnetAsiEpisode *e = &L->ep[idx];
        if (!e->active) continue;
        if (name_eq(e->skill, skill)) {
            *out = *e;
            return 1;
        }
    }
    return 0;
}

int cnet_asi_continual_regressions(const CnetAsiLib *L) {
    size_t i, k, n;
    int reg = 0;
    if (!L) return 0;
    n = L->ep_n;
    if (n > CNET_ASI_MAX_EP) n = CNET_ASI_MAX_EP;

    for (i = 0; i < L->n_skills; i++) {
        const char *name;
        int saw_ok = 0;
        int latest_ok = -1;
        uint64_t latest_tick = 0;
        if (!L->skills[i].active) continue;
        name = L->skills[i].name;
        for (k = 0; k < n; k++) {
            const CnetAsiEpisode *e = &L->ep[k];
            if (!e->active || !name_eq(e->skill, name)) continue;
            if (e->ok) saw_ok = 1;
            if (e->tick >= latest_tick) {
                latest_tick = e->tick;
                latest_ok = e->ok;
            }
        }
        if (saw_ok && latest_ok == 0) reg++;
    }
    return reg;
}

int cnet_asi_role_allow(int role, int effect_class, const int *allow_matrix,
                        int n_roles, int n_effects) {
    if (!allow_matrix || n_roles <= 0 || n_effects <= 0) return 0;
    if (role < 0 || role >= n_roles || effect_class < 0 ||
        effect_class >= n_effects)
        return 0;
    return allow_matrix[role * n_effects + effect_class] ? 1 : 0;
}

int cnet_asi_av_compare(int wins_a, int wins_b, int n_decisive, double alpha) {
    double n, pa, pb, gap, bound;
    if (n_decisive <= 0 || alpha <= 0.0 || alpha >= 1.0) return 0;
    if (wins_a < 0 || wins_b < 0 || wins_a + wins_b > n_decisive) return 0;
    n = (double)n_decisive;
    pa = (double)wins_a / n;
    pb = (double)wins_b / n;
    gap = pa - pb;
    /* Hoeffding bound on mean difference of bounded [-1,1] outcomes if A win=+1 B
     * win=-1: simplified use  sqrt(ln(2/alpha)/(2n)) * 2 for difference of two means
     * conservative: bound = sqrt(log(2/alpha)/(2n)) */
    bound = sqrt(log(2.0 / alpha) / (2.0 * n));
    /* scale: each trial contributes difference in {+1,-1,0}; use 2*bound */
    bound *= 2.0;
    if (gap > bound) return 1;
    if (-gap > bound) return -1;
    return 0;
}
