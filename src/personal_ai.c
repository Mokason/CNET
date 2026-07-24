#include "../include/personal_ai.h"
#include "../include/self_improve.h"
#include "../include/acquire.h"
#include "../include/residual_gguf.h"
#include "../include/cnet_curiosity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Dense name table for PersonalAiSource (enum values are 0..5). */
static const char *const k_source_names[] = {
    "local",    /* PERSONAL_AI_LOCAL */
    "teacher",  /* PERSONAL_AI_TEACHER */
    "abstain",  /* PERSONAL_AI_ABSTAIN */
    "error",    /* PERSONAL_AI_ERROR */
    "soft",     /* PERSONAL_AI_SOFT */
    "residual"  /* PERSONAL_AI_RESIDUAL */
};

void personal_ai_policy_defaults(PersonalAiPolicy *p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->teach_inline = 0;
    p->allow_teacher = 1;
    p->max_inline_teaches = 0;
    p->allow_soft = 1;
    p->allow_residual = 1;
    p->allow_medium = 1;
    p->structure_mine_on_serve = 0;
    p->structure_min_hits = 3;
}

const char *personal_ai_source_name(PersonalAiSource s) {
    unsigned u = (unsigned)s;
    if (u < sizeof k_source_names / sizeof k_source_names[0])
        return k_source_names[u];
    return "error";
}

/* One getenv per key (old PAI_SET called getenv twice). */
static int pai_setenv_default(const char *k, const char *v) {
    const char *cur = getenv(k);
    if (cur && cur[0]) return 0;
#if defined(_WIN32)
    {
        char b[128];
        snprintf(b, sizeof b, "%s=%s", k, v);
        return _putenv(b) == 0 ? 1 : 0;
    }
#else
    return setenv(k, v, 0) == 0 ? 1 : 0;
#endif
}

int personal_ai_apply_env(void) {
    static const char *const kv[][2] = {
        {"CNET_PERSONAL_ALLOW_TEACHER", "1"},
        {"CNET_PERSONAL_TEACH_INLINE", "0"},
        {"CNET_PERSONAL_ALLOW_SOFT", "1"},
        {"CNET_PERSONAL_ALLOW_RESIDUAL", "1"},
        {"CNET_TEACHER_IDLE_SEC", "300"},
        {"CNET_LANE_MAX_CLOSURES", "4"},
    };
    int n = self_improve_apply_deploy_env();
    size_t i;
    for (i = 0; i < sizeof kv / sizeof kv[0]; i++)
        n += pai_setenv_default(kv[i][0], kv[i][1]);
    /* Real residual GGUF is operator-set (CNET_RESIDUAL_GGUF); not defaulted. */
    return n;
}

/* Env parsers — avoid repeated if (e && e[0]) ladders. */
static int env_truthy_on(const char *e) {
    return e && e[0] == '1' && e[1] == '\0';
}
static int env_truthy_off(const char *e) {
    /* default-on flags: only explicit "0" disables */
    return !(e && e[0] == '0' && e[1] == '\0');
}
static int env_size_at_least(const char *e, long min_v, size_t *out) {
    long v;
    if (!e || !e[0] || !out) return 0;
    v = atol(e);
    if (v < min_v) return 0;
    *out = (size_t)v;
    return 1;
}

static void policy_from_env(PersonalAiPolicy *p) {
    size_t v;
    if (!p) return;
    {
        const char *e = getenv("CNET_PERSONAL_ALLOW_TEACHER");
        if (e && e[0]) p->allow_teacher = env_truthy_off(e);
    }
    {
        const char *e = getenv("CNET_PERSONAL_TEACH_INLINE");
        if (e && e[0]) p->teach_inline = env_truthy_on(e);
    }
    if (env_size_at_least(getenv("CNET_PERSONAL_MAX_INLINE_TEACHES"), 0, &v))
        p->max_inline_teaches = v;
    {
        const char *e = getenv("CNET_PERSONAL_ALLOW_SOFT");
        if (e && e[0]) p->allow_soft = env_truthy_off(e);
    }
    {
        const char *e = getenv("CNET_PERSONAL_ALLOW_RESIDUAL");
        if (e && e[0]) p->allow_residual = env_truthy_off(e);
    }
    if (env_size_at_least(getenv("CNET_PERSONAL_STRUCTURE_MIN_HITS"), 1, &v))
        p->structure_min_hits = v;
    if (env_truthy_on(getenv("CNET_PERSONAL_STRUCTURE_MINE_ON_SERVE")))
        p->structure_mine_on_serve = 1;
}

/* Collapse repeated serve bookkeeping (source/trust/tier + counters). */
static void serve_record_hit(PersonalAi *ai, PersonalAiReport *rep,
                             PersonalAiSource src, HybridTrust trust,
                             HybridTier tier) {
    rep->source = src;
    rep->trust = trust;
    rep->tier = tier;
    switch (src) {
    case PERSONAL_AI_LOCAL:
        rep->local_hits = 1;
        ai->totals.local_hits++;
        ai->hybrid.tier_a_hits++;
        ai->hybrid.prefer_warm_hits++;
        break;
    case PERSONAL_AI_SOFT:
        rep->soft_hits = 1;
        ai->totals.soft_hits++;
        ai->hybrid.prefer_warm_hits++;
        break;
    case PERSONAL_AI_RESIDUAL:
        rep->residual_hits = 1;
        ai->totals.residual_hits++;
        break;
    case PERSONAL_AI_TEACHER:
        rep->teacher_helps = 1;
        ai->totals.teacher_helps++;
        break;
    default:
        break;
    }
}

int personal_ai_open(PersonalAi *ai, const char *base_path,
                     const char *ledger_path, const char *inbox_path,
                     const PersonalAiPolicy *policy) {
    CnetGovPolicy gp;
    if (!ai || !base_path || !ledger_path) return -1;
    memset(ai, 0, sizeof *ai);
    personal_ai_apply_env();
    if (policy) ai->policy = *policy;
    else personal_ai_policy_defaults(&ai->policy);
    policy_from_env(&ai->policy);

    if (gap_lane_open(&ai->lane, base_path, ledger_path, inbox_path) != 0)
        return -2;

    /* Compute orchestrator: eco|balanced|turbo sparse/page/MTP knobs +
       duty-cycle (CNET_GOV_PROFILE). Apply env before any heavy load. */
    cnet_gov_policy_deploy_defaults(&gp);
    (void)cnet_gov_policy_from_env(&gp);
    if (cnet_gov_open(&ai->gov, &gp) != 0) {
        gap_lane_close(&ai->lane);
        return -3;
    }
    (void)cnet_gov_apply_compute_env(&ai->gov);
    if (gp.max_closures_per_drain)
        ai->lane.acq.max_closures_per_drain = gp.max_closures_per_drain;
    {
        const char *me = getenv("CNET_ACQ_MIN_EVIDENCE");
        if (me && me[0]) {
            long v = atol(me);
            if (v >= 1) ai->lane.acq.min_evidence = (size_t)v;
        }
    }

    hybrid_ai_init(&ai->hybrid);
    ai->owned_residual = NULL;
    ai->loaded = 1;
    /* Auto-bind real Tier C residual when operator set CNET_RESIDUAL_GGUF. */
    {
        ResidualGguf *r = NULL;
        int ar = personal_ai_auto_residual_gguf(ai, &r);
        if (ar == 0 && r) {
            ai->owned_residual = r;
        } else if (ar < 0) {
            const char *want = getenv("CNET_RESIDUAL_GGUF");
            if (want && want[0]) {
                /* Fail-closed: residual was requested but could not load. */
                personal_ai_close(ai);
                return -4;
            }
        }
    }
    return 0;
}

int personal_ai_bind_teacher(PersonalAi *ai, const char *name,
                             Port input_port, Port goal_port,
                             CnetOracleFn fn, void *ctx) {
    if (!ai || !ai->loaded || !name || !fn) return -1;
    if (!ai->policy.allow_teacher) return -2;
    return acquire_oracle_register(&ai->lane.oracles, name, input_port,
                                   goal_port, fn, ctx);
}

int personal_ai_bind_residual(PersonalAi *ai, const char *name,
                              CnetOracleFn fn, void *ctx) {
    if (!ai || !ai->loaded) return -1;
    if (!ai->policy.allow_residual) return -2;
    return hybrid_bind_residual(&ai->hybrid, name, fn, ctx);
}

int personal_ai_bind_soft(PersonalAi *ai, const char *name, Port in, Port out,
                          CnetOracleFn fn, void *ctx, double min_margin) {
    if (!ai || !ai->loaded) return -1;
    if (!ai->policy.allow_soft) return -2;
    return hybrid_bind_soft(&ai->hybrid, name, in, out, fn, ctx, min_margin);
}

int personal_ai_bind_medium(PersonalAi *ai, const char *name, Port in, Port out,
                            CnetOracleFn fn, void *ctx,
                            uint64_t resident_bytes) {
    if (!ai || !ai->loaded) return -1;
    if (!ai->policy.allow_medium) return -2;
    return hybrid_bind_medium(&ai->hybrid, &ai->gov, name, in, out, fn, ctx,
                              resident_bytes);
}

int personal_ai_enable_adapter(PersonalAi *ai, const double *bias, size_t dim,
                               double scale) {
    if (!ai || !ai->loaded) return -1;
    return hybrid_adapter_enable(&ai->hybrid, bias, dim, scale);
}

int personal_ai_serve(PersonalAi *ai, Port input_port, Port goal_port,
                      const double *input, size_t in_len, double *output,
                      size_t out_cap, PersonalAiReport *rep) {
    PersonalAiReport scratch;
    RoutePlan plan;
    int rc;

    if (!rep) {
        memset(&scratch, 0, sizeof scratch);
        rep = &scratch;
    } else {
        memset(rep, 0, sizeof *rep);
    }
    rep->trust = HYBRID_TRUST_UNCERTIFIED;
    rep->tier = HYBRID_TIER_C;

    if (!ai || !ai->loaded || !input || !output) {
        rep->source = PERSONAL_AI_ERROR;
        return -1;
    }

    /* ---- Tier A: certified plan ---- */
    memset(&plan, 0, sizeof plan);
    if (route_plan(&ai->lane.reg, input_port, goal_port, &plan) == 0 &&
        plan.length > 0) {
        if (route_execute(&plan, input, in_len, output, out_cap) != 0) {
            rep->source = PERSONAL_AI_ERROR;
            ai->totals.abstains++;
            return -1;
        }
        serve_record_hit(ai, rep, PERSONAL_AI_LOCAL, HYBRID_TRUST_CERTIFIED,
                         HYBRID_TIER_A);
        return 0;
    }

    /* ---- Tier B: medium modules then soft specialists ---- */
    if (ai->policy.allow_medium &&
        hybrid_try_medium(&ai->hybrid, input_port, goal_port, input, in_len,
                          output, out_cap) == 0) {
        serve_record_hit(ai, rep, PERSONAL_AI_SOFT, HYBRID_TRUST_PROVISIONAL,
                         HYBRID_TIER_B);
        return 0;
    }
    if (ai->policy.allow_soft) {
        char sn[64];
        if (hybrid_try_soft(&ai->hybrid, input_port, goal_port, input, in_len,
                            output, out_cap, sn, sizeof sn) == 0) {
            serve_record_hit(ai, rep, PERSONAL_AI_SOFT,
                             HYBRID_TRUST_PROVISIONAL, HYBRID_TIER_B);
            return 0;
        }
    }

    /* ---- Tier C: residual generative ---- */
    if (ai->policy.allow_residual && ai->hybrid.residual.bound &&
        hybrid_try_residual(&ai->hybrid, input_port, goal_port, input, in_len,
                            output, out_cap) == 0) {
        serve_record_hit(ai, rep, PERSONAL_AI_RESIDUAL,
                         HYBRID_TRUST_UNCERTIFIED, HYBRID_TIER_C);
        if (ai->policy.structure_mine_on_serve) {
            BinaryTransformNetwork *stu = NULL;
            if (personal_ai_structure_mine(ai, &stu) == 0) {
                rep->structure_mined = 1;
                (void)stu; /* registry borrows */
            }
        }
        return 0;
    }

    /* ---- Legacy teacher oracle path (signature-matched big AI) ---- */
    if (ai->policy.allow_teacher && ai->lane.oracles.count > 0) {
        rc = gap_lane_execute(&ai->lane, input_port, goal_port, input, in_len,
                              output, out_cap);
        rep->gap_noted = 1;
        if (rc == 0) {
            serve_record_hit(ai, rep, PERSONAL_AI_TEACHER,
                             HYBRID_TRUST_UNCERTIFIED, HYBRID_TIER_C);
            if (ai->policy.teach_inline &&
                (!ai->policy.max_inline_teaches ||
                 ai->inline_teaches_done < ai->policy.max_inline_teaches)) {
                AcquireReport ar;
                memset(&ar, 0, sizeof ar);
                if (acquire_now(&ai->lane.reg, &ai->lane.ledger,
                                &ai->lane.oracles, &ai->lane.acq, input_port,
                                goal_port, &ar) == 0) {
                    rep->taught = 1;
                    ai->inline_teaches_done++;
                    ai->totals.teaches++;
                    (void)gap_lane_checkpoint(&ai->lane);
                }
            }
            return 0;
        }
    } else {
        (void)acquire_note_no_plan(&ai->lane.ledger, input_port, goal_port);
        if (ai->lane.inbox_path[0])
            (void)gap_inbox_note_no_plan(ai->lane.inbox_path, input_port,
                                         goal_port);
        rep->gap_noted = 1;
    }

    rep->source = PERSONAL_AI_ABSTAIN;
    rep->trust = HYBRID_TRUST_UNCERTIFIED;
    ai->totals.abstains++;
    return -1;
}

int personal_ai_tick(PersonalAi *ai, GapLaneTickReport *tick_rep) {
    int rc;
    size_t open_gaps = 0, gi;
    if (!ai || !ai->loaded) return -1;
    cnet_gov_begin_drain(&ai->gov);
    rc = gap_lane_tick(&ai->lane, tick_rep, 0);
    /* P5: structure mine only if residual traces exist (skip empty scan). */
    if (ai->hybrid.trace_count > 0) {
        BinaryTransformNetwork *stu = NULL;
        if (hybrid_structure_mine(&ai->hybrid, &ai->lane.reg,
                                  ai->policy.structure_min_hits, &stu) == 0) {
            (void)stu;
            (void)gap_lane_checkpoint(&ai->lane);
        }
    }
    /* Curiosity: budgeted self-seeding when demand is quiet.
       Skip open-gap scan entirely when curiosity is off. */
    {
        const char *cur = getenv("CNET_CURIOSITY");
        if (cur && cur[0] == '1') {
            CnetCuriosityConfig cc;
            CnetCuriosityReport cr = {0};
            int state_path_ok = 1;
            for (gi = 0; gi < ai->lane.ledger.count; gi++)
                if (ai->lane.ledger.gaps[gi].status == GAP_OPEN) open_gaps++;
            cnet_curiosity_config_from_env(&cc);
            if (ai->lane.inbox_path[0])
                snprintf(cc.inbox_path, sizeof cc.inbox_path, "%s",
                         ai->lane.inbox_path);
            {
                const char *st = getenv("CNET_CURIOSITY_STATE");
                if ((!st || !st[0]) && ai->lane.base_path[0]) {
                    static const char suffix[] = ".curiosity";
                    size_t base_len = strlen(ai->lane.base_path);
                    if (base_len + sizeof suffix <= sizeof cc.state_path) {
                        memcpy(cc.state_path, ai->lane.base_path, base_len);
                        memcpy(cc.state_path + base_len, suffix, sizeof suffix);
                    } else {
                        state_path_ok = 0;
                        fprintf(stderr,
                                "personal_ai: curiosity state path too long — "
                                "skipping tick\n");
                    }
                }
            }
            if (state_path_ok)
                (void)cnet_curiosity_tick(&cc, &ai->lane.reg, open_gaps, &cr);
            if (cr.proposed > 0)
                fprintf(stderr,
                        "personal_ai: curiosity proposed=%zu covered_skip=%zu\n",
                        cr.proposed, cr.skipped_covered);
        }
    }
    /* Governed adapter maintenance: when installed (opt-in via
       registry_lora_install_orchestrator), teach + certify low-rank adapters
       from units' accumulated fault queues. NULL by default => no-op, so the
       tick is byte-identical unless a caller arms the hook. CCE-free call. */
    if (g_cnet_lora_tick_hook)
        g_cnet_lora_tick_hook(&ai->lane.reg);
    return rc;
}

int personal_ai_structure_mine(PersonalAi *ai,
                               BinaryTransformNetwork **student_out) {
    if (!ai || !ai->loaded) return -1;
    return hybrid_structure_mine(&ai->hybrid, &ai->lane.reg,
                                 ai->policy.structure_min_hits, student_out);
}

int personal_ai_distill_plan(PersonalAi *ai, const RoutePlan *plan,
                             BinaryTransformNetwork **chunk_out) {
    if (!ai || !ai->loaded) return -1;
    return hybrid_distill_plan(&ai->hybrid, &ai->lane.reg, plan, &ai->gov,
                               chunk_out);
}

void personal_ai_close(PersonalAi *ai) {
    if (!ai || !ai->loaded) return;
    /* Drop residual fn bind before freeing the model it points at. */
    hybrid_ai_free(&ai->hybrid);
    if (ai->owned_residual) {
        residual_gguf_close(ai->owned_residual);
        ai->owned_residual = NULL;
    }
    gap_lane_close(&ai->lane);
    cnet_gov_close(&ai->gov);
    ai->loaded = 0;
}

void personal_ai_totals(const PersonalAi *ai, PersonalAiReport *out) {
    if (!ai || !out) return;
    *out = ai->totals;
}

const HybridAi *personal_ai_hybrid(const PersonalAi *ai) {
    return ai ? &ai->hybrid : NULL;
}
