#include "../include/personal_ai.h"
#include "../include/self_improve.h"
#include "../include/acquire.h"
#include "../include/residual_gguf.h"
#include "../include/residual_http.h"
#include "../include/cnet_curiosity.h"
#include "../include/cnet_moe.h"
#include "../include/cnet_acct.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Organic Tier-C capture — dual-write into the unified fault bus (cnet_fault.c).
 * Weak so binaries that don't link the fault TU stay unchanged. */
void cnet_fault_mirror_kind(const char *unit, const double *input,
                            const double *target, int in_dim, int out_dim,
                            const char *source_name, const char *label_kind,
                            const char *note) __attribute__((weak));

/* Tier-C residual answers are the only labelled pairs CNET may learn from: the
 * label comes from an external model, never from CNET's own certified output.
 * Rows are stamped source=surprise/label_kind=residual so a consumer can tell
 * organic capture from a synthetic seeder by provenance alone. No-op unless
 * CNET_FAULT_LOG is set (mirror enforces that) and capture is not disabled.
 *
 * Returns 1 when a pair was offered to the bus, 0 when policy/env/shape ruled
 * it out. The bus may still dedup an offered pair, so the row count in the log
 * — not this counter — is the source of truth for organic intake volume. */
static int residual_capture(Port input_port, Port goal_port,
                            const double *input, size_t in_len,
                            const double *output, size_t out_cap) {
    char unit[96];
    size_t out_dim = goal_port.field_width * goal_port.field_count;
    const char *en, *nm, *fl;
    if (!cnet_fault_mirror_kind) return 0;
    en = getenv("CNET_RESIDUAL_CAPTURE");
    if (en && en[0] == '0' && en[1] == '\0') return 0; /* explicit off */
    fl = getenv("CNET_FAULT_LOG");
    if (!fl || !fl[0]) return 0; /* no bus addressed */
    if (!input || !output || in_len == 0) return 0;
    if (out_dim == 0 || out_dim > out_cap) out_dim = out_cap;
    if (out_dim == 0) return 0;
    if (in_len > (size_t)INT_MAX || out_dim > (size_t)INT_MAX) return 0;
    (void)input_port;
    nm = getenv("CNET_RESIDUAL_CAPTURE_UNIT");
    if (nm && nm[0])
        snprintf(unit, sizeof unit, "%s", nm);
    else
        snprintf(unit, sizeof unit, "res_%zux%zu", in_len, out_dim);
    cnet_fault_mirror_kind(unit, input, output, (int)in_len, (int)out_dim,
                           "surprise", "residual", "residual_serve");
    return 1;
}

/* S7: coverage travels beside the base, like the gap ledger and curiosity
   state. Returns 0 and fills out on success. */
static int coverage_path(const PersonalAi *ai, char *out, size_t cap) {
    static const char suffix[] = ".coverage";
    size_t n;
    if (!ai || !out || !ai->lane.base_path[0]) return -1;
    n = strlen(ai->lane.base_path);
    if (n + sizeof suffix > cap) return -1;
    memcpy(out, ai->lane.base_path, n);
    memcpy(out + n, suffix, sizeof suffix);
    return 0;
}

/* Persist after a successful mine so a restart cannot silently drop the gate.
   Best-effort: a write failure must not fail the serve that triggered it, but
   it is reported because the protection is weaker until the next mine. */
static void coverage_persist(const PersonalAi *ai) {
    char path[512];
    if (coverage_path(ai, path, sizeof path) != 0) return;
    if (hybrid_coverage_save(&ai->hybrid, path) != 0)
        fprintf(stderr, "personal_ai: could not write coverage to %s\n", path);
}

/* A2/B2 startup self-check. A base can hold mined units while the coverage
   sidecar is missing, truncated or stale — deleted by a janitor, lost to a
   crash, or restored from an older snapshot. Default-allow would then serve
   those units unguarded, which is exactly the confident-wrong failure the gate
   exists to stop, and it would do it silently.
   Returns the number of mined units in the base that have no coverage record;
   nonzero arms fail-closed for them and logs an ERROR. */
static size_t coverage_selfcheck(PersonalAi *ai, int load_failed) {
    size_t i, mined = 0, unguarded = 0, stale = 0;
    /* Drop records for units the base no longer holds. A sidecar outliving its
       base (base deleted or rolled back, sidecar left behind) would otherwise
       claim a shape as already-mined forever and block it from ever being
       learned again. Nothing is mined yet at open, so every live record must
       correspond to a base unit. */
    for (i = 0; i < ai->hybrid.coverage_count; i++) {
        const char *nm = ai->hybrid.coverage[i].unit;
        if (!ai->hybrid.coverage[i].active || !nm[0]) continue;
        if (cnb_has_unit(&ai->lane.base, nm)) continue;
        if (hybrid_coverage_forget_unit(&ai->hybrid, nm)) stale++;
    }
    if (stale > 0)
        fprintf(stderr,
                "personal_ai: dropped %zu stale coverage record(s) with no unit "
                "in the base\n", stale);
    for (i = 0; i < ai->lane.base.unit_count; i++) {
        const char *nm = ai->lane.base.units[i].name;
        BinaryTransformNetwork btn;
        Contract ct;
        int bound = 0;
        if (!hybrid_unit_is_mined(nm)) continue;
        mined++;
        /* Bind the EXACT relation, not the name. `hybrid_coverage_has_unit`
           answers "is this name mentioned anywhere", so a stale or corrupt
           record that merely carried the name suppressed this arm while
           binding no ports and no dimension. Materializing the unit is the
           only way to learn the ports it will actually serve on; this runs
           once per mined unit at open. */
        memset(&btn, 0, sizeof btn);
        memset(&ct, 0, sizeof ct);
        if (cnb_get_unit(&ai->lane.base, nm, &btn, &ct) == 0) {
            if (btn.input_port_count >= 1 && btn.output_port_count >= 1)
                bound = hybrid_coverage_binds_unit(&ai->hybrid, nm,
                                                   btn.input_ports[0],
                                                   btn.output_ports[0],
                                                   btn.input_count);
            contract_free(&ct);
            btn_free(&btn);
        }
        if (!bound) unguarded++;
    }
    if (unguarded > 0 || load_failed) {
        hybrid_coverage_arm_fail_closed(&ai->hybrid, 1);
        fprintf(stderr,
                "personal_ai: ERROR coverage does not bind %zu of %zu mined "
                "unit(s)%s — refusing to serve them until a re-mine restores "
                "the guard (fail-closed)\n",
                unguarded, mined,
                load_failed ? " (coverage sidecar rejected)" : "");
    }
    return unguarded;
}

/* S6: may Tier A claim this input? Off only by explicit operator override —
   the default is safe, because the failure it prevents (a confident wrong
   answer replacing a correct teacher one) is invisible to the residual_rate
   KPI, which scores it as a win. */
static int coverage_gate_disabled(void) {
    const char *e = getenv("CNET_COVERAGE_ABSTAIN");
    return e && e[0] == '0' && e[1] == '\0';
}

static int coverage_gate_open(const PersonalAi *ai, Port input_port,
                              Port goal_port, const double *input,
                              size_t in_len) {
    if (coverage_gate_disabled()) return 1;
    return hybrid_coverage_admits(&ai->hybrid, input_port, goal_port, input,
                                  in_len);
}

/* Plan-level check: the shape lookup above only sees the request's ports, so a
   mined unit whose guard was lost still needs refusing by name. */
static int plan_coverage_open(const PersonalAi *ai, const RoutePlan *plan,
                              const double *input, size_t in_len) {
    size_t i;
    if (coverage_gate_disabled()) return 1;
    if (!ai->hybrid.coverage_fail_closed) return 1;
    for (i = 0; i < plan->length; i++) {
        const char *nm = plan->names[i];
        if (!hybrid_unit_is_mined(nm)) continue;
        if (!hybrid_coverage_admits_unit(&ai->hybrid, nm, input, in_len))
            return 0;
    }
    return 1;
}

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
    /* Rehydrate certified coverage before anything can serve: a mined unit
       reloaded from the base with no coverage record would default-allow, and
       the confident-wrong answers S6 blocks would come straight back. */
    {
        char path[512];
        int load_failed = 0;
        if (coverage_path(ai, path, sizeof path) == 0)
            load_failed = hybrid_coverage_load(&ai->hybrid, path) < 0;
        (void)coverage_selfcheck(ai, load_failed);
    }
    ai->owned_residual = NULL;
    ai->owned_residual_http = NULL;
    ai->loaded = 1;
    /* B1: the gate is default-on and turning it off is break-glass. Say so at
       startup rather than let a stale env var silently reinstate the failure. */
    if (coverage_gate_disabled())
        fprintf(stderr, "personal_ai: WARNING CNET_COVERAGE_ABSTAIN=0 — mined "
                        "units may answer outside their certified domain\n");
    /* Prefer HTTP residual (Bonsai Q1 etc.) over in-process GGUF. */
    {
        ResidualHttp *rh = NULL;
        int arh = personal_ai_auto_residual_http(ai, &rh);
        if (arh == 0 && rh) {
            ai->owned_residual_http = rh;
        } else if (arh < 0) {
            const char *want = getenv("CNET_RESIDUAL_HTTP");
            if (want && want[0]) {
                personal_ai_close(ai);
                return -4;
            }
        }
    }
    /* Auto-bind real Tier C residual when operator set CNET_RESIDUAL_GGUF. */
    if (!ai->hybrid.residual.bound) {
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
    /* B4: mine-on-serve only fires inside the Tier-C branch, so with no
       residual bound and no teacher it can never run. Silently doing nothing
       reads as "learning is on" in every dashboard — say it plainly instead. */
    if (ai->policy.structure_mine_on_serve && !ai->hybrid.residual.bound &&
        !ai->policy.allow_teacher)
        fprintf(stderr, "personal_ai: ERROR STRUCTURE_MINE_ON_SERVE=1 with no "
                        "residual bound and no teacher — nothing can be mined; "
                        "check CNET_RESIDUAL_HTTP/CNET_RESIDUAL_GGUF\n");

    /* Auto-install LoRA orchestrator when enabled (default ON if store/fault set). */
    {
        const char *ao = getenv("CNET_LORA_AUTO_ORCH");
        int want = 0;
        if (ao && ao[0])
            want = !(ao[0] == '0' && ao[1] == '\0');
        else if (getenv("CNET_FAULT_LOG") || getenv("CNET_LORA_STORE_DIR"))
            want = 1;
        if (want) {
            /* Weak: binaries without registry_lora still link. */
            extern void registry_lora_install_orchestrator(PrimitiveRegistry *reg,
                                                          const void *opt)
                __attribute__((weak));
            if (registry_lora_install_orchestrator)
                registry_lora_install_orchestrator(&ai->lane.reg, NULL);
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
        cnet_acct_add_error();
        return -1;
    }

    /* ---- MoE hard expert: goal tag names a certified unit ---- */
    {
        CnetMoeHit mh;
        int mr = cnet_moe_try_hard(&ai->lane.reg, input_port, goal_port, input,
                                   in_len, output, out_cap, &mh);
        /* This branch dispatches on goal_port.tag naming a unit, so it reaches
           certified weights without passing the Tier-A plan below. Same
           coverage rule applies — otherwise it is a second door into the exact
           behaviour the gate exists to stop. Default-allow keeps every
           non-mined shape unaffected. */
        if (getenv("CNET_MOE_DEBUG"))
            fprintf(stderr, "[moe] mr=%d hit=%d unit=%s tag=%s\n", mr, mh.hit,
                    mh.unit, goal_port.tag);
        if (mr == 0 && mh.hit && !coverage_gate_disabled() &&
            !hybrid_coverage_admits_unit(&ai->hybrid, mh.unit, input, in_len)) {
            ai->hybrid.coverage_abstains++;
            ai->totals.coverage_abstains++;
            rep->coverage_abstains = 1;
            cnet_acct_add_abstain();
            mh.hit = 0;
        }
        if (mr == 0 && mh.hit) {
            serve_record_hit(ai, rep, PERSONAL_AI_LOCAL, HYBRID_TRUST_CERTIFIED,
                             HYBRID_TIER_A);
            cnet_acct_add_hard((uint64_t)mh.steps);
            return 0;
        }
    }

    /* ---- Tier A: certified plan ----
       Gated by certified coverage: a mined unit answers inputs outside its
       contract's domain confidently and wrongly (measured — margin is 1.0 on
       exactly those), which would displace a correct teacher answer with a
       wrong own one. Outside coverage we decline Tier A and fall through to
       B/C rather than claim certified authority we did not earn. */
    memset(&plan, 0, sizeof plan);
    if (route_plan(&ai->lane.reg, input_port, goal_port, &plan) == 0 &&
        plan.length > 0) {
        if (!coverage_gate_open(ai, input_port, goal_port, input, in_len) ||
            !plan_coverage_open(ai, &plan, input, in_len)) {
            ai->hybrid.coverage_abstains++;
            ai->totals.coverage_abstains++;
            rep->coverage_abstains = 1;
            cnet_acct_add_abstain();
        } else {
            if (route_execute(&plan, input, in_len, output, out_cap) != 0) {
                rep->source = PERSONAL_AI_ERROR;
                ai->totals.abstains++;
                cnet_acct_add_error();
                return -1;
            }
            serve_record_hit(ai, rep, PERSONAL_AI_LOCAL, HYBRID_TRUST_CERTIFIED,
                             HYBRID_TIER_A);
            cnet_acct_add_tier_a((uint64_t)plan.length);
            return 0;
        }
    }

    /* ---- Tier B: medium modules then soft specialists ---- */
    if (ai->policy.allow_medium &&
        hybrid_try_medium(&ai->hybrid, input_port, goal_port, input, in_len,
                          output, out_cap) == 0) {
        serve_record_hit(ai, rep, PERSONAL_AI_SOFT, HYBRID_TRUST_PROVISIONAL,
                         HYBRID_TIER_B);
        cnet_acct_add_tier_b();
        return 0;
    }
    if (ai->policy.allow_soft) {
        char sn[64];
        if (hybrid_try_soft(&ai->hybrid, input_port, goal_port, input, in_len,
                            output, out_cap, sn, sizeof sn) == 0) {
            serve_record_hit(ai, rep, PERSONAL_AI_SOFT,
                             HYBRID_TRUST_PROVISIONAL, HYBRID_TIER_B);
            cnet_acct_add_tier_b();
            return 0;
        }
    }

    /* ---- Tier C: residual generative (only after A/B miss) ---- */
    if (ai->policy.allow_residual && ai->hybrid.residual.bound &&
        hybrid_try_residual(&ai->hybrid, input_port, goal_port, input, in_len,
                            output, out_cap) == 0) {
        /* Margin gate: refuse low-confidence residual when configured */
        {
            const char *rm = getenv("CNET_RESIDUAL_MIN_MARGIN");
            int accept = 1;
            if (rm && rm[0]) {
                double need = atof(rm);
                size_t od = goal_port.field_width * goal_port.field_count;
                if (od == 0) od = out_cap;
                if (od > out_cap) od = out_cap;
                if (need > 0.0 && od >= 2) {
                    double t1 = -1e300, t2 = -1e300;
                    size_t j;
                    for (j = 0; j < od; j++) {
                        double x = output[j];
                        if (x > t1) {
                            t2 = t1;
                            t1 = x;
                        } else if (x > t2) {
                            t2 = x;
                        }
                    }
                    if ((t1 - t2) < need) accept = 0;
                }
            }
            if (!accept) {
                /* fall through to teacher/gap */
            } else {
                hybrid_adapter_apply(&ai->hybrid, output, out_cap);
                serve_record_hit(ai, rep, PERSONAL_AI_RESIDUAL,
                                 HYBRID_TRUST_UNCERTIFIED, HYBRID_TIER_C);
                cnet_acct_add_tier_c();
                /* Organic intake: the teacher just answered a miss — keep the
                   (input, answer) pair so the PEFT learner has real traffic to
                   train on instead of only seeded rows. */
                if (residual_capture(input_port, goal_port, input, in_len,
                                     output, out_cap)) {
                    rep->residual_captures = 1;
                    ai->totals.residual_captures++;
                }
                if (ai->policy.structure_mine_on_serve) {
                    BinaryTransformNetwork *stu = NULL;
                    if (personal_ai_structure_mine(ai, &stu) == 0) {
                        rep->structure_mined = 1;
                        (void)stu;
                    }
                }
                return 0;
            }
        }
    }

    /* ---- Legacy teacher oracle path (signature-matched big AI) ---- */
    if (ai->policy.allow_teacher && ai->lane.oracles.count > 0) {
        rc = gap_lane_execute(&ai->lane, input_port, goal_port, input, in_len,
                              output, out_cap);
        rep->gap_noted = 1;
        cnet_acct_add_gap();
        if (rc == 0) {
            serve_record_hit(ai, rep, PERSONAL_AI_TEACHER,
                             HYBRID_TRUST_UNCERTIFIED, HYBRID_TIER_C);
            cnet_acct_add_teacher();
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
        cnet_acct_add_gap();
    }

    rep->source = PERSONAL_AI_ABSTAIN;
    rep->trust = HYBRID_TRUST_UNCERTIFIED;
    ai->totals.abstains++;
    cnet_acct_add_abstain();
    return -1;
}

int personal_ai_tick(PersonalAi *ai, GapLaneTickReport *tick_rep) {
    int rc;
    size_t open_gaps = 0, gi;
    if (!ai || !ai->loaded) return -1;
    cnet_gov_begin_drain(&ai->gov);
    /* Cheap low-rank adapters FIRST, ahead of the dense heal: when installed,
       teach + certify adapters from units' fault queues. registry_lora_tick
       marks a certified unit non-RESET, so gap_lane's PRIM_RESET-gated heal
       below skips it — the adapter is the retrainer, dense heal the fallback for
       units the adapter can't certify. NULL by default => no-op. CCE-free. */
    if (g_cnet_lora_tick_hook)
        g_cnet_lora_tick_hook(&ai->lane.reg);
    rc = gap_lane_tick(&ai->lane, tick_rep, 0);
    /* P5: structure mine only if residual traces exist (skip empty scan). */
    if (ai->hybrid.trace_count > 0) {
        BinaryTransformNetwork *stu = NULL;
        /* Via the wrapper so the tick path persists coverage too. */
        if (personal_ai_structure_mine(ai, &stu) == 0) {
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
    return rc;
}

int personal_ai_structure_mine(PersonalAi *ai,
                               BinaryTransformNetwork **student_out) {
    int rc;
    if (!ai || !ai->loaded) return -1;
    rc = hybrid_structure_mine(&ai->hybrid, &ai->lane.reg,
                               ai->policy.structure_min_hits, student_out);
    if (rc == 0) {
        /* S8: a unit that only reaches the registry dies with the process, and
           unattended mining that forgets everything on restart is not learning.
           Seal it into the base from the rows it was certified on, write the
           coverage that guards it, and checkpoint both — the mine already cost
           thousands of training epochs, so the I/O is noise beside it. */
        int seal = hybrid_seal_mined_unit(&ai->hybrid, &ai->lane.base,
                                          student_out ? *student_out : NULL,
                                          NULL);
        if (seal < 0)
            fprintf(stderr, "personal_ai: mined unit not sealed (rc=%d) — it "
                            "will not survive restart\n", seal);
        coverage_persist(ai);
        if (gap_lane_checkpoint(&ai->lane) != 0)
            fprintf(stderr, "personal_ai: checkpoint after mine failed\n");
    }
    return rc;
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
    if (ai->owned_residual_http) {
        residual_http_close(ai->owned_residual_http);
        ai->owned_residual_http = NULL;
    }
    gap_lane_close(&ai->lane);
    cnet_gov_close(&ai->gov);
    ai->loaded = 0;
}

void personal_ai_totals(const PersonalAi *ai, PersonalAiReport *out) {
    if (!ai || !out) return;
    *out = ai->totals;
}

int personal_ai_kpi_json(const PersonalAi *ai, char *out, size_t out_capacity) {
    const PersonalAiReport *t;
    size_t served, own;
    double residual_rate = 0.0, substitution_rate = 0.0, abstain_rate = 0.0;
    int written;
    if (!ai || !out || out_capacity == 0) return -1;
    t = &ai->totals;
    /* served = answered requests; abstains are counted separately so a fall in
       residual_rate bought by abstaining more is visible, not hidden. */
    served = t->local_hits + t->soft_hits + t->residual_hits + t->teacher_helps;
    own = t->local_hits + t->soft_hits;
    if (served > 0) {
        residual_rate = (double)t->residual_hits / (double)served;
        substitution_rate = (double)own / (double)served;
    }
    if (served + t->abstains > 0)
        abstain_rate = (double)t->abstains / (double)(served + t->abstains);
    written = snprintf(
        out, out_capacity,
        "{\"schema_version\":1,\"served\":%zu,\"local_hits\":%zu,"
        "\"soft_hits\":%zu,\"residual_hits\":%zu,\"teacher_helps\":%zu,"
        "\"abstains\":%zu,\"teaches\":%zu,\"residual_captures\":%zu,"
        "\"coverage_abstains\":%zu,"
        "\"residual_rate\":%.6f,\"substitution_rate\":%.6f,"
        "\"abstain_rate\":%.6f}",
        served, t->local_hits, t->soft_hits, t->residual_hits,
        t->teacher_helps, t->abstains, t->teaches, t->residual_captures,
        t->coverage_abstains, residual_rate, substitution_rate, abstain_rate);
    if (written < 0 || (size_t)written >= out_capacity) return -2;
    return written;
}

int personal_ai_kpi_write(const PersonalAi *ai, const char *path) {
    char buf[512];
    FILE *fp;
    if (!ai || !path || !path[0]) return -1;
    if (personal_ai_kpi_json(ai, buf, sizeof buf) < 0) return -2;
    fp = fopen(path, "w");
    if (!fp) return -3;
    if (fprintf(fp, "%s\n", buf) < 0) {
        fclose(fp);
        return -4;
    }
    if (fclose(fp) != 0) return -5;
    return 0;
}

const HybridAi *personal_ai_hybrid(const PersonalAi *ai) {
    return ai ? &ai->hybrid : NULL;
}
