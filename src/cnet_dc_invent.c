#include "../include/cnet_dc_invent.h"

#include <stdio.h>
#include <string.h>

void cnet_dc_grammar_init(CnetDcGrammar *g) {
    if (g == NULL) return;
    memset(g, 0, sizeof *g);
}

int cnet_dc_grammar_add(CnetDcGrammar *g, const char *name, int type,
                        int invented) {
    CnetDcPrim *p;
    if (g == NULL || name == NULL || name[0] == '\0' || type < 0) return -1;
    if (g->n_prims >= CNET_DC_MAX_PRIMS) return -1;
    p = &g->prims[g->n_prims];
    snprintf(p->name, sizeof p->name, "%s", name);
    p->type = type;
    p->invented = invented ? 1 : 0;
    g->n_prims++;
    return 0;
}

static int term_uses_invented(const CnetDcGrammar *g, const char *text) {
    int i;
    if (g == NULL || text == NULL) return 0;
    for (i = 0; i < g->n_prims; ++i) {
        if (g->prims[i].invented && strstr(text, g->prims[i].name) != NULL)
            return 1;
    }
    return 0;
}

static int already(const CnetDcTerm *pool, int n, const char *text) {
    int i;
    for (i = 0; i < n; ++i) {
        if (strcmp(pool[i].text, text) == 0) return 1;
    }
    return 0;
}

static int add_term(CnetDcTerm *pool, int *n, int cap, const char *text,
                    int type, int depth, int invented) {
    CnetDcTerm *t;
    if (pool == NULL || n == NULL || text == NULL || type < 0) return -1;
    if (*n >= cap) return 0;
    if (already(pool, *n, text)) return 0;
    t = &pool[*n];
    snprintf(t->text, sizeof t->text, "%s", text);
    t->type = type;
    t->depth = depth;
    t->used_invented = invented ? 1 : 0;
    (*n)++;
    return 0;
}

static int generate_pool(CnetDcArena *arena, const CnetDcGrammar *g,
                         int max_depth, CnetDcTerm *pool, int cap, int *n) {
    int i, f, a, added, guard;
    char text[CNET_DC_TERM_MAX];
    int ret, depth, invented;
    if (arena == NULL || g == NULL || pool == NULL || n == NULL) return -1;
    *n = 0;
    if (max_depth < 0) max_depth = 0;
    for (i = 0; i < g->n_prims; ++i) {
        if (add_term(pool, n, cap, g->prims[i].name, g->prims[i].type, 0,
                     g->prims[i].invented) != 0)
            return -1;
    }
    for (guard = 0; guard < CNET_DC_MAX_POOL; ++guard) {
        int before = *n;
        for (f = 0; f < before; ++f) {
            if (!cnet_dc_is_arrow(arena, pool[f].type)) continue;
            for (a = 0; a < before; ++a) {
                if (pool[f].depth >= max_depth) continue;
                if (cnet_dc_apply_fn(arena, pool[f].type, pool[a].type, &ret) !=
                    0)
                    continue;
                depth = pool[f].depth + 1;
                if (pool[a].depth + 1 > depth) depth = pool[a].depth + 1;
                if (depth > max_depth) continue;
                if (snprintf(text, sizeof text, "(%s %s)", pool[f].text,
                             pool[a].text) >= (int)sizeof text)
                    continue;
                invented = pool[f].used_invented || pool[a].used_invented ||
                           term_uses_invented(g, text);
                if (add_term(pool, n, cap, text, ret, depth, invented) != 0)
                    return -1;
                if (*n >= cap) return 0;
            }
        }
        added = *n - before;
        if (added == 0) break;
    }
    return 0;
}

int cnet_dc_wake(CnetDcArena *arena, const CnetDcGrammar *g, int goal,
                 int max_depth, CnetDcTerm *hits, int cap, int *n_hits) {
    CnetDcTerm pool[CNET_DC_MAX_POOL];
    int n_pool = 0, i;
    if (n_hits == NULL) return -1;
    *n_hits = 0;
    if (arena == NULL || g == NULL || hits == NULL || cap <= 0 || goal < 0)
        return -1;
    if (generate_pool(arena, g, max_depth, pool, CNET_DC_MAX_POOL, &n_pool) !=
        0)
        return -1;
    for (i = 0; i < n_pool && *n_hits < cap; ++i) {
        if (!cnet_dc_can_unify(arena, pool[i].type, goal)) continue;
        hits[*n_hits] = pool[i];
        (*n_hits)++;
    }
    return 0;
}

int cnet_dc_sleep_invent(CnetDcGrammar *g, const char *name,
                         const CnetDcTerm *term) {
    int i;
    if (g == NULL || name == NULL || name[0] == '\0' || term == NULL ||
        term->text[0] == '\0' || term->type < 0)
        return -1;
    for (i = 0; i < g->n_prims; ++i) {
        if (strcmp(g->prims[i].name, name) == 0) return -1;
    }
    return cnet_dc_grammar_add(g, name, term->type, 1);
}

static unsigned next_rng(unsigned *rng) {
    unsigned x = (rng != NULL && *rng != 0) ? *rng : 1u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    if (rng != NULL) *rng = x;
    return x;
}

int cnet_dc_dream(CnetDcArena *arena, const CnetDcGrammar *g, unsigned *rng,
                  int max_depth, CnetDcTerm *out) {
    CnetDcTerm pool[CNET_DC_MAX_POOL];
    int n_pool = 0;
    unsigned pick;
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (arena == NULL || g == NULL || g->n_prims == 0) return 1;
    if (generate_pool(arena, g, max_depth, pool, CNET_DC_MAX_POOL, &n_pool) !=
        0)
        return -1;
    if (n_pool <= 0) return 1;
    pick = next_rng(rng) % (unsigned)n_pool;
    *out = pool[pick];
    return 0;
}

int cnet_dc_speak_bound(const CnetDcTerm *term, const char *value, char *out,
                        size_t cap) {
    int written;
    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (term == NULL || term->text[0] == '\0' || value == NULL ||
        value[0] == '\0')
        return -1;
    written = snprintf(out, cap, "Marble reports %s via %s (%s).", value,
                       term->text, CNET_DC_INVENT_CONTRACT);
    if (written < 0 || (size_t)written >= cap) {
        out[0] = '\0';
        return -1;
    }
    return 0;
}
