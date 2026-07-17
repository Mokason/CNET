#include "../include/resource_governor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long env_long(const char *name, long dflt) {
    const char *v = getenv(name);
    return (v && v[0]) ? atol(v) : dflt;
}

static int env_flag(const char *name, int dflt) {
    const char *v = getenv(name);
    if (!v || !v[0]) return dflt;
    if (v[0] == '0' && v[1] == '\0') return 0;
    return 1;
}

static uint64_t env_u64(const char *name, uint64_t dflt) {
    const char *v = getenv(name);
    if (!v || !v[0]) return dflt;
    return (uint64_t)strtoull(v, NULL, 10);
}

int cnet_gov_policy_validate(const CnetGovPolicy *p) {
    if (!p) return 0;
    if (p->abi_version != CNET_GOV_ABI_VERSION) return 0;
    if (p->struct_size < sizeof(CnetGovPolicy)) return 0;
    if (p->teacher_idle_sec < 0) return 0;
    if (p->health_tick_seconds < 0) return 0;
    return 1;
}

void cnet_gov_policy_deploy_defaults(CnetGovPolicy *p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->abi_version = CNET_GOV_ABI_VERSION;
    p->struct_size = (uint32_t)sizeof *p;
    /* Unlimited budgets until host tightens them — refuse is still
       fail-closed when a host sets a ceiling. */
    p->ram_budget_bytes = 0;
    p->vram_budget_bytes = 0;
    p->max_closures_per_drain = 4;   /* budgeted self-improve default */
    p->teacher_idle_sec = 300;       /* 5 min sleep when idle */
    p->want_oracle_int8 = 1;
    p->want_train_fast = 1;
    p->want_topk_set = 0;            /* v2 campaign opt-in; not silent default */
    p->want_health_tick = 1;
    p->health_tick_seconds = 300;
    p->acq_stages = 40;
    p->want_counterfactual_order = 0; /* opt-in ORDER_ONLY */
}

int cnet_gov_policy_from_env(CnetGovPolicy *p) {
    if (!p || !cnet_gov_policy_validate(p)) return -1;
    if (getenv("CNET_GOV_RAM_BYTES"))
        p->ram_budget_bytes = env_u64("CNET_GOV_RAM_BYTES", p->ram_budget_bytes);
    if (getenv("CNET_GOV_VRAM_BYTES"))
        p->vram_budget_bytes = env_u64("CNET_GOV_VRAM_BYTES", p->vram_budget_bytes);
    if (getenv("CNET_LANE_MAX_CLOSURES") || getenv("CNET_GOV_MAX_CLOSURES")) {
        long v = env_long("CNET_LANE_MAX_CLOSURES", -1);
        if (v < 0) v = env_long("CNET_GOV_MAX_CLOSURES", (long)p->max_closures_per_drain);
        if (v >= 0) p->max_closures_per_drain = (size_t)v;
    }
    if (getenv("CNET_TEACHER_IDLE_SEC"))
        p->teacher_idle_sec = env_long("CNET_TEACHER_IDLE_SEC", p->teacher_idle_sec);
    if (getenv("CNET_ORACLE_INT8"))
        p->want_oracle_int8 = env_flag("CNET_ORACLE_INT8", p->want_oracle_int8);
    if (getenv("CNET_TRAIN_FAST"))
        p->want_train_fast = env_flag("CNET_TRAIN_FAST", p->want_train_fast);
    if (getenv("CNET_TOPK_SET"))
        p->want_topk_set = env_flag("CNET_TOPK_SET", p->want_topk_set);
    if (getenv("CNET_HEALTH_TICK_SECONDS")) {
        p->health_tick_seconds = env_long("CNET_HEALTH_TICK_SECONDS",
                                          p->health_tick_seconds);
        p->want_health_tick = p->health_tick_seconds > 0;
    }
    if (getenv("CNET_ACQ_STAGES"))
        p->acq_stages = (size_t)env_long("CNET_ACQ_STAGES", (long)p->acq_stages);
    if (getenv("CNET_CF_ORDER"))
        p->want_counterfactual_order = env_flag("CNET_CF_ORDER",
                                                p->want_counterfactual_order);
    return 0;
}

int cnet_gov_open(CnetResourceGovernor *g, const CnetGovPolicy *p) {
    CnetGovPolicy local;
    if (!g) return CNET_GOV_INVALID;
    memset(g, 0, sizeof *g);
    if (p) {
        if (!cnet_gov_policy_validate(p)) return CNET_GOV_INVALID;
        g->policy = *p;
    } else {
        cnet_gov_policy_deploy_defaults(&local);
        g->policy = local;
    }
    g->loaded = 1;
    return CNET_GOV_OK;
}

void cnet_gov_close(CnetResourceGovernor *g) {
    if (!g) return;
    memset(g, 0, sizeof *g);
}

int cnet_gov_report_usage(CnetResourceGovernor *g, const CnetGovUsage *u) {
    if (!g || !g->loaded || !u) return CNET_GOV_INVALID;
    g->usage = *u;
    return CNET_GOV_OK;
}

int cnet_gov_would_admit(const CnetResourceGovernor *g,
                         uint64_t ram_delta, uint64_t vram_delta) {
    if (!g || !g->loaded) return 0;
    if (g->policy.ram_budget_bytes) {
        uint64_t next = g->usage.ram_resident_bytes + ram_delta;
        if (next < g->usage.ram_resident_bytes) return 0; /* overflow */
        if (next > g->policy.ram_budget_bytes) return 0;
    }
    if (g->policy.vram_budget_bytes) {
        uint64_t next = g->usage.vram_resident_bytes + vram_delta;
        if (next < g->usage.vram_resident_bytes) return 0;
        if (next > g->policy.vram_budget_bytes) return 0;
    }
    return 1;
}

int cnet_gov_admit(CnetResourceGovernor *g,
                   uint64_t ram_delta, uint64_t vram_delta) {
    if (!g || !g->loaded) return CNET_GOV_INVALID;
    if (!cnet_gov_would_admit(g, ram_delta, vram_delta)) {
        g->refuses++;
        return CNET_GOV_OVER_BUDGET;
    }
    g->usage.ram_resident_bytes += ram_delta;
    g->usage.vram_resident_bytes += vram_delta;
    g->admits++;
    return CNET_GOV_OK;
}

int cnet_gov_release(CnetResourceGovernor *g,
                     uint64_t ram_delta, uint64_t vram_delta) {
    if (!g || !g->loaded) return CNET_GOV_INVALID;
    if (ram_delta > g->usage.ram_resident_bytes ||
        vram_delta > g->usage.vram_resident_bytes)
        return CNET_GOV_INVALID;
    g->usage.ram_resident_bytes -= ram_delta;
    g->usage.vram_resident_bytes -= vram_delta;
    return CNET_GOV_OK;
}

void cnet_gov_begin_drain(CnetResourceGovernor *g) {
    if (!g || !g->loaded) return;
    g->usage.closures_this_drain = 0;
}

int cnet_gov_note_close(CnetResourceGovernor *g) {
    if (!g || !g->loaded) return 0;
    if (g->policy.max_closures_per_drain == 0) {
        g->usage.closures_this_drain++;
        return 1;
    }
    if (g->usage.closures_this_drain >= g->policy.max_closures_per_drain)
        return 0;
    g->usage.closures_this_drain++;
    return 1;
}

int cnet_gov_teacher_should_sleep(const CnetResourceGovernor *g) {
    if (!g || !g->loaded) return 0;
    if (g->policy.teacher_idle_sec <= 0) return 0;
    if (!g->usage.teacher_resident) return 0;
    return g->usage.teacher_idle_for_sec >= g->policy.teacher_idle_sec;
}

int cnet_gov_format(const CnetResourceGovernor *g, char *out, size_t out_cap) {
    int n;
    if (!g || !g->loaded || !out || out_cap < 8) return -1;
    n = snprintf(out, out_cap,
                 "gov ram=%llu/%llu vram=%llu/%llu close=%zu/%zu "
                 "idle=%ld/%ld int8=%d fast=%d topk_set=%d cf_order=%d "
                 "admits=%u refuses=%u",
                 (unsigned long long)g->usage.ram_resident_bytes,
                 (unsigned long long)g->policy.ram_budget_bytes,
                 (unsigned long long)g->usage.vram_resident_bytes,
                 (unsigned long long)g->policy.vram_budget_bytes,
                 g->usage.closures_this_drain,
                 g->policy.max_closures_per_drain,
                 g->usage.teacher_idle_for_sec,
                 g->policy.teacher_idle_sec,
                 g->policy.want_oracle_int8,
                 g->policy.want_train_fast,
                 g->policy.want_topk_set,
                 g->policy.want_counterfactual_order,
                 g->admits, g->refuses);
    return n;
}
