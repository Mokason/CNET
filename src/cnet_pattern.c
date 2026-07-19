#include "../include/cnet_pattern.h"
#include "../include/cnet_platform.h"
#include "../include/cnet_lfru.h"
#include "../include/cnet_math_solve.h"
#include "../include/contract/mcp_math_eval.h"
#include "../include/contract/mcp_wiki.h"
#include "../include/contract/mcp_web_search.h"
#include "../include/agent_memory.h"

#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


/* Optional SoulHost via dlopen(cnet.so) — avoids linking the full CCE stack
 * into every pattern binary while still enabling live unit callouts. */
typedef int (*fn_soul_open)(const char *, const char *, void **);
typedef void (*fn_soul_close)(void *);
typedef int (*fn_soul_unit_count)(void *);
typedef int (*fn_soul_unit_name)(void *, int, char *, int);
typedef int (*fn_soul_unit_dims)(void *, const char *, int *, int *);
typedef int (*fn_soul_unit_reliability_milli)(void *, const char *);
typedef int (*fn_soul_run)(void *, const char *, const double *, double *, int);

static void *g_cnet_so;
static fn_soul_open g_soul_open;
static fn_soul_close g_soul_close;
static fn_soul_unit_count g_soul_unit_count;
static fn_soul_unit_name g_soul_unit_name;
static fn_soul_unit_dims g_soul_unit_dims;
static fn_soul_unit_reliability_milli g_soul_unit_rel;
static fn_soul_run g_soul_run;

static int soul_api_load(void) {
    const char *cands[8];
    int i, n = 0;
    if (g_soul_open) return 0;
    cands[n++] = getenv("CNET_SO_PATH");
    cands[n++] = "./cnet.so";
    cands[n++] = "cnet.so";
    cands[n++] = "/home/marble/AI/CNET/cnet.so";
    for (i = 0; i < n; i++) {
        if (!cands[i] || !cands[i][0]) continue;
        g_cnet_so = dlopen(cands[i], RTLD_NOW | RTLD_GLOBAL);
        if (g_cnet_so) break;
    }
    if (!g_cnet_so) return -1;
    g_soul_open = (fn_soul_open)dlsym(g_cnet_so, "soul_open");
    g_soul_close = (fn_soul_close)dlsym(g_cnet_so, "soul_close");
    g_soul_unit_count = (fn_soul_unit_count)dlsym(g_cnet_so, "soul_unit_count");
    g_soul_unit_name = (fn_soul_unit_name)dlsym(g_cnet_so, "soul_unit_name");
    g_soul_unit_dims = (fn_soul_unit_dims)dlsym(g_cnet_so, "soul_unit_dims");
    g_soul_unit_rel =
        (fn_soul_unit_reliability_milli)dlsym(g_cnet_so, "soul_unit_reliability_milli");
    g_soul_run = (fn_soul_run)dlsym(g_cnet_so, "soul_run");
    if (!g_soul_open || !g_soul_close || !g_soul_unit_count || !g_soul_unit_name ||
        !g_soul_unit_dims || !g_soul_run)
        return -1;
    return 0;
}

static uint64_t fnv1a64(const char *s) {
    uint64_t h = 14695981039346656037ull;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    while (*p) {
        h ^= *p++;
        h *= 1099511628211ull;
    }
    return h;
}

static void port_raw_tag(Port *p, const char *tag, size_t w) {
    memset(p, 0, sizeof *p);
    p->family = PORT_RAW;
    p->field_width = w ? w : CNET_PATTERN_CODE_DIM;
    p->field_count = 1;
    if (tag) snprintf(p->tag, sizeof p->tag, "%s", tag);
}

const char *cnet_pattern_state_name(CnetPatternState s) {
    switch (s) {
    case CNET_PAT_STATE_FLUID: return "fluid";
    case CNET_PAT_STATE_IMPROVING: return "improving";
    case CNET_PAT_STATE_FROZEN: return "frozen";
    case CNET_PAT_STATE_DEMOTED: return "demoted";
    default: return "none";
    }
}

const char *cnet_pattern_body_name(CnetPatternBodyKind k) {
    switch (k) {
    case CNET_PAT_BODY_UNIT: return "unit";
    case CNET_PAT_BODY_TOOL: return "tool";
    case CNET_PAT_BODY_MATH: return "math";
    case CNET_PAT_BODY_SKILL: return "skill";
    case CNET_PAT_BODY_HERMETIC: return "hermetic";
    default: return "none";
    }
}

void cnet_pattern_runtime_init(CnetPatternRuntime *rt) {
    if (!rt) return;
    memset(rt, 0, sizeof *rt);
    rt->hot_cap = 8;
    rt->freeze_min_reliability = 0.95;
    rt->freeze_min_successes = 3;
    snprintf(rt->store_path, sizeof rt->store_path, "logs/pattern_runtime.jsonl");
    snprintf(rt->skills_dir, sizeof rt->skills_dir, "logs/personal_ai_skills");
}

void cnet_pattern_runtime_from_env(CnetPatternRuntime *rt) {
    const char *e;
    cnet_pattern_runtime_init(rt);
    e = getenv("CNET_PATTERN_STORE");
    if (e && e[0]) snprintf(rt->store_path, sizeof rt->store_path, "%s", e);
    e = getenv("CNET_SKILLS_DIR");
    if (e && e[0]) snprintf(rt->skills_dir, sizeof rt->skills_dir, "%s", e);
    e = getenv("CNET_BASE_PATH");
    if (!e || !e[0]) e = getenv("BASE_PATH");
    if (e && e[0]) snprintf(rt->base_path, sizeof rt->base_path, "%s", e);
    e = getenv("CNET_PATTERN_HOT_CAP");
    if (e && e[0]) {
        int c = atoi(e);
        if (c > 0 && c <= CNET_PATTERN_MAX_HOT) rt->hot_cap = c;
    }
}

int cnet_pattern_bind_soul(CnetPatternRuntime *rt, void *soul_host) {
    if (!rt) return -1;
    if (rt->soul_owned && rt->soul_host && g_soul_close) {
        g_soul_close(rt->soul_host);
        rt->soul_owned = 0;
    }
    rt->soul_host = soul_host;
    rt->soul_owned = 0;
    return soul_host ? 0 : -1;
}

int cnet_pattern_bind_base(CnetPatternRuntime *rt, const char *base_path) {
    void *h = NULL;
    char path[CNET_PATTERN_PATH_MAX];
    if (!rt) return -1;
    if (rt->soul_owned && rt->soul_host && g_soul_close) {
        g_soul_close(rt->soul_host);
        rt->soul_host = NULL;
        rt->soul_owned = 0;
    }
    /* Copy first — callers often pass rt->base_path (no overlap UB). */
    path[0] = '\0';
    if (base_path && base_path[0])
        snprintf(path, sizeof path, "%s", base_path);
    else if (rt->base_path[0])
        snprintf(path, sizeof path, "%s", rt->base_path);
    if (!path[0]) return -1;
    if (soul_api_load() != 0) {
        fprintf(stderr, "cnet_pattern: soul_api_load failed (dlopen cnet.so)\n");
        return -2;
    }
    snprintf(rt->base_path, sizeof rt->base_path, "%s", path);
    {
        int orc = g_soul_open(path, NULL, &h);
        if (orc != 0 || !h) {
            fprintf(stderr, "cnet_pattern: soul_open(%s) rc=%d h=%p\n", path, orc, h);
            return -1;
        }
    }
    rt->soul_host = h;
    rt->soul_owned = 1;
    return 0;
}

void cnet_pattern_unbind_soul(CnetPatternRuntime *rt) {
    if (!rt) return;
    if (rt->soul_owned && rt->soul_host && g_soul_close)
        g_soul_close(rt->soul_host);
    rt->soul_host = NULL;
    rt->soul_owned = 0;
}

int cnet_pattern_import_units(CnetPatternRuntime *rt, int max_n) {
    void *h;
    int n, i, imported = 0, lim;
    if (!rt) return -1;
    if (!rt->soul_host && rt->base_path[0])
        cnet_pattern_bind_base(rt, rt->base_path);
    h = rt->soul_host;
    if (!h || !g_soul_unit_count) return -1;
    n = g_soul_unit_count(h);
    if (n < 0) return -1;
    lim = (max_n > 0 && max_n < n) ? max_n : n;
    /* Cap to free edge slots */
    if ((size_t)lim > CNET_PATTERN_MAX_EDGES - rt->edge_count)
        lim = (int)(CNET_PATTERN_MAX_EDGES - rt->edge_count);
    for (i = 0; i < lim; i++) {
        char uname[CNET_PATTERN_NAME_MAX];
        char edge_name[CNET_PATTERN_NAME_MAX];
        CnetPatternAddr in_a, out_a;
        Port in_p, out_p;
        int in_t = 0, out_t = 0, idx, milli = 0;
        if (g_soul_unit_name(h, i, uname, (int)sizeof uname) != 0) continue;
        g_soul_unit_dims(h, uname, &in_t, &out_t);
        if (g_soul_unit_rel) milli = g_soul_unit_rel(h, uname);
        cnet_pattern_addr_from_text(uname, "unit", &in_a);
        cnet_pattern_addr_from_text(uname, "unit_out", &out_a);
        memset(&in_p, 0, sizeof in_p);
        memset(&out_p, 0, sizeof out_p);
        in_p.family = PORT_RAW;
        in_p.field_width = (size_t)(in_t > 0 ? in_t : CNET_PATTERN_CODE_DIM);
        in_p.field_count = 1;
        snprintf(in_p.tag, sizeof in_p.tag, "unit_in");
        out_p.family = PORT_ONEHOT;
        out_p.field_width = (size_t)(out_t > 0 ? out_t : 1);
        out_p.field_count = 1;
        snprintf(out_p.tag, sizeof out_p.tag, "unit_out");
        snprintf(edge_name, sizeof edge_name, "unit:%.48s", uname);
        idx = cnet_pattern_propose(rt, edge_name, &in_a, &out_a, in_p, out_p,
                                   CNET_PAT_BODY_UNIT, uname);
        if (idx < 0) break;
        {
            CnetPatternEdge *e = &rt->edges[idx];
            e->successes = milli > 0 ? 3u : 1u;
            e->failures = 0;
            e->reliability = milli > 0 ? (milli / 1000.0) : 1.0;
            if (e->reliability > 1.0) e->reliability = 1.0;
            e->state = CNET_PAT_STATE_FROZEN;
            e->frozen_unix = (uint64_t)time(NULL);
        }
        imported++;
    }
    return imported;
}

int cnet_pattern_promote_all(CnetPatternRuntime *rt) {
    size_t i;
    int n = 0;
    if (!rt) return -1;
    for (i = 0; i < rt->edge_count; i++) {
        if (rt->edges[i].state == CNET_PAT_STATE_IMPROVING ||
            rt->edges[i].state == CNET_PAT_STATE_FLUID) {
            if (cnet_pattern_freeze(rt, (int)i, 0) == 0) n++;
        }
    }
    return n;
}

int cnet_pattern_propose_curriculum(CnetPatternRuntime *rt, const char *kind,
                                    const char *text) {
    CnetPatternAddr in_a, out_a;
    Port in_p, out_p;
    char name[CNET_PATTERN_NAME_MAX];
    CnetPatternBodyKind bk = CNET_PAT_BODY_TOOL;
    const char *ref = "wiki_lookup";
    int idx;
    if (!rt || !text || !text[0]) return -1;
    if (kind && (strcmp(kind, "grow_local") == 0 || strcmp(kind, "window_token") == 0)) {
        bk = CNET_PAT_BODY_MATH; /* teachable numeric/pattern demand */
        ref = "math_solve";
    } else if (kind && strcmp(kind, "lookup_skill") == 0) {
        bk = CNET_PAT_BODY_TOOL;
        ref = "wiki_lookup";
    } else if (kind && strcmp(kind, "jtc_expand") == 0) {
        bk = CNET_PAT_BODY_HERMETIC;
        ref = "jtc_expand_todo";
    } else if (kind && (strcmp(kind, "math") == 0 || strcmp(kind, "algebra") == 0)) {
        bk = CNET_PAT_BODY_MATH;
        ref = "math_solve";
    }
    cnet_pattern_addr_from_text(text, kind ? kind : "curriculum", &in_a);
    cnet_pattern_addr_from_text(ref, "curriculum_out", &out_a);
    memset(&in_p, 0, sizeof in_p);
    memset(&out_p, 0, sizeof out_p);
    in_p.family = PORT_RAW;
    in_p.field_width = CNET_PATTERN_CODE_DIM;
    in_p.field_count = 1;
    snprintf(in_p.tag, sizeof in_p.tag, "%s", kind ? kind : "curriculum");
    out_p = in_p;
    snprintf(out_p.tag, sizeof out_p.tag, "answer");
    snprintf(name, sizeof name, "cur:%08x",
             (unsigned)(fnv1a64(text) & 0xffffffffu));
    idx = cnet_pattern_propose(rt, name, &in_a, &out_a, in_p, out_p, bk, ref);
    return idx;
}

void cnet_pattern_addr_from_text(const char *text, const char *role,
                                 CnetPatternAddr *out) {
    uint64_t h;
    int i;
    if (!out) return;
    memset(out, 0, sizeof *out);
    h = fnv1a64(text ? text : "");
    for (i = 0; i < CNET_PATTERN_CODE_DIM; i++) {
        /* mix into discrete-ish codes in [0, 15] then normalize to [0,1] */
        uint64_t nibble = (h >> (i * 8)) & 0xffull;
        out->code[i] = (double)(nibble % 16) / 15.0;
        h ^= h << 13;
        h *= 0x9e3779b97f4a7c15ull;
    }
    if (role && role[0])
        snprintf(out->role, sizeof out->role, "%s", role);
    else
        snprintf(out->role, sizeof out->role, "query");
    out->version = 1;
    out->place_slot = UINT32_MAX;
}

static double addr_similarity(const CnetPatternAddr *a, const CnetPatternAddr *b) {
    double dot = 0, na = 0, nb = 0;
    int i;
    if (!a || !b) return 0;
    if (a->role[0] && b->role[0] && strcmp(a->role, b->role) != 0) return 0;
    for (i = 0; i < CNET_PATTERN_CODE_DIM; i++) {
        dot += a->code[i] * b->code[i];
        na += a->code[i] * a->code[i];
        nb += b->code[i] * b->code[i];
    }
    if (na < 1e-12 || nb < 1e-12) return 0;
    return dot / (sqrt(na) * sqrt(nb) + 1e-12);
}

static int find_edge_by_name(const CnetPatternRuntime *rt, const char *name) {
    size_t i;
    if (!rt || !name) return -1;
    for (i = 0; i < rt->edge_count; i++)
        if (strcmp(rt->edges[i].name, name) == 0) return (int)i;
    return -1;
}

int cnet_pattern_propose(CnetPatternRuntime *rt, const char *name,
                         const CnetPatternAddr *in_addr,
                         const CnetPatternAddr *out_addr, Port in_port,
                         Port out_port, CnetPatternBodyKind body_kind,
                         const char *body_ref) {
    int idx;
    CnetPatternEdge *e;
    if (!rt || !name || !name[0] || !in_addr || !out_addr) return -1;
    idx = find_edge_by_name(rt, name);
    if (idx >= 0) {
        e = &rt->edges[idx];
        if (e->state == CNET_PAT_STATE_DEMOTED) e->state = CNET_PAT_STATE_FLUID;
        e->in_addr = *in_addr;
        e->out_addr = *out_addr;
        e->in_port = in_port;
        e->out_port = out_port;
        e->body_kind = body_kind;
        if (body_ref)
            snprintf(e->body_ref, sizeof e->body_ref, "%s", body_ref);
        e->in_addr.version++;
        e->out_addr.version = e->in_addr.version;
        return idx;
    }
    if (rt->edge_count >= CNET_PATTERN_MAX_EDGES) return -1;
    idx = (int)rt->edge_count++;
    e = &rt->edges[idx];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    e->in_addr = *in_addr;
    e->out_addr = *out_addr;
    e->in_port = in_port;
    e->out_port = out_port;
    e->state = CNET_PAT_STATE_FLUID;
    e->body_kind = body_kind;
    if (body_ref) snprintf(e->body_ref, sizeof e->body_ref, "%s", body_ref);
    e->created_unix = (uint64_t)time(NULL);
    e->reliability = 0;
    e->resident = 0;
    return idx;
}

static void recompute_reliability(CnetPatternEdge *e) {
    uint32_t t;
    if (!e) return;
    t = e->successes + e->failures;
    e->reliability = t ? (double)e->successes / (double)t : 0.0;
}

/* Forward decls for mutual use with residency helpers */
int cnet_pattern_freeze(CnetPatternRuntime *rt, int edge_index, int force);
int cnet_pattern_demote(CnetPatternRuntime *rt, int edge_index);
int cnet_pattern_unload(CnetPatternRuntime *rt, int edge_index);

int cnet_pattern_feedback(CnetPatternRuntime *rt, int edge_index, int success) {
    CnetPatternEdge *e;
    if (!rt || edge_index < 0 || (size_t)edge_index >= rt->edge_count) return -1;
    e = &rt->edges[edge_index];
    if (success)
        e->successes++;
    else
        e->failures++;
    recompute_reliability(e);
    e->heat++;
    e->last_use = rt->clock;
    if (success) {
        if (e->state == CNET_PAT_STATE_FLUID && e->successes >= 1)
            e->state = CNET_PAT_STATE_IMPROVING;
        if (e->state == CNET_PAT_STATE_IMPROVING &&
            e->successes >= rt->freeze_min_successes &&
            e->reliability + 1e-12 >= rt->freeze_min_reliability)
            cnet_pattern_freeze(rt, edge_index, 0);
    } else if (e->state == CNET_PAT_STATE_FROZEN && e->reliability < 0.5) {
        cnet_pattern_demote(rt, edge_index);
    }
    return 0;
}

int cnet_pattern_freeze(CnetPatternRuntime *rt, int edge_index, int force) {
    CnetPatternEdge *e;
    if (!rt || edge_index < 0 || (size_t)edge_index >= rt->edge_count) return -1;
    e = &rt->edges[edge_index];
    if (!force) {
        if (e->successes < rt->freeze_min_successes) return 1;
        if (e->reliability + 1e-12 < rt->freeze_min_reliability) return 1;
        if (e->state == CNET_PAT_STATE_DEMOTED) return 1;
    }
    e->state = CNET_PAT_STATE_FROZEN;
    e->frozen_unix = (uint64_t)time(NULL);
    e->out_addr.version++;
    e->in_addr.version = e->out_addr.version;
    return 0;
}

int cnet_pattern_demote(CnetPatternRuntime *rt, int edge_index) {
    CnetPatternEdge *e;
    if (!rt || edge_index < 0 || (size_t)edge_index >= rt->edge_count) return -1;
    e = &rt->edges[edge_index];
    e->state = CNET_PAT_STATE_DEMOTED;
    if (e->resident) cnet_pattern_unload(rt, edge_index);
    return 0;
}

static int hot_find(const CnetPatternRuntime *rt, int edge_index) {
    int i;
    for (i = 0; i < rt->hot_count; i++)
        if (rt->hot[i] == edge_index) return i;
    return -1;
}

int cnet_pattern_unload(CnetPatternRuntime *rt, int edge_index) {
    int hi;
    if (!rt || edge_index < 0 || (size_t)edge_index >= rt->edge_count) return -1;
    hi = hot_find(rt, edge_index);
    if (hi < 0) return 0;
    rt->edges[edge_index].resident = 0;
    rt->edges[edge_index].in_addr.place_slot = UINT32_MAX;
    rt->hot[hi] = rt->hot[rt->hot_count - 1];
    rt->hot_count--;
    return 0;
}

int cnet_pattern_load(CnetPatternRuntime *rt, int edge_index) {
    int hi;
    if (!rt || edge_index < 0 || (size_t)edge_index >= rt->edge_count) return -1;
    if (rt->edges[edge_index].state == CNET_PAT_STATE_DEMOTED) return -1;
    hi = hot_find(rt, edge_index);
    if (hi >= 0) {
        rt->edges[edge_index].heat++;
        rt->edges[edge_index].last_use = rt->clock;
        return 0;
    }
    /* need a slot */
    if (rt->hot_count >= rt->hot_cap) {
        /* LFRU victim among hot */
        uint32_t heat[CNET_PATTERN_MAX_EDGES];
        uint32_t last[CNET_PATTERN_MAX_EDGES];
        size_t i;
        int vict;
        for (i = 0; i < rt->edge_count; i++) {
            heat[i] = rt->edges[i].heat;
            last[i] = rt->edges[i].last_use;
        }
        vict = cnet_lfru_pick_victim(heat, last, rt->clock, rt->hot, rt->hot_count);
        if (vict >= 0) cnet_pattern_unload(rt, rt->hot[vict]);
    }
    if (rt->hot_count >= rt->hot_cap) return -1;
    rt->hot[rt->hot_count++] = edge_index;
    rt->edges[edge_index].resident = 1;
    rt->edges[edge_index].in_addr.place_slot = (uint32_t)(rt->hot_count - 1);
    rt->edges[edge_index].heat++;
    rt->edges[edge_index].last_use = rt->clock;
    return 0;
}

void cnet_pattern_tick(CnetPatternRuntime *rt) {
    size_t i;
    if (!rt) return;
    rt->clock++;
    if ((rt->clock & 63u) == 0) {
        for (i = 0; i < rt->edge_count; i++) rt->edges[i].heat >>= 1;
    }
}

int cnet_pattern_find(const CnetPatternRuntime *rt, const char *query,
                      const char *role) {
    CnetPatternAddr qaddr;
    size_t i;
    int best = -1;
    double best_score = -1;
    if (!rt || !query) return -1;
    cnet_pattern_addr_from_text(query, role ? role : "query", &qaddr);
    for (i = 0; i < rt->edge_count; i++) {
        const CnetPatternEdge *e = &rt->edges[i];
        double sim, score;
        int state_w;
        if (e->state == CNET_PAT_STATE_DEMOTED) continue;
        /* Exact unit / edge name match wins */
        if (strcmp(e->name, query) == 0 ||
            (e->body_ref[0] && strcmp(e->body_ref, query) == 0) ||
            (strncmp(e->name, "unit:", 5) == 0 && strcmp(e->name + 5, query) == 0)) {
            best = (int)i;
            best_score = 1e9;
            continue;
        }
        sim = addr_similarity(&qaddr, &e->in_addr);
        if (sim < 0.55) {
            if (!strstr(e->name, query) && !(e->body_ref[0] && strstr(query, e->name)))
                continue;
            sim = 0.7;
        }
        switch (e->state) {
        case CNET_PAT_STATE_FROZEN: state_w = 4; break;
        case CNET_PAT_STATE_IMPROVING: state_w = 2; break;
        case CNET_PAT_STATE_FLUID: state_w = 1; break;
        default: state_w = 0; break;
        }
        score = sim * 10.0 + state_w + e->reliability * 2.0 +
                (e->resident ? 0.5 : 0.0);
        if (score > best_score) {
            best_score = score;
            best = (int)i;
        }
    }
    return best;
}

static int exec_body(CnetPatternRuntime *rt, CnetPatternEdge *e, const char *query,
                     char *answer, size_t acap, char *detail, size_t dcap) {
    if (!e || !answer || acap == 0) return -1;
    answer[0] = '\0';
    if (detail && dcap) detail[0] = '\0';

    switch (e->body_kind) {
    case CNET_PAT_BODY_HERMETIC: {
        snprintf(answer, acap, "HERMETIC_OK:%s", e->name);
        if (detail) snprintf(detail, dcap, "hermetic callout");
        return 0;
    }
    case CNET_PAT_BODY_MATH: {
        CnetMathSolveConfig cfg;
        CnetMathSolveReport rep;
        cnet_math_solve_config_from_env(&cfg);
        snprintf(cfg.skills_dir, sizeof cfg.skills_dir, "%s", rt->skills_dir);
        cfg.write_skill = 0;
        if (cnet_math_solve(query, &cfg, &rep) == 0 && rep.verified) {
            snprintf(answer, acap, "%s", rep.answer);
            if (detail)
                snprintf(detail, dcap, "math %s tier=%s", rep.method,
                         cnet_math_tier_name(rep.tier));
            return 0;
        }
        if (detail) snprintf(detail, dcap, "math abstain: %s", rep.detail);
        return 1;
    }
    case CNET_PAT_BODY_TOOL: {
        int from = 0;
        if (strcmp(e->body_ref, "math_eval") == 0 ||
            strcmp(e->body_ref, "calculator") == 0) {
            if (port_contract_mcp_math_eval(query, answer, acap, &from) != 0 ||
                strcmp(answer, "EVAL_ERROR") == 0)
                return 1;
            if (detail)
                snprintf(detail, dcap, "tool math_eval cache=%d", from);
            return 0;
        }
        if (strcmp(e->body_ref, "wiki_lookup") == 0 ||
            strcmp(e->body_ref, "wiki") == 0) {
            if (port_contract_mcp_wiki_lookup(query, answer, acap, &from) != 0)
                return 1;
            if (!answer[0] || strcmp(answer, "LOOKUP_FAILED") == 0) return 1;
            if (detail) snprintf(detail, dcap, "tool wiki cache=%d", from);
            return 0;
        }
        if (strcmp(e->body_ref, "web_search") == 0 ||
            strcmp(e->body_ref, "web") == 0) {
            if (port_contract_mcp_web_search(query, answer, acap, &from) != 0)
                return 1;
            if (!answer[0] || strncmp(answer, "No useful", 9) == 0) return 1;
            if (detail) snprintf(detail, dcap, "tool web cache=%d", from);
            return 0;
        }
        if (detail) snprintf(detail, dcap, "unknown tool %s", e->body_ref);
        return 1;
    }
    case CNET_PAT_BODY_SKILL: {
        FILE *f;
        char line[512];
        int in_ans = 0;
        size_t used = 0;
        const char *path = e->body_ref;
        f = fopen(path, "r");
        if (!f) return 1;
        answer[0] = '\0';
        while (fgets(line, sizeof line, f)) {
            if (strncmp(line, "## Answer", 9) == 0) {
                in_ans = 1;
                continue;
            }
            if (in_ans && line[0] == '#' && line[1] == '#') break;
            if (in_ans) {
                size_t ln = strlen(line);
                if (used + ln + 1 < acap) {
                    memcpy(answer + used, line, ln);
                    used += ln;
                    answer[used] = '\0';
                }
            }
        }
        fclose(f);
        if (used < 1) return 1;
        if (detail) snprintf(detail, dcap, "skill file");
        return 0;
    }
    case CNET_PAT_BODY_UNIT: {
        void *h = rt->soul_host;
        int in_t = 0, out_t = 0, rc, k;
        double *in_buf = NULL, *out_buf = NULL;
        if (!h && rt->base_path[0]) {
            if (cnet_pattern_bind_base(rt, rt->base_path) == 0) h = rt->soul_host;
        }
        if (!h || !g_soul_run || !g_soul_unit_dims) {
            if (detail)
                snprintf(detail, dcap,
                         "unit %s: SoulHost not bound (cnet.so + CNET_BASE_PATH)",
                         e->body_ref);
            snprintf(answer, acap, "UNIT_UNBOUND:%s", e->body_ref);
            return 1;
        }
        if (g_soul_unit_dims(h, e->body_ref, &in_t, &out_t) != 0) {
            if (detail) snprintf(detail, dcap, "unit %s: dims fail", e->body_ref);
            return 1;
        }
        if (in_t < 0 || out_t < 0 || in_t > 65536 || out_t > 65536) {
            if (detail)
                snprintf(detail, dcap, "unit %s: dims out of range in=%d out=%d",
                         e->body_ref, in_t, out_t);
            return 1;
        }
        in_buf = (double *)calloc((size_t)in_t + 1, sizeof(double));
        out_buf = (double *)calloc((size_t)out_t + 1, sizeof(double));
        if (!in_buf || !out_buf) {
            free(in_buf);
            free(out_buf);
            return 1;
        }
        /* Valid one-hot / raw probe: canonical first-basis input.
         * (Random codes fail port_validate on ONEHOT units → soul_run -4.) */
        for (k = 0; k < in_t; k++) in_buf[k] = 0.0;
        if (in_t > 0) in_buf[0] = 1.0;
        rc = g_soul_run(h, e->body_ref, in_buf, out_buf, out_t > 0 ? out_t : 1);
        if (rc < 0) {
            if (detail)
                snprintf(detail, dcap, "unit %s: soul_run rc=%d", e->body_ref, rc);
            free(in_buf);
            free(out_buf);
            return 1;
        }
        {
            size_t used = 0;
            int n = rc < 8 ? rc : 8;
            used = (size_t)snprintf(answer, acap, "UNIT:%s", e->body_ref);
            for (k = 0; k < n && used + 24 < acap; k++) {
                int w = snprintf(answer + used, acap - used, " %.6g", out_buf[k]);
                if (w > 0) used += (size_t)w;
            }
        }
        if (detail)
            snprintf(detail, dcap, "soul_run certified unit out_total=%d", rc);
        free(in_buf);
        free(out_buf);
        return 0;
    }
    default:
        return 1;
    }
}

int cnet_pattern_callout(CnetPatternRuntime *rt, const char *query, const char *role,
                         CnetPatternCalloutReport *rep) {
    int idx, rc, loaded = 0, froze_before;
    CnetPatternEdge *e;
    if (rep) memset(rep, 0, sizeof *rep);
    if (!rt || !query) return -1;

    idx = cnet_pattern_find(rt, query, role);
    if (idx < 0) {
        /* no edge: try math propose on the fly (fluid learning) */
        CnetMathSolveConfig cfg;
        CnetMathSolveReport mr;
        cnet_math_solve_config_from_env(&cfg);
        snprintf(cfg.skills_dir, sizeof cfg.skills_dir, "%s", rt->skills_dir);
        cfg.write_skill = 1;
        if (cnet_math_solve(query, &cfg, &mr) == 0 && mr.verified) {
            cnet_pattern_observe_math(rt, query, mr.method, mr.answer, 1);
            idx = cnet_pattern_find(rt, query, role ? role : "math");
            if (rep) {
                rep->found = 1;
                snprintf(rep->answer, sizeof rep->answer, "%s", mr.answer);
                snprintf(rep->detail, sizeof rep->detail, "on-the-fly math → pattern");
                rep->body_kind = CNET_PAT_BODY_MATH;
                rep->state = CNET_PAT_STATE_FLUID;
            }
            if (idx >= 0) cnet_pattern_feedback(rt, idx, 1);
            return 0;
        }
        if (rep) snprintf(rep->detail, sizeof rep->detail, "no pattern edge");
        return 1;
    }

    e = &rt->edges[idx];
    froze_before = (e->state == CNET_PAT_STATE_FROZEN);
    if (cnet_pattern_load(rt, idx) == 0) loaded = 1;
    cnet_pattern_tick(rt);

    if (rep) {
        rep->found = 1;
        rep->edge_index = idx;
        rep->state = e->state;
        rep->body_kind = e->body_kind;
        snprintf(rep->body_ref, sizeof rep->body_ref, "%s", e->body_ref);
        rep->loaded = loaded;
        rep->reliability = e->reliability;
    }

    rc = exec_body(rt, e, query, rep ? rep->answer : NULL,
                   rep ? sizeof rep->answer : 0, rep ? rep->detail : NULL,
                   rep ? sizeof rep->detail : 0);
    if (rc == 0) {
        cnet_pattern_feedback(rt, idx, 1);
        if (rep && !froze_before && e->state == CNET_PAT_STATE_FROZEN) rep->froze = 1;
        if (rep) {
            rep->state = e->state;
            rep->reliability = e->reliability;
        }
        return 0;
    }
    cnet_pattern_feedback(rt, idx, 0);
    if (rep) {
        rep->state = e->state;
        if (!rep->detail[0])
            snprintf(rep->detail, sizeof rep->detail, "callout body failed");
    }
    return 1;
}

int cnet_pattern_observe_math(CnetPatternRuntime *rt, const char *question,
                              const char *method, const char *answer, int verified) {
    CnetPatternAddr in_a, out_a;
    Port in_p, out_p;
    char name[CNET_PATTERN_NAME_MAX];
    int idx;
    if (!rt || !question || !verified) return -1;
    (void)answer;
    cnet_pattern_addr_from_text(question, "math", &in_a);
    cnet_pattern_addr_from_text(method ? method : "math_solve", "math_out", &out_a);
    port_raw_tag(&in_p, "math_q", CNET_PATTERN_CODE_DIM);
    port_raw_tag(&out_p, "math_a", CNET_PATTERN_CODE_DIM);
    snprintf(name, sizeof name, "math:%.48s", method ? method : "solve");
    /* unique-ish per method family; also propose query-specific */
    idx = cnet_pattern_propose(rt, name, &in_a, &out_a, in_p, out_p, CNET_PAT_BODY_MATH,
                               method ? method : "math_solve");
    if (idx >= 0) cnet_pattern_feedback(rt, idx, 1);
    {
        char qname[CNET_PATTERN_NAME_MAX];
        uint64_t h = fnv1a64(question);
        snprintf(qname, sizeof qname, "q:%08x", (unsigned)(h & 0xffffffffu));
        idx = cnet_pattern_propose(rt, qname, &in_a, &out_a, in_p, out_p,
                                   CNET_PAT_BODY_MATH, method ? method : "math_solve");
        if (idx >= 0) cnet_pattern_feedback(rt, idx, 1);
    }
    return 0;
}

int cnet_pattern_observe_skill(CnetPatternRuntime *rt, const char *query,
                               const char *skill_path, int success) {
    CnetPatternAddr in_a, out_a;
    Port in_p, out_p;
    char name[CNET_PATTERN_NAME_MAX];
    int idx;
    if (!rt || !query || !skill_path) return -1;
    cnet_pattern_addr_from_text(query, "skill", &in_a);
    cnet_pattern_addr_from_text(skill_path, "skill_out", &out_a);
    port_raw_tag(&in_p, "skill_q", CNET_PATTERN_CODE_DIM);
    port_raw_tag(&out_p, "skill_a", CNET_PATTERN_CODE_DIM);
    snprintf(name, sizeof name, "skill:%08x", (unsigned)(fnv1a64(query) & 0xffffffffu));
    idx = cnet_pattern_propose(rt, name, &in_a, &out_a, in_p, out_p, CNET_PAT_BODY_SKILL,
                               skill_path);
    if (idx >= 0) cnet_pattern_feedback(rt, idx, success ? 1 : 0);
    return idx;
}

int cnet_pattern_bootstrap_defaults(CnetPatternRuntime *rt) {
    CnetPatternAddr in_a, out_a;
    Port in_p, out_p;
    int n = 0, idx;
    if (!rt) return -1;
    port_raw_tag(&in_p, "query", CNET_PATTERN_CODE_DIM);
    port_raw_tag(&out_p, "answer", CNET_PATTERN_CODE_DIM);

    cnet_pattern_addr_from_text("hermetic_ping", "test", &in_a);
    cnet_pattern_addr_from_text("hermetic_pong", "test", &out_a);
    idx = cnet_pattern_propose(rt, "hermetic_ping", &in_a, &out_a, in_p, out_p,
                               CNET_PAT_BODY_HERMETIC, "ping");
    if (idx >= 0) {
        cnet_pattern_feedback(rt, idx, 1);
        cnet_pattern_feedback(rt, idx, 1);
        cnet_pattern_feedback(rt, idx, 1);
        n++;
    }

    cnet_pattern_addr_from_text("math_bridge", "math", &in_a);
    cnet_pattern_addr_from_text("math_result", "math", &out_a);
    idx = cnet_pattern_propose(rt, "bridge_math", &in_a, &out_a, in_p, out_p,
                               CNET_PAT_BODY_MATH, "math_solve");
    if (idx >= 0) n++;

    cnet_pattern_addr_from_text("math_eval_bridge", "expr", &in_a);
    idx = cnet_pattern_propose(rt, "bridge_math_eval", &in_a, &out_a, in_p, out_p,
                               CNET_PAT_BODY_TOOL, "math_eval");
    if (idx >= 0) n++;

    cnet_pattern_addr_from_text("wiki_bridge", "lookup", &in_a);
    idx = cnet_pattern_propose(rt, "bridge_wiki", &in_a, &out_a, in_p, out_p,
                               CNET_PAT_BODY_TOOL, "wiki_lookup");
    if (idx >= 0) n++;

    return n;
}

size_t cnet_pattern_count(const CnetPatternRuntime *rt) {
    return rt ? rt->edge_count : 0;
}

size_t cnet_pattern_count_state(const CnetPatternRuntime *rt, CnetPatternState s) {
    size_t i, n = 0;
    if (!rt) return 0;
    for (i = 0; i < rt->edge_count; i++)
        if (rt->edges[i].state == s) n++;
    return n;
}

/* --- persistence: simple JSONL --- */

static int mkdir_parent(const char *path) {
    char tmp[CNET_PATTERN_PATH_MAX];
    size_t i, len;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            cnet_mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return 0;
}

int cnet_pattern_runtime_save(const CnetPatternRuntime *rt, const char *path) {
    FILE *f;
    size_t i;
    const char *p = path ? path : (rt ? rt->store_path : NULL);
    if (!rt || !p || !p[0]) return -1;
    mkdir_parent(p);
    f = fopen(p, "w");
    if (!f) return -1;
    fprintf(f, "{\"type\":\"meta\",\"edges\":%zu,\"clock\":%u,\"hot_cap\":%d}\n",
            rt->edge_count, rt->clock, rt->hot_cap);
    for (i = 0; i < rt->edge_count; i++) {
        const CnetPatternEdge *e = &rt->edges[i];
        fprintf(f,
                "{\"type\":\"edge\",\"name\":\"%s\",\"state\":\"%s\",\"body\":\"%s\","
                "\"ref\":\"%s\",\"role\":\"%s\",\"succ\":%u,\"fail\":%u,\"rel\":%.6f,"
                "\"heat\":%u,\"ver\":%u}\n",
                e->name, cnet_pattern_state_name(e->state),
                cnet_pattern_body_name(e->body_kind), e->body_ref, e->in_addr.role,
                e->successes, e->failures, e->reliability, e->heat, e->in_addr.version);
    }
    fclose(f);
    return 0;
}

int cnet_pattern_runtime_load(CnetPatternRuntime *rt, const char *path) {
    FILE *f;
    char line[1024];
    const char *p = path ? path : (rt ? rt->store_path : NULL);
    if (!rt || !p || !p[0]) return -1;
    f = fopen(p, "r");
    if (!f) return 1; /* missing ok */
    while (fgets(line, sizeof line, f)) {
        char name[64] = "", state[32] = "", body[32] = "", ref[256] = "", role[32] = "query";
        unsigned succ = 0, fail = 0, heat = 0, ver = 1;
        double rel = 0;
        if (strstr(line, "\"type\":\"meta\"")) continue;
        if (!strstr(line, "\"type\":\"edge\"")) continue;
        /* minimal parse */
        {
            char *s;
            if ((s = strstr(line, "\"name\":\"")))
                sscanf(s, "\"name\":\"%63[^\"]\"", name);
            if ((s = strstr(line, "\"state\":\"")))
                sscanf(s, "\"state\":\"%31[^\"]\"", state);
            if ((s = strstr(line, "\"body\":\"")))
                sscanf(s, "\"body\":\"%31[^\"]\"", body);
            if ((s = strstr(line, "\"ref\":\"")))
                sscanf(s, "\"ref\":\"%255[^\"]\"", ref);
            if ((s = strstr(line, "\"role\":\"")))
                sscanf(s, "\"role\":\"%31[^\"]\"", role);
            if ((s = strstr(line, "\"succ\":"))) sscanf(s, "\"succ\":%u", &succ);
            if ((s = strstr(line, "\"fail\":"))) sscanf(s, "\"fail\":%u", &fail);
            if ((s = strstr(line, "\"rel\":"))) sscanf(s, "\"rel\":%lf", &rel);
            if ((s = strstr(line, "\"heat\":"))) sscanf(s, "\"heat\":%u", &heat);
            if ((s = strstr(line, "\"ver\":"))) sscanf(s, "\"ver\":%u", &ver);
        }
        if (!name[0]) continue;
        {
            CnetPatternAddr in_a, out_a;
            Port in_p, out_p;
            CnetPatternBodyKind bk = CNET_PAT_BODY_NONE;
            int idx;
            cnet_pattern_addr_from_text(name, role, &in_a);
            cnet_pattern_addr_from_text(ref[0] ? ref : name, role, &out_a);
            in_a.version = ver;
            out_a.version = ver;
            port_raw_tag(&in_p, role, CNET_PATTERN_CODE_DIM);
            port_raw_tag(&out_p, "answer", CNET_PATTERN_CODE_DIM);
            if (strcmp(body, "math") == 0) bk = CNET_PAT_BODY_MATH;
            else if (strcmp(body, "tool") == 0) bk = CNET_PAT_BODY_TOOL;
            else if (strcmp(body, "skill") == 0) bk = CNET_PAT_BODY_SKILL;
            else if (strcmp(body, "unit") == 0) bk = CNET_PAT_BODY_UNIT;
            else if (strcmp(body, "hermetic") == 0) bk = CNET_PAT_BODY_HERMETIC;
            idx = cnet_pattern_propose(rt, name, &in_a, &out_a, in_p, out_p, bk, ref);
            if (idx >= 0) {
                CnetPatternEdge *e = &rt->edges[idx];
                e->successes = succ;
                e->failures = fail;
                e->reliability = rel;
                e->heat = heat;
                if (strcmp(state, "frozen") == 0) e->state = CNET_PAT_STATE_FROZEN;
                else if (strcmp(state, "improving") == 0)
                    e->state = CNET_PAT_STATE_IMPROVING;
                else if (strcmp(state, "demoted") == 0)
                    e->state = CNET_PAT_STATE_DEMOTED;
                else
                    e->state = CNET_PAT_STATE_FLUID;
            }
        }
    }
    fclose(f);
    return 0;
}
