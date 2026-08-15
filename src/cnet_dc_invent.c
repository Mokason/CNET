#include "../include/cnet_dc_invent.h"

#include <stdio.h>
#include <stdlib.h>
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
    p->body[0] = '\0';
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
    if (cnet_dc_grammar_add(g, name, term->type, 1) != 0) return -1;
    snprintf(g->prims[g->n_prims - 1].body,
             sizeof g->prims[g->n_prims - 1].body, "%s", term->text);
    return 0;
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

void cnet_dc_value_int(CnetDcValue *v, long n) {
    if (v == NULL) return;
    memset(v, 0, sizeof *v);
    v->kind = CNET_DC_VAL_INT;
    v->num = n;
}

void cnet_dc_value_list(CnetDcValue *v, const long *items, int n) {
    int i;
    if (v == NULL) return;
    memset(v, 0, sizeof *v);
    v->kind = CNET_DC_VAL_LIST;
    if (n < 0) n = 0;
    if (n > CNET_DC_LIST_MAX) n = CNET_DC_LIST_MAX;
    v->n_items = n;
    for (i = 0; i < n && items != NULL; ++i) v->items[i] = items[i];
}

int cnet_dc_value_equal(const CnetDcValue *a, const CnetDcValue *b) {
    int i;
    if (a == NULL || b == NULL || a->kind != b->kind) return 0;
    if (a->kind == CNET_DC_VAL_INT) return a->num == b->num;
    if (a->kind == CNET_DC_VAL_LIST) {
        if (a->n_items != b->n_items) return 0;
        for (i = 0; i < a->n_items; ++i) {
            if (a->items[i] != b->items[i]) return 0;
        }
        return 1;
    }
    if (a->kind == CNET_DC_VAL_FN_CONS1)
        return a->num == b->num;
    return a->kind == b->kind && a->kind != CNET_DC_VAL_NONE;
}

static const char *skip_ws(const char *s) {
    while (s != NULL && (*s == ' ' || *s == '\t')) ++s;
    return s;
}

static int apply_value(const CnetDcValue *fn, const CnetDcValue *arg,
                       CnetDcValue *out) {
    if (fn == NULL || arg == NULL || out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (fn->kind == CNET_DC_VAL_FN_INCR) {
        if (arg->kind != CNET_DC_VAL_INT) return 1;
        cnet_dc_value_int(out, arg->num + 1);
        return 0;
    }
    if (fn->kind == CNET_DC_VAL_FN_CONS) {
        if (arg->kind != CNET_DC_VAL_INT) return 1;
        memset(out, 0, sizeof *out);
        out->kind = CNET_DC_VAL_FN_CONS1;
        out->num = arg->num;
        return 0;
    }
    if (fn->kind == CNET_DC_VAL_FN_CONS1) {
        if (arg->kind != CNET_DC_VAL_LIST) return 1;
        if (arg->n_items >= CNET_DC_LIST_MAX) return 1;
        cnet_dc_value_list(out, arg->items, arg->n_items);
        memmove(out->items + 1, out->items, (size_t)out->n_items * sizeof out->items[0]);
        out->items[0] = fn->num;
        out->n_items++;
        return 0;
    }
    return 1;
}

static int eval_atom(const CnetDcGrammar *g, const char *name, CnetDcValue *out);
static int eval_rec(const CnetDcGrammar *g, const char *s, const char **end,
                    CnetDcValue *out);

static int eval_atom(const CnetDcGrammar *g, const char *name, CnetDcValue *out) {
    int i;
    if (g == NULL || name == NULL || out == NULL) return -1;
    memset(out, 0, sizeof *out);
    for (i = 0; i < g->n_prims; ++i) {
        if (strcmp(g->prims[i].name, name) != 0) continue;
        if (g->prims[i].body[0] != '\0')
            return cnet_dc_eval_term(g, g->prims[i].body, out);
        break;
    }
    if (strcmp(name, "zero") == 0) {
        cnet_dc_value_int(out, 0);
        return 0;
    }
    if (strcmp(name, "incr") == 0) {
        out->kind = CNET_DC_VAL_FN_INCR;
        return 0;
    }
    if (strcmp(name, "empty_int") == 0 || strcmp(name, "empty") == 0) {
        cnet_dc_value_list(out, NULL, 0);
        return 0;
    }
    if (strcmp(name, "cons") == 0) {
        out->kind = CNET_DC_VAL_FN_CONS;
        return 0;
    }
    return 1;
}

static int eval_rec(const CnetDcGrammar *g, const char *s, const char **end,
                    CnetDcValue *out) {
    CnetDcValue fn, arg;
    char atom[CNET_DC_NAME_MAX];
    int n = 0;
    s = skip_ws(s);
    if (s == NULL || *s == '\0') return 1;
    if (*s == '(') {
        int rc;
        ++s;
        rc = eval_rec(g, s, &s, &fn);
        if (rc != 0) return rc;
        rc = eval_rec(g, s, &s, &arg);
        if (rc != 0) return rc;
        s = skip_ws(s);
        if (*s != ')') return 1;
        ++s;
        if (end != NULL) *end = s;
        return apply_value(&fn, &arg, out);
    }
    while (s[n] != '\0' && s[n] != ' ' && s[n] != '\t' && s[n] != '(' &&
           s[n] != ')') {
        if (n + 1 >= (int)sizeof atom) return 1;
        atom[n] = s[n];
        n++;
    }
    if (n == 0) return 1;
    atom[n] = '\0';
    if (end != NULL) *end = s + n;
    return eval_atom(g, atom, out);
}

int cnet_dc_eval_term(const CnetDcGrammar *g, const char *text,
                      CnetDcValue *out) {
    const char *end = NULL;
    int rc;
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (g == NULL || text == NULL || text[0] == '\0') return -1;
    rc = eval_rec(g, text, &end, out);
    if (rc != 0) return rc;
    end = skip_ws(end);
    return (end != NULL && *end == '\0') ? 0 : 1;
}

static int term_matches_io(const CnetDcGrammar *g, const CnetDcTerm *term,
                           const CnetDcExample *ex, int n_ex) {
    int i;
    CnetDcValue v, got;
    if (g == NULL || term == NULL || (n_ex > 0 && ex == NULL)) return 0;
    if (n_ex <= 0) return 0;
    if (cnet_dc_eval_term(g, term->text, &v) != 0) return 0;
    for (i = 0; i < n_ex; ++i) {
        if (v.kind == CNET_DC_VAL_FN_INCR || v.kind == CNET_DC_VAL_FN_CONS ||
            v.kind == CNET_DC_VAL_FN_CONS1) {
            if (ex[i].in.kind == CNET_DC_VAL_NONE) return 0;
            if (apply_value(&v, &ex[i].in, &got) != 0) return 0;
            if (!cnet_dc_value_equal(&got, &ex[i].out)) return 0;
        } else {
            if (!cnet_dc_value_equal(&v, &ex[i].out)) return 0;
        }
    }
    return 1;
}

int cnet_dc_wake_io(CnetDcArena *arena, const CnetDcGrammar *g, int goal,
                    int max_depth, const CnetDcExample *ex, int n_ex,
                    CnetDcTerm *hits, int cap, int *n_hits) {
    CnetDcTerm typed[CNET_DC_MAX_HITS];
    int n_typed = 0, i;
    if (n_hits == NULL) return -1;
    *n_hits = 0;
    if (arena == NULL || g == NULL || hits == NULL || cap <= 0 || goal < 0)
        return -1;
    if (n_ex <= 0 || ex == NULL) return 0; /* abstain: no I/O to solve */
    if (cnet_dc_wake(arena, g, goal, max_depth, typed, CNET_DC_MAX_HITS,
                     &n_typed) != 0)
        return -1;
    for (i = 0; i < n_typed && *n_hits < cap; ++i) {
        if (!term_matches_io(g, &typed[i], ex, n_ex)) continue;
        hits[*n_hits] = typed[i];
        (*n_hits)++;
    }
    return 0;
}

static int already_str(char terms[][CNET_DC_TERM_MAX], int n, const char *s) {
    int i;
    for (i = 0; i < n; ++i) {
        if (strcmp(terms[i], s) == 0) return 1;
    }
    return 0;
}

static int add_subterm(char terms[][CNET_DC_TERM_MAX], int *n, int cap,
                       const char *s, int len) {
    char buf[CNET_DC_TERM_MAX];
    if (s == NULL || len <= 0 || *n >= cap) return 0;
    if (len >= (int)sizeof buf) return 0;
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    if (already_str(terms, *n, buf)) return 0;
    snprintf(terms[*n], CNET_DC_TERM_MAX, "%s", buf);
    (*n)++;
    return 0;
}

static int collect_subterms(const char *text, char terms[][CNET_DC_TERM_MAX],
                            int cap, int *n) {
    const char *s;
    if (text == NULL || n == NULL) return -1;
    s = text;
    while (*s) {
        if (*s == '(') {
            int depth = 0;
            const char *p = s;
            for (; *p; ++p) {
                if (*p == '(') depth++;
                else if (*p == ')') {
                    depth--;
                    if (depth == 0) {
                        add_subterm(terms, n, cap, s, (int)(p - s + 1));
                        collect_subterms(s + 1, terms, cap, n);
                        s = p;
                        break;
                    }
                }
            }
        }
        ++s;
    }
    return 0;
}

static int is_prim_name(const CnetDcGrammar *g, const char *text) {
    int i;
    if (g == NULL || text == NULL) return 0;
    for (i = 0; i < g->n_prims; ++i) {
        if (strcmp(g->prims[i].name, text) == 0) return 1;
    }
    return 0;
}

static int is_proper_subterm(const char *hay, const char *needle) {
    const char *p;
    if (hay == NULL || needle == NULL || needle[0] == '\0') return 0;
    if (strcmp(hay, needle) == 0) return 0;
    p = strstr(hay, needle);
    return p != NULL ? 1 : 0;
}

int cnet_dc_sleep_compress(CnetDcGrammar *g, const CnetDcTerm *solved,
                           int n_solved, char *name, size_t name_cap,
                           CnetDcTerm *brick) {
    char subs[CNET_DC_MAX_POOL][CNET_DC_TERM_MAX];
    int n_subs = 0, i, j, best = -1, best_len = 0;
    int proper[CNET_DC_MAX_POOL];
    char brick_name[CNET_DC_NAME_MAX];
    if (brick != NULL) memset(brick, 0, sizeof *brick);
    if (name != NULL && name_cap > 0) name[0] = '\0';
    if (g == NULL || solved == NULL || n_solved < 2) return 1;
    memset(proper, 0, sizeof proper);
    for (i = 0; i < n_solved; ++i) {
        if (solved[i].text[0] == '\0') continue;
        collect_subterms(solved[i].text, subs, CNET_DC_MAX_POOL, &n_subs);
    }
    for (j = 0; j < n_subs; ++j) {
        int seen = 0;
        if (subs[j][0] != '(') continue;
        if (is_prim_name(g, subs[j])) continue;
        for (i = 0; i < n_solved; ++i) {
            if (strstr(solved[i].text, subs[j]) == NULL) continue;
            seen++;
            if (is_proper_subterm(solved[i].text, subs[j])) proper[j] = 1;
        }
        if (seen >= 2 && proper[j]) {
            int len = (int)strlen(subs[j]);
            if (len > best_len) {
                best_len = len;
                best = j;
            }
        }
    }
    if (best < 0) return 1;
    snprintf(brick_name, sizeof brick_name, "brick%d", g->n_prims);
    {
        CnetDcTerm local;
        memset(&local, 0, sizeof local);
        snprintf(local.text, sizeof local.text, "%s", subs[best]);
        local.type = solved[0].type;
        for (i = 0; i < n_solved; ++i) {
            if (strcmp(solved[i].text, subs[best]) == 0) {
                local.type = solved[i].type;
                local.depth = solved[i].depth;
                break;
            }
        }
        if (cnet_dc_sleep_invent(g, brick_name, &local) != 0) return -1;
        if (brick != NULL) *brick = local;
    }
    if (name != NULL && name_cap > 0)
        snprintf(name, name_cap, "%s", brick_name);
    return 0;
}

static int banned_mine_path(const char *path) {
    char buf[512];
    size_t i;
    if (path == NULL) return 1;
    snprintf(buf, sizeof buf, "%s", path);
    for (i = 0; buf[i] != '\0'; ++i) {
        char c = buf[i];
        if (c >= 'A' && c <= 'Z') buf[i] = (char)(c - 'A' + 'a');
        if (buf[i] == '\\') buf[i] = '/';
    }
    if (strstr(buf, "asi-5") != NULL || strstr(buf, "asi5") != NULL ||
        strstr(buf, "asi_5") != NULL)
        return 1;
    if (strstr(buf, "chat-1") != NULL || strstr(buf, "chat1") != NULL ||
        strstr(buf, "chat_1") != NULL)
        return 1;
    if (strstr(buf, "compete_suite") != NULL ||
        strstr(buf, "suite_data_v5") != NULL ||
        strstr(buf, "suite_data_v4") != NULL)
        return 1;
    return 0;
}

static int parse_json_long(const char *line, const char *key, long *out,
                           int *found) {
    const char *p;
    char pat[32];
    if (found != NULL) *found = 0;
    if (line == NULL || key == NULL || out == NULL) return -1;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(line, pat);
    if (p == NULL) return 0;
    p = strchr(p + strlen(pat), ':');
    if (p == NULL) return 0;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p != '-' && (*p < '0' || *p > '9')) return 0;
    *out = strtol(p, NULL, 10);
    if (found != NULL) *found = 1;
    return 0;
}

int cnet_dc_mine_io_jsonl(const char *path, CnetDcExample *ex, int cap,
                          int *n_ex) {
    FILE *f;
    char line[256];
    if (n_ex == NULL) return -1;
    *n_ex = 0;
    if (path == NULL || ex == NULL || cap <= 0) return -1;
    if (banned_mine_path(path)) return -1;
    f = fopen(path, "r");
    if (f == NULL) return 0; /* missing file: skip, do not invent tasks */
    while (fgets(line, sizeof line, f) != NULL && *n_ex < cap) {
        long in_v = 0, out_v = 0;
        int has_in = 0, has_out = 0;
        if (line[0] == '#' || line[0] == '\n') continue;
        parse_json_long(line, "in", &in_v, &has_in);
        parse_json_long(line, "out", &out_v, &has_out);
        if (!has_out) continue;
        memset(&ex[*n_ex], 0, sizeof ex[*n_ex]);
        if (has_in) cnet_dc_value_int(&ex[*n_ex].in, in_v);
        cnet_dc_value_int(&ex[*n_ex].out, out_v);
        (*n_ex)++;
    }
    fclose(f);
    return 0;
}
