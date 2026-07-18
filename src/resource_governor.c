#include "../include/resource_governor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

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

static uint64_t now_ms(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000ull +
               (uint64_t)ts.tv_nsec / 1000000ull;
#endif
    return (uint64_t)time(NULL) * 1000ull;
}

static void sleep_ms(long ms) {
    if (ms <= 0) return;
#if defined(_WIN32)
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* setenv if force, or if key unset / empty. */
static void env_set(const char *key, const char *val, int force) {
    const char *cur;
    if (!key || !val) return;
    cur = getenv(key);
    if (!force && cur && cur[0]) return;
    setenv(key, val, 1);
}

static void env_set_int(const char *key, int v, int force) {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", v);
    env_set(key, buf, force);
}

static void env_set_float(const char *key, float v, int force) {
    char buf[32];
    snprintf(buf, sizeof buf, "%.4g", (double)v);
    env_set(key, buf, force);
}

int cnet_gov_policy_validate(const CnetGovPolicy *p) {
    if (!p) return 0;
    if (p->abi_version != CNET_GOV_ABI_VERSION) return 0;
    if (p->struct_size < sizeof(CnetGovPolicy)) return 0;
    if (p->teacher_idle_sec < 0) return 0;
    if (p->health_tick_seconds < 0) return 0;
    if (p->min_token_gap_ms < 0) return 0;
    if (p->idle_cool_ms < 0) return 0;
    if (p->dsa_fraction < 0.f || p->dsa_fraction > 1.f) return 0;
    if ((int)p->compute_profile < 0 ||
        (int)p->compute_profile > (int)CNET_GOV_PROFILE_CUSTOM)
        return 0;
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
    /* Compute: balanced by default — sparse without floor grid. */
    p->compute_profile = CNET_GOV_PROFILE_BALANCED;
    p->dsa_fraction = 0.f; /* 0 → use profile table */
    p->force_env = 0;
    p->min_token_gap_ms = 0; /* 0 → profile */
    p->idle_cool_ms = 0;
    p->want_kv_page = -1;
    p->want_mtp_k = -1;
}

int cnet_gov_policy_set_profile(CnetGovPolicy *p, CnetGovProfileKind k) {
    if (!p) return CNET_GOV_INVALID;
    if ((int)k < 0 || (int)k > (int)CNET_GOV_PROFILE_CUSTOM)
        return CNET_GOV_INVALID;
    p->compute_profile = k;
    return CNET_GOV_OK;
}

int cnet_gov_policy_set_profile_name(CnetGovPolicy *p, const char *name) {
    if (!p) return CNET_GOV_INVALID;
    if (!name || !name[0]) {
        p->compute_profile = CNET_GOV_PROFILE_BALANCED;
        return CNET_GOV_OK;
    }
    if (strcmp(name, "eco") == 0 || strcmp(name, "ECO") == 0)
        p->compute_profile = CNET_GOV_PROFILE_ECO;
    else if (strcmp(name, "balanced") == 0 || strcmp(name, "BALANCED") == 0 ||
             strcmp(name, "default") == 0)
        p->compute_profile = CNET_GOV_PROFILE_BALANCED;
    else if (strcmp(name, "turbo") == 0 || strcmp(name, "TURBO") == 0 ||
             strcmp(name, "max") == 0)
        p->compute_profile = CNET_GOV_PROFILE_TURBO;
    else if (strcmp(name, "custom") == 0)
        p->compute_profile = CNET_GOV_PROFILE_CUSTOM;
    else
        return CNET_GOV_INVALID;
    return CNET_GOV_OK;
}

int cnet_gov_policy_from_env(CnetGovPolicy *p) {
    const char *prof;
    if (!p || !cnet_gov_policy_validate(p)) return -1;
    if (getenv("CNET_GOV_RAM_BYTES"))
        p->ram_budget_bytes = env_u64("CNET_GOV_RAM_BYTES", p->ram_budget_bytes);
    if (getenv("CNET_GOV_VRAM_BYTES"))
        p->vram_budget_bytes =
            env_u64("CNET_GOV_VRAM_BYTES", p->vram_budget_bytes);
    if (getenv("CNET_LANE_MAX_CLOSURES") || getenv("CNET_GOV_MAX_CLOSURES")) {
        long v = env_long("CNET_LANE_MAX_CLOSURES", -1);
        if (v < 0)
            v = env_long("CNET_GOV_MAX_CLOSURES", (long)p->max_closures_per_drain);
        if (v >= 0) p->max_closures_per_drain = (size_t)v;
    }
    if (getenv("CNET_TEACHER_IDLE_SEC"))
        p->teacher_idle_sec =
            env_long("CNET_TEACHER_IDLE_SEC", p->teacher_idle_sec);
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
        p->want_counterfactual_order =
            env_flag("CNET_CF_ORDER", p->want_counterfactual_order);

    prof = getenv("CNET_GOV_PROFILE");
    if (prof && prof[0])
        (void)cnet_gov_policy_set_profile_name(p, prof);
    if (getenv("CNET_GOV_FORCE"))
        p->force_env = env_flag("CNET_GOV_FORCE", p->force_env);
    if (getenv("CNET_GOV_TOKEN_GAP_MS"))
        p->min_token_gap_ms =
            env_long("CNET_GOV_TOKEN_GAP_MS", p->min_token_gap_ms);
    if (getenv("CNET_GOV_IDLE_COOL_MS"))
        p->idle_cool_ms = env_long("CNET_GOV_IDLE_COOL_MS", p->idle_cool_ms);
    if (getenv("CNET_GOV_DSA_FRACTION")) {
        const char *e = getenv("CNET_GOV_DSA_FRACTION");
        if (e && e[0]) {
            float f = (float)atof(e);
            if (f >= 0.f && f <= 1.f) p->dsa_fraction = f;
        }
    }
    if (getenv("CNET_GOV_KV_PAGE"))
        p->want_kv_page = env_flag("CNET_GOV_KV_PAGE", 1) ? 1 : 0;
    if (getenv("CNET_GOV_MTP_K"))
        p->want_mtp_k = (int)env_long("CNET_GOV_MTP_K", 0);
    return 0;
}

static void fill_plan_eco(CnetGovComputePlan *c) {
    memset(c, 0, sizeof *c);
    c->dsa_enable = 1;
    c->dsa_fraction = 0.20f;
    c->dsa_speed = 1; /* more skips */
    c->mla_kv = 1;
    c->kv_page = 1;
    c->kv_hot_pages = 2;
    c->kv_page_len = 256;
    c->kv_rehydrate = 0;
    c->kv_quant = 1;
    c->mtp_k = 2;
    c->mtp_parallel = 1;
    c->ep_places = 1;
    c->min_token_gap_ms = 2;  /* light pace — avoid permanent peg */
    c->idle_cool_ms = 500;
    c->teacher_idle_sec = 120;
    c->max_gpu_places = 1;
}

static void fill_plan_balanced(CnetGovComputePlan *c) {
    memset(c, 0, sizeof *c);
    c->dsa_enable = 1;
    c->dsa_fraction = 0.25f;
    c->dsa_speed = 0; /* quality: sleep only */
    c->mla_kv = 1;
    c->kv_page = 1;
    c->kv_hot_pages = 4;
    c->kv_page_len = 256;
    c->kv_rehydrate = 0;
    c->kv_quant = 1;
    c->mtp_k = 2;
    c->mtp_parallel = 1;
    c->ep_places = 2;
    c->min_token_gap_ms = 0;
    c->idle_cool_ms = 200;
    c->teacher_idle_sec = 300;
    c->max_gpu_places = 2;
}

static void fill_plan_turbo(CnetGovComputePlan *c) {
    memset(c, 0, sizeof *c);
    c->dsa_enable = 1;
    c->dsa_fraction = 0.50f; /* denser support */
    c->dsa_speed = 0;
    c->mla_kv = 1;
    c->kv_page = 1;
    c->kv_hot_pages = 8;
    c->kv_page_len = 256;
    c->kv_rehydrate = 1; /* long-range ok */
    c->kv_quant = 1;
    c->mtp_k = 4;
    c->mtp_parallel = 1;
    c->ep_places = 2;
    c->min_token_gap_ms = 0;
    c->idle_cool_ms = 50;
    c->teacher_idle_sec = 600;
    c->max_gpu_places = 2;
}

int cnet_gov_resolve_compute(CnetResourceGovernor *g) {
    CnetGovComputePlan *c;
    const CnetGovPolicy *p;
    if (!g || !g->loaded) return CNET_GOV_INVALID;
    p = &g->policy;
    c = &g->plan;
    switch (p->compute_profile) {
    case CNET_GOV_PROFILE_ECO:
        fill_plan_eco(c);
        break;
    case CNET_GOV_PROFILE_TURBO:
        fill_plan_turbo(c);
        break;
    case CNET_GOV_PROFILE_CUSTOM:
        fill_plan_balanced(c); /* base then overrides */
        break;
    case CNET_GOV_PROFILE_BALANCED:
    default:
        fill_plan_balanced(c);
        break;
    }
    if (p->dsa_fraction > 0.f) {
        c->dsa_fraction = p->dsa_fraction;
        c->dsa_enable = 1;
    }
    if (p->min_token_gap_ms > 0) c->min_token_gap_ms = p->min_token_gap_ms;
    if (p->idle_cool_ms > 0) c->idle_cool_ms = p->idle_cool_ms;
    if (p->want_kv_page >= 0) c->kv_page = p->want_kv_page ? 1 : 0;
    if (p->want_mtp_k >= 0) c->mtp_k = p->want_mtp_k;
    if (p->teacher_idle_sec > 0) c->teacher_idle_sec = p->teacher_idle_sec;
    c->applied = 0;
    return CNET_GOV_OK;
}

int cnet_gov_apply_compute_env(CnetResourceGovernor *g) {
    CnetGovComputePlan *c;
    int force;
    if (!g || !g->loaded) return CNET_GOV_INVALID;
    if (cnet_gov_resolve_compute(g) != CNET_GOV_OK) return CNET_GOV_INVALID;
    c = &g->plan;
    force = g->policy.force_env || env_flag("CNET_GOV_FORCE", 0);

    if (c->dsa_enable) {
        env_set_int("CNET_DSA", 1, force);
        env_set_float("CNET_SPARSE_KV", c->dsa_fraction, force);
        if (c->dsa_speed)
            env_set("CNET_DSA_PROFILE", "speed", force);
    } else {
        env_set_int("CNET_DSA", 0, force);
    }
    env_set_int("CNET_MLA_KV", c->mla_kv ? 1 : 0, force);
    env_set_int("CNET_KV_PAGE", c->kv_page ? 1 : 0, force);
    if (c->kv_page) {
        env_set_int("CNET_KV_HOT_PAGES", c->kv_hot_pages, force);
        env_set_int("CNET_KV_PAGE_LEN", c->kv_page_len, force);
        env_set_int("CNET_KV_QUANT", c->kv_quant ? 1 : 0, force);
        env_set_int("CNET_KV_REHYDRATE", c->kv_rehydrate ? 1 : 0, force);
        env_set_int("CNET_KV_ASYNC", 1, force);
    }
    if (c->mtp_k > 0) {
        env_set_int("CNET_MTP_K", c->mtp_k, force);
        env_set_int("CNET_MTP_PARALLEL", c->mtp_parallel ? 1 : 0, force);
    } else {
        env_set_int("CNET_MTP_K", 0, force);
    }
    env_set_int("CNET_EP_PLACES", c->ep_places > 0 ? c->ep_places : 1, force);

    c->applied = 1;
    return CNET_GOV_OK;
}

int cnet_gov_orchestrate_boot(CnetResourceGovernor *g,
                              const char *profile_name) {
    CnetGovPolicy p;
    const char *name = profile_name;
    if (!g) return CNET_GOV_INVALID;
    cnet_gov_policy_deploy_defaults(&p);
    if (!name || !name[0]) name = getenv("CNET_GOV_PROFILE");
    if (name && name[0]) {
        if (cnet_gov_policy_set_profile_name(&p, name) != CNET_GOV_OK)
            return CNET_GOV_INVALID;
    }
    (void)cnet_gov_policy_from_env(&p);
    if (cnet_gov_open(g, &p) != CNET_GOV_OK) return CNET_GOV_INVALID;
    if (cnet_gov_apply_compute_env(g) != CNET_GOV_OK) return CNET_GOV_INVALID;
    g->usage.phase = CNET_GOV_PHASE_IDLE;
    return CNET_GOV_OK;
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
    g->usage.phase = CNET_GOV_PHASE_IDLE;
    (void)cnet_gov_resolve_compute(g);
    return CNET_GOV_OK;
}

void cnet_gov_close(CnetResourceGovernor *g) {
    if (!g) return;
    memset(g, 0, sizeof *g);
}

int cnet_gov_report_usage(CnetResourceGovernor *g, const CnetGovUsage *u) {
    CnetGovPhase keep_phase;
    uint64_t tok, bursts, yields, cool;
    if (!g || !g->loaded || !u) return CNET_GOV_INVALID;
    /* Preserve orchestrator counters unless host overwrites phase carelessly:
       merge residency fields, keep our tokens if host zeros them. */
    keep_phase = g->usage.phase;
    tok = g->usage.tokens_generated;
    bursts = g->usage.generate_bursts;
    yields = g->usage.pace_yields;
    cool = g->usage.cool_signals;
    g->usage = *u;
    if (g->usage.tokens_generated == 0 && tok) g->usage.tokens_generated = tok;
    if (g->usage.generate_bursts == 0 && bursts)
        g->usage.generate_bursts = bursts;
    if (g->usage.pace_yields == 0 && yields) g->usage.pace_yields = yields;
    if (g->usage.cool_signals == 0 && cool) g->usage.cool_signals = cool;
    if (u->phase == CNET_GOV_PHASE_IDLE && keep_phase != CNET_GOV_PHASE_IDLE &&
        u->tokens_generated == 0)
        g->usage.phase = keep_phase;
    return CNET_GOV_OK;
}

int cnet_gov_would_admit(const CnetResourceGovernor *g, uint64_t ram_delta,
                         uint64_t vram_delta) {
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

int cnet_gov_admit(CnetResourceGovernor *g, uint64_t ram_delta,
                   uint64_t vram_delta) {
    if (!g || !g->loaded) return CNET_GOV_INVALID;
    if (!cnet_gov_would_admit(g, ram_delta, vram_delta)) {
        g->refuses++;
        return CNET_GOV_OVER_BUDGET;
    }
    g->usage.ram_resident_bytes += ram_delta;
    g->usage.vram_resident_bytes += vram_delta;
    g->admits++;
    if (g->usage.phase == CNET_GOV_PHASE_IDLE)
        g->usage.phase = CNET_GOV_PHASE_WARM;
    return CNET_GOV_OK;
}

int cnet_gov_release(CnetResourceGovernor *g, uint64_t ram_delta,
                     uint64_t vram_delta) {
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
    g->usage.phase = CNET_GOV_PHASE_DRAIN;
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
    long thr;
    if (!g || !g->loaded) return 0;
    thr = g->policy.teacher_idle_sec > 0 ? g->policy.teacher_idle_sec
                                         : g->plan.teacher_idle_sec;
    if (thr <= 0) return 0;
    if (!g->usage.teacher_resident) return 0;
    /* Prefer cool when IDLE after generate. */
    if (g->usage.phase != CNET_GOV_PHASE_IDLE &&
        g->usage.phase != CNET_GOV_PHASE_DRAIN)
        return 0;
    return g->usage.teacher_idle_for_sec >= thr;
}

int cnet_gov_begin_generate(CnetResourceGovernor *g) {
    if (!g || !g->loaded) return CNET_GOV_INVALID;
    g->usage.phase = CNET_GOV_PHASE_GENERATE;
    g->usage.generate_bursts++;
    g->last_token_ms = 0;
    return CNET_GOV_OK;
}

int cnet_gov_between_token(CnetResourceGovernor *g) {
    long gap;
    uint64_t t, elapsed;
    if (!g || !g->loaded) return CNET_GOV_INVALID;
    g->usage.tokens_generated++;
    g->usage.phase = CNET_GOV_PHASE_GENERATE;
    gap = g->plan.min_token_gap_ms;
    if (g->policy.min_token_gap_ms > 0) gap = g->policy.min_token_gap_ms;
    t = now_ms();
    if (gap > 0 && g->last_token_ms > 0) {
        elapsed = t - g->last_token_ms;
        if (elapsed < (uint64_t)gap) {
            sleep_ms((long)((uint64_t)gap - elapsed));
            g->usage.pace_yields++;
            t = now_ms();
        }
    }
    g->last_token_ms = t;
    return CNET_GOV_OK;
}

int cnet_gov_end_generate(CnetResourceGovernor *g) {
    long cool;
    if (!g || !g->loaded) return 0;
    g->usage.phase = CNET_GOV_PHASE_DRAIN;
    cool = g->plan.idle_cool_ms;
    if (g->policy.idle_cool_ms > 0) cool = g->policy.idle_cool_ms;
    g->usage.phase = CNET_GOV_PHASE_IDLE;
    g->last_token_ms = 0;
    if (cool > 0) {
        g->usage.cool_signals++;
        /* Brief yield so the process is not pegged between bursts. */
        sleep_ms(cool > 50 ? 5 : cool); /* tiny yield; full cool is host's job */
        return 1;
    }
    return 0;
}

int cnet_gov_tick_idle(CnetResourceGovernor *g, long idle_sec) {
    if (!g || !g->loaded) return CNET_GOV_INVALID;
    if (idle_sec < 0) idle_sec = 0;
    g->usage.teacher_idle_for_sec = idle_sec;
    if (g->usage.phase == CNET_GOV_PHASE_GENERATE) {
        /* host says idle — leave generate */
        g->usage.phase = CNET_GOV_PHASE_IDLE;
    }
    return cnet_gov_teacher_should_sleep(g) ? 1 : 0;
}

int cnet_gov_should_cool(const CnetResourceGovernor *g) {
    if (!g || !g->loaded) return 0;
    if (g->usage.phase != CNET_GOV_PHASE_IDLE) return 0;
    return g->usage.cool_signals > 0 ? 1 : 0;
}

int cnet_gov_format(const CnetResourceGovernor *g, char *out, size_t out_cap) {
    int n;
    const char *prof = "?";
    const char *phase = "?";
    if (!g || !g->loaded || !out || out_cap < 8) return -1;
    switch (g->policy.compute_profile) {
    case CNET_GOV_PROFILE_ECO:
        prof = "eco";
        break;
    case CNET_GOV_PROFILE_BALANCED:
        prof = "balanced";
        break;
    case CNET_GOV_PROFILE_TURBO:
        prof = "turbo";
        break;
    case CNET_GOV_PROFILE_CUSTOM:
        prof = "custom";
        break;
    }
    switch (g->usage.phase) {
    case CNET_GOV_PHASE_IDLE:
        phase = "idle";
        break;
    case CNET_GOV_PHASE_WARM:
        phase = "warm";
        break;
    case CNET_GOV_PHASE_GENERATE:
        phase = "generate";
        break;
    case CNET_GOV_PHASE_DRAIN:
        phase = "drain";
        break;
    }
    n = snprintf(out, out_cap,
                 "gov prof=%s phase=%s ram=%llu/%llu vram=%llu/%llu "
                 "close=%zu/%zu idle=%ld/%ld dsa=%.2f hot=%d mtp=%d gap_ms=%ld "
                 "tok=%llu bursts=%llu yields=%llu cool=%llu "
                 "int8=%d fast=%d admits=%u refuses=%u",
                 prof, phase,
                 (unsigned long long)g->usage.ram_resident_bytes,
                 (unsigned long long)g->policy.ram_budget_bytes,
                 (unsigned long long)g->usage.vram_resident_bytes,
                 (unsigned long long)g->policy.vram_budget_bytes,
                 g->usage.closures_this_drain, g->policy.max_closures_per_drain,
                 g->usage.teacher_idle_for_sec, g->policy.teacher_idle_sec,
                 (double)g->plan.dsa_fraction, g->plan.kv_hot_pages,
                 g->plan.mtp_k, g->plan.min_token_gap_ms,
                 (unsigned long long)g->usage.tokens_generated,
                 (unsigned long long)g->usage.generate_bursts,
                 (unsigned long long)g->usage.pace_yields,
                 (unsigned long long)g->usage.cool_signals,
                 g->policy.want_oracle_int8, g->policy.want_train_fast,
                 g->admits, g->refuses);
    return n;
}
