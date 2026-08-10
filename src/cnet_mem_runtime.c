#include "../include/cnet_mem_runtime.h"
#include "../include/cnet_capsule.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int name_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

static CnetMemSlot *stm_find(CnetMemRuntime *m, const char *name) {
    size_t i;
    for (i = 0; i < m->stm_n; i++)
        if (m->stm[i].active && name_eq(m->stm[i].name, name)) return &m->stm[i];
    return NULL;
}

static CnetMemSlot *ltm_find(CnetMemRuntime *m, const char *name) {
    size_t i;
    for (i = 0; i < m->ltm_n; i++)
        if (m->ltm[i].active && name_eq(m->ltm[i].name, name)) return &m->ltm[i];
    return NULL;
}

static int unit_in_reg(const PrimitiveRegistry *reg, const char *name) {
    size_t i;
    if (!reg || !name) return 0;
    for (i = 0; i < reg->count; i++)
        if (reg->entries[i].name && strcmp(reg->entries[i].name, name) == 0 &&
            reg->entries[i].certified)
            return 1;
    return 0;
}

static int rebuild_reg(CnetMemRuntime *m) {
    size_t skipped = 0;
    if (m->reg_ready) {
        registry_free(&m->reg);
        m->reg_ready = 0;
    }
    registry_init(&m->reg);
    m->reg_ready = 1;
    if (!m->base_ready) return -1;
    return cnb_load_registry(&m->base, &m->reg, &skipped);
}

static int skill_has_regression(const CnetMemRuntime *m, const char *name) {
    size_t k, n;
    int saw_ok = 0;
    int latest_ok = -1;
    uint64_t latest_tick = 0;
    if (!m || !name) return 0;
    n = m->asi.ep_n;
    if (n > CNET_ASI_MAX_EP) n = CNET_ASI_MAX_EP;
    for (k = 0; k < n; k++) {
        const CnetAsiEpisode *e = &m->asi.ep[k];
        if (!e->active || !name_eq(e->skill, name)) continue;
        if (e->ok) saw_ok = 1;
        if (e->tick >= latest_tick) {
            latest_tick = e->tick;
            latest_ok = e->ok;
        }
    }
    return saw_ok && latest_ok == 0;
}

void cnet_mem_init(CnetMemRuntime *m, size_t stm_cap) {
    memset(m, 0, sizeof *m);
    m->stm_cap = stm_cap && stm_cap <= CNET_MEM_STM_MAX ? stm_cap : 16;
    cnb_init(&m->base);
    m->base_ready = 1;
    hybrid_ai_init(&m->cov);
    registry_init(&m->reg);
    m->reg_ready = 1;
    cnet_asi_init(&m->asi);
    m->asi_gate = 1;
}

void cnet_mem_free(CnetMemRuntime *m) {
    if (!m) return;
    if (m->reg_ready) registry_free(&m->reg);
    if (m->base_ready) cnb_free(&m->base);
    hybrid_ai_free(&m->cov);
    memset(m, 0, sizeof *m);
}

int cnet_mem_ltm_add(CnetMemRuntime *m, const char *name, const char *capsule_dir) {
    size_t i;
    if (!m || !name || !name[0] || !capsule_dir || !capsule_dir[0]) return -1;
    for (i = 0; i < m->ltm_n; i++) {
        if (m->ltm[i].active && name_eq(m->ltm[i].name, name)) {
            snprintf(m->ltm[i].capsule_dir, sizeof m->ltm[i].capsule_dir, "%s",
                     capsule_dir);
            /* keep ASI entry if present */
            return 0;
        }
    }
    if (m->ltm_n >= CNET_MEM_LTM_MAX) return -1;
    memset(&m->ltm[m->ltm_n], 0, sizeof m->ltm[0]);
    snprintf(m->ltm[m->ltm_n].name, sizeof m->ltm[0].name, "%s", name);
    snprintf(m->ltm[m->ltm_n].capsule_dir, sizeof m->ltm[0].capsule_dir, "%s",
             capsule_dir);
    m->ltm[m->ltm_n].active = 1;
    m->ltm_n++;
    /* Auto-catalog in ASI (open world, specialist, no conformal) if missing */
    if (cnet_asi_add_skill(&m->asi, name, name, 0u, 0u, CNET_ASI_KIND_SPECIALIST) != 0) {
        /* already present is ok (-2) */
    }
    return 0;
}

int cnet_mem_asi_configure(CnetMemRuntime *m, const char *name,
                           const char *capability, uint32_t precond_mask,
                           uint32_t privilege, int kind, double conf_q) {
    size_t i;
    CnetAsiSkill *s = NULL;
    if (!m || !name || !name[0]) return -1;
    for (i = 0; i < m->asi.n_skills; i++) {
        if (m->asi.skills[i].active && name_eq(m->asi.skills[i].name, name)) {
            s = &m->asi.skills[i];
            break;
        }
    }
    if (!s) {
        if (cnet_asi_add_skill(&m->asi, name, capability ? capability : name,
                               precond_mask, privilege, kind) != 0)
            return -1;
        return cnet_asi_set_conf_q(&m->asi, name, conf_q);
    }
    if (capability && capability[0])
        snprintf(s->capability, sizeof s->capability, "%s", capability);
    s->precond_mask = precond_mask;
    s->privilege = privilege;
    s->kind = kind;
    s->conf_q = conf_q;
    return 0;
}

int cnet_mem_stm_pin(CnetMemRuntime *m, const char *name) {
    CnetMemSlot *s;
    size_t i;
    if (!m || !name || !name[0]) return -1;
    if (!unit_in_reg(&m->reg, name)) return -1; /* only CERT */
    s = stm_find(m, name);
    if (s) {
        s->last_tick = ++m->tick;
        return 0;
    }
    /* evict lowest hits if full */
    if (m->stm_n >= m->stm_cap) {
        size_t victim = 0;
        for (i = 1; i < m->stm_n; i++)
            if (m->stm[i].hits < m->stm[victim].hits ||
                (m->stm[i].hits == m->stm[victim].hits &&
                 m->stm[i].last_tick < m->stm[victim].last_tick))
                victim = i;
        m->stm[victim] = m->stm[m->stm_n - 1];
        memset(&m->stm[m->stm_n - 1], 0, sizeof m->stm[0]);
        m->stm_n--;
    }
    if (m->stm_n >= CNET_MEM_STM_MAX) return -1;
    memset(&m->stm[m->stm_n], 0, sizeof m->stm[0]);
    snprintf(m->stm[m->stm_n].name, sizeof m->stm[0].name, "%s", name);
    m->stm[m->stm_n].active = 1;
    m->stm[m->stm_n].last_tick = ++m->tick;
    m->stm_n++;
    return 0;
}

static int ltm_fetch_into_base(CnetMemRuntime *m, CnetMemSlot *lt) {
    CnetCapsuleReport rep;
    if (!lt || !lt->capsule_dir[0]) return -1;
    if (cnb_has_unit(&m->base, lt->name)) {
        if (rebuild_reg(m) != 0) return -1;
        return 0;
    }
    memset(&rep, 0, sizeof rep);
    if (cnet_capsule_import(&m->base, &m->cov, lt->capsule_dir, &rep) != 0) return -1;
    if (rebuild_reg(m) != 0) return -1;
    if (!unit_in_reg(&m->reg, lt->name)) return -1;
    m->n_ltm_fetch++;
    return 0;
}

static int mem_resolve_body(CnetMemRuntime *m, const char *name, const char **unit_out) {
    CnetMemSlot *s, *lt;
    /* 1) STM hit */
    s = stm_find(m, name);
    if (s && unit_in_reg(&m->reg, name)) {
        s->hits++;
        s->last_tick = m->tick;
        m->n_stm_hit++;
        if (unit_out) *unit_out = s->name;
        return CNET_MEM_OK;
    }

    /* 2) LTM fetch */
    lt = ltm_find(m, name);
    if (lt) {
        if (ltm_fetch_into_base(m, lt) == 0 && unit_in_reg(&m->reg, name)) {
            (void)cnet_mem_stm_pin(m, name);
            s = stm_find(m, name);
            if (s) s->hits++;
            if (stm_find(m, name)) m->n_stm_hit++;
            if (unit_out) *unit_out = lt->name;
            return CNET_MEM_OK;
        }
    }

    /* already in base/reg but not STM */
    if (unit_in_reg(&m->reg, name)) {
        (void)cnet_mem_stm_pin(m, name);
        m->n_stm_hit++;
        if (unit_out) {
            s = stm_find(m, name);
            *unit_out = s ? s->name : name;
        }
        return CNET_MEM_OK;
    }

    m->n_abstain++;
    return CNET_MEM_ABSTAIN;
}

int cnet_mem_resolve_ex(CnetMemRuntime *m, const char *name, uint32_t world_mask,
                        double residual, const char **unit_out) {
    const char *asi_name = NULL;
    int ar;

    if (unit_out) *unit_out = NULL;
    if (!m || !name || !name[0]) return CNET_MEM_ERR;
    m->n_serve++;
    m->tick++;

    /* Product wire: ASI gate before CERT path when skill is catalogued */
    if (m->asi_gate) {
        int known = 0;
        size_t i;
        for (i = 0; i < m->asi.n_skills; i++) {
            if (m->asi.skills[i].active && name_eq(m->asi.skills[i].name, name)) {
                known = 1;
                break;
            }
        }
        if (known) {
            ar = cnet_asi_resolve(&m->asi, name, world_mask, residual, &asi_name);
            if (ar == CNET_ASI_EXEC_BLOCKED || ar == CNET_ASI_CONFORMAL_ABSTAIN ||
                ar == CNET_ASI_RECALL_MISS || ar == CNET_ASI_DEFER_ABSTAIN) {
                m->n_asi_block++;
                m->n_abstain++;
                return CNET_MEM_ABSTAIN;
            }
            if (ar != CNET_ASI_OK && ar != CNET_ASI_ERR) {
                m->n_asi_block++;
                m->n_abstain++;
                return CNET_MEM_ABSTAIN;
            }
            /* CNET_ASI_ERR on empty catalog name shouldn't happen if known */
        }
    }

    return mem_resolve_body(m, name, unit_out);
}

int cnet_mem_resolve(CnetMemRuntime *m, const char *name, const char **unit_out) {
    /* Open world + unmeasured residual — back-compat with prior mem_runtime */
    return cnet_mem_resolve_ex(m, name, 0xffffffffu, -1.0, unit_out);
}

void cnet_mem_feedback(CnetMemRuntime *m, const char *name, int good) {
    cnet_mem_feedback_ex(m, name, good, 0u, -1.0);
}

void cnet_mem_feedback_ex(CnetMemRuntime *m, const char *name, int good,
                          uint32_t world_mask, double residual) {
    CnetMemSlot *s;
    if (!m || !name) return;
    s = stm_find(m, name);
    if (s) {
        s->ema_good = 0.8 * s->ema_good + 0.2 * (good ? 1.0 : 0.0);
        if (!good && s->hits > 0) s->hits--;
    }
    if (cnet_asi_episode_log(&m->asi, name, world_mask, good, residual) == 0)
        m->n_episode_log++;
}

int cnet_mem_form_submit(CnetMemRuntime *m, const char *name, const char *capsule_dir,
                         double score, double incumbent) {
    size_t i;
    if (!m || !name || !name[0]) return -1;
    if (incumbent > 0.0 && !(score > incumbent)) {
        m->n_form_reject++;
        return -2;
    }
    for (i = 0; i < m->form_n; i++) {
        if (m->form[i].active && name_eq(m->form[i].name, name)) {
            m->form[i].score = score;
            m->form[i].incumbent = incumbent;
            m->form[i].pending = 1;
            m->form[i].promoted = 0;
            m->form[i].rejected = 0;
            if (capsule_dir && capsule_dir[0])
                snprintf(m->form[i].capsule_dir, sizeof m->form[i].capsule_dir, "%s",
                         capsule_dir);
            m->n_form_submit++;
            return 0;
        }
    }
    if (m->form_n >= CNET_MEM_FORM_MAX) return -1;
    memset(&m->form[m->form_n], 0, sizeof m->form[0]);
    snprintf(m->form[m->form_n].name, sizeof m->form[0].name, "%s", name);
    if (capsule_dir && capsule_dir[0])
        snprintf(m->form[m->form_n].capsule_dir, sizeof m->form[0].capsule_dir, "%s",
                 capsule_dir);
    m->form[m->form_n].score = score;
    m->form[m->form_n].incumbent = incumbent;
    m->form[m->form_n].active = 1;
    m->form[m->form_n].pending = 1;
    m->form_n++;
    m->n_form_submit++;
    return 0;
}

int cnet_mem_form_tick(CnetMemRuntime *m, int pin_on_promote) {
    size_t i;
    int n_prom = 0;
    if (!m) return -1;
    m->n_async_ticks++;
    for (i = 0; i < m->form_n; i++) {
        CnetMemFormItem *f = &m->form[i];
        CnetCapsuleReport rep;
        if (!f->active || !f->pending || f->promoted) continue;
        if (f->incumbent > 0.0 && !(f->score > f->incumbent)) {
            f->pending = 0;
            f->rejected = 1;
            m->n_form_reject++;
            continue;
        }
        /* Continual regression → refuse promote (fail-closed) */
        if (skill_has_regression(m, f->name)) {
            f->pending = 0;
            f->rejected = 1;
            m->n_form_reject++;
            continue;
        }
        if (!f->capsule_dir[0]) continue;
        if (cnb_has_unit(&m->base, f->name)) {
            if (rebuild_reg(m) == 0 && unit_in_reg(&m->reg, f->name)) {
                f->pending = 0;
                f->promoted = 1;
                (void)cnet_mem_ltm_add(m, f->name, f->capsule_dir);
                if (pin_on_promote) (void)cnet_mem_stm_pin(m, f->name);
                n_prom++;
                m->n_form_promote++;
            }
            continue;
        }
        memset(&rep, 0, sizeof rep);
        if (cnet_capsule_import(&m->base, &m->cov, f->capsule_dir, &rep) != 0) {
            f->pending = 0;
            f->rejected = 1;
            m->n_form_reject++;
            continue;
        }
        if (rebuild_reg(m) != 0 || !unit_in_reg(&m->reg, f->name)) {
            f->pending = 0;
            f->rejected = 1;
            m->n_form_reject++;
            continue;
        }
        (void)cnet_mem_ltm_add(m, f->name, f->capsule_dir);
        if (pin_on_promote) (void)cnet_mem_stm_pin(m, f->name);
        f->pending = 0;
        f->promoted = 1;
        n_prom++;
        m->n_form_promote++;
    }
    return n_prom;
}

double cnet_mem_stm_hit_rate(const CnetMemRuntime *m) {
    if (!m || m->n_serve == 0) return 0.0;
    return (double)m->n_stm_hit / (double)m->n_serve;
}

int cnet_mem_continual_regressions(const CnetMemRuntime *m) {
    if (!m) return 0;
    return cnet_asi_continual_regressions(&m->asi);
}

void cnet_mem_dump_stats(const CnetMemRuntime *m, char *buf, size_t cap) {
    if (!m || !buf || !cap) return;
    snprintf(buf, cap,
             "serve=%llu stm_hit=%llu ltm_fetch=%llu abstain=%llu "
             "form_sub=%llu form_prom=%llu form_rej=%llu async_ticks=%llu "
             "asi_block=%llu ep_log=%llu "
             "stm_n=%zu ltm_n=%zu form_n=%zu hit_rate=%.3f",
             (unsigned long long)m->n_serve, (unsigned long long)m->n_stm_hit,
             (unsigned long long)m->n_ltm_fetch, (unsigned long long)m->n_abstain,
             (unsigned long long)m->n_form_submit, (unsigned long long)m->n_form_promote,
             (unsigned long long)m->n_form_reject, (unsigned long long)m->n_async_ticks,
             (unsigned long long)m->n_asi_block, (unsigned long long)m->n_episode_log,
             m->stm_n, m->ltm_n, m->form_n, cnet_mem_stm_hit_rate(m));
}
