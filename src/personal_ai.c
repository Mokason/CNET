#include "../include/personal_ai.h"
#include "../include/self_improve.h"
#include "../include/acquire.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void personal_ai_policy_defaults(PersonalAiPolicy *p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->teach_inline = 0;
    p->allow_teacher = 1;
    p->max_inline_teaches = 0;
}

const char *personal_ai_source_name(PersonalAiSource s) {
    switch (s) {
    case PERSONAL_AI_LOCAL: return "local";
    case PERSONAL_AI_TEACHER: return "teacher";
    case PERSONAL_AI_ABSTAIN: return "abstain";
    default: return "error";
    }
}

int personal_ai_apply_env(void) {
    int n = 0;
    n += self_improve_apply_deploy_env();
#if defined(_WIN32)
#define PAI_SET(k, v) do { \
        if (!getenv(k) || !getenv(k)[0]) { \
            char b[128]; snprintf(b, sizeof b, "%s=%s", k, v); \
            if (_putenv(b) == 0) n++; \
        } \
    } while (0)
#else
#define PAI_SET(k, v) do { \
        if (!getenv(k) || !getenv(k)[0]) { \
            if (setenv(k, v, 0) == 0) n++; \
        } \
    } while (0)
#endif
    PAI_SET("CNET_PERSONAL_ALLOW_TEACHER", "1");
    PAI_SET("CNET_PERSONAL_TEACH_INLINE", "0");
    PAI_SET("CNET_TEACHER_IDLE_SEC", "300");
    PAI_SET("CNET_LANE_MAX_CLOSURES", "4");
#undef PAI_SET
    return n;
}

static void policy_from_env(PersonalAiPolicy *p) {
    const char *a, *t, *m;
    if (!p) return;
    a = getenv("CNET_PERSONAL_ALLOW_TEACHER");
    if (a && a[0]) p->allow_teacher = !(a[0] == '0' && a[1] == '\0');
    t = getenv("CNET_PERSONAL_TEACH_INLINE");
    if (t && t[0]) p->teach_inline = (t[0] == '1');
    m = getenv("CNET_PERSONAL_MAX_INLINE_TEACHES");
    if (m && m[0]) {
        long v = atol(m);
        if (v >= 0) p->max_inline_teaches = (size_t)v;
    }
}

int personal_ai_open(PersonalAi *ai,
                     const char *base_path,
                     const char *ledger_path,
                     const char *inbox_path,
                     const PersonalAiPolicy *policy)
{
    CnetGovPolicy gp;
    if (!ai || !base_path || !ledger_path) return -1;
    memset(ai, 0, sizeof *ai);
    personal_ai_apply_env();
    if (policy) ai->policy = *policy;
    else personal_ai_policy_defaults(&ai->policy);
    policy_from_env(&ai->policy);

    if (gap_lane_open(&ai->lane, base_path, ledger_path, inbox_path) != 0)
        return -2;

    cnet_gov_policy_deploy_defaults(&gp);
    (void)cnet_gov_policy_from_env(&gp);
    if (cnet_gov_open(&ai->gov, &gp) != 0) {
        gap_lane_close(&ai->lane);
        return -3;
    }
    if (gp.max_closures_per_drain)
        ai->lane.acq.max_closures_per_drain = gp.max_closures_per_drain;
    /* Personal/small-domain skills often have card << 16; allow override.
       Default acquire min_evidence=16 is correct for windowed LM units. */
    {
        const char *me = getenv("CNET_ACQ_MIN_EVIDENCE");
        if (me && me[0]) {
            long v = atol(me);
            if (v >= 1) ai->lane.acq.min_evidence = (size_t)v;
        }
    }

    ai->loaded = 1;
    return 0;
}

int personal_ai_bind_teacher(PersonalAi *ai,
                             const char *name,
                             Port input_port,
                             Port goal_port,
                             CnetOracleFn fn,
                             void *ctx)
{
    if (!ai || !ai->loaded || !name || !fn) return -1;
    if (!ai->policy.allow_teacher) return -2;
    return acquire_oracle_register(&ai->lane.oracles, name, input_port,
                                   goal_port, fn, ctx);
}

int personal_ai_serve(PersonalAi *ai,
                      Port input_port,
                      Port goal_port,
                      const double *input,
                      size_t in_len,
                      double *output,
                      size_t out_cap,
                      PersonalAiReport *rep)
{
    PersonalAiReport scratch;
    RoutePlan plan;
    size_t units_before;
    int rc;

    if (!rep) {
        memset(&scratch, 0, sizeof scratch);
        rep = &scratch;
    } else {
        memset(rep, 0, sizeof *rep);
    }

    if (!ai || !ai->loaded || !input || !output) {
        rep->source = PERSONAL_AI_ERROR;
        return -1;
    }

    units_before = ai->lane.reg.count;

    /* 1) Local certified library first. */
    memset(&plan, 0, sizeof plan);
    if (route_plan(&ai->lane.reg, input_port, goal_port, &plan) == 0 &&
        plan.length > 0) {
        if (route_execute(&plan, input, in_len, output, out_cap) != 0) {
            rep->source = PERSONAL_AI_ERROR;
            ai->totals.abstains++;
            return -1;
        }
        rep->source = PERSONAL_AI_LOCAL;
        rep->local_hits = 1;
        ai->totals.local_hits++;
        return 0;
    }

    /* 2) No plan: local-only mode still notes the gap for later. */
    if (!ai->policy.allow_teacher || ai->lane.oracles.count == 0) {
        (void)acquire_note_no_plan(&ai->lane.ledger, input_port, goal_port);
        if (ai->lane.inbox_path[0])
            (void)gap_inbox_note_no_plan(ai->lane.inbox_path, input_port,
                                         goal_port);
        rep->source = PERSONAL_AI_ABSTAIN;
        rep->gap_noted = 1;
        ai->totals.abstains++;
        return -1;
    }

    /* 3) Big AI on call: plan-or-fallback (notes gap + teacher answer). */
    rc = gap_lane_execute(&ai->lane, input_port, goal_port, input, in_len,
                          output, out_cap);
    rep->gap_noted = 1;

    if (rc != 0) {
        rep->source = PERSONAL_AI_ABSTAIN;
        ai->totals.abstains++;
        return -1;
    }

    /* If a plan appeared (unlikely) treat as local; else teacher help. */
    memset(&plan, 0, sizeof plan);
    if (route_plan(&ai->lane.reg, input_port, goal_port, &plan) == 0 &&
        plan.length > 0 && ai->lane.reg.count == units_before) {
        rep->source = PERSONAL_AI_LOCAL;
        rep->local_hits = 1;
        ai->totals.local_hits++;
        return 0;
    }

    rep->source = PERSONAL_AI_TEACHER;
    rep->teacher_helps = 1;
    ai->totals.teacher_helps++;

    /* 4) Optional inline teach: help becomes a local skill now. */
    if (ai->policy.teach_inline) {
        int can = 1;
        if (ai->policy.max_inline_teaches &&
            ai->inline_teaches_done >= ai->policy.max_inline_teaches)
            can = 0;
        if (can) {
            AcquireReport ar;
            memset(&ar, 0, sizeof ar);
            if (acquire_now(&ai->lane.reg, &ai->lane.ledger, &ai->lane.oracles,
                            &ai->lane.acq, input_port, goal_port, &ar) == 0) {
                rep->taught = 1;
                ai->inline_teaches_done++;
                ai->totals.teaches++;
                (void)gap_lane_checkpoint(&ai->lane);
            }
        }
    }
    return 0;
}

int personal_ai_tick(PersonalAi *ai, GapLaneTickReport *tick_rep) {
    if (!ai || !ai->loaded) return -1;
    cnet_gov_begin_drain(&ai->gov);
    return gap_lane_tick(&ai->lane, tick_rep, 0);
}

void personal_ai_close(PersonalAi *ai) {
    if (!ai || !ai->loaded) return;
    gap_lane_close(&ai->lane);
    cnet_gov_close(&ai->gov);
    ai->loaded = 0;
}

void personal_ai_totals(const PersonalAi *ai, PersonalAiReport *out) {
    if (!ai || !out) return;
    *out = ai->totals;
}
