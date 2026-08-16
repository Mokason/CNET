#include "../include/cnet_dc_invent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Small e-graph sleep. Inverse-β bound 1, congruence + hashcons.
   Propose only: cnet_dc_sleep_invent. Never specialist_admit.
   CSE (cnet_dc_sleep_compress / strstr) stays the old door. */

#define CNET_DC_EG_MAX 128
#define CNET_DC_AST_MAX 256
#define CNET_DC_EN_ATOM 0
#define CNET_DC_EN_APP 1
#define CNET_DC_EN_LAM 2

static int extract_specialist_admit_calls;

int cnet_dc_extract_specialist_admit_calls(void) {
    return extract_specialist_admit_calls;
}

typedef struct {
    int kind;
    char atom[CNET_DC_NAME_MAX];
    int child_a;
    int child_b;
    int eclass;
} CnetDcENode;

typedef struct {
    int parent;
    int rank;
    int trace_mask;
    int root_mask;
    int proper_mask;
    int candidate;
} CnetDcEClass;

typedef struct {
    CnetDcENode nodes[CNET_DC_EG_MAX];
    int n_nodes;
    CnetDcEClass classes[CNET_DC_EG_MAX];
    int n_classes;
} CnetDcEGraph;

typedef struct {
    int kind;
    char name[CNET_DC_NAME_MAX];
    int left;
    int right;
    int eclass;
} CnetDcAstNode;

typedef struct {
    CnetDcAstNode nodes[CNET_DC_AST_MAX];
    int n;
} CnetDcAst;

static void eg_init(CnetDcEGraph *eg) {
    if (eg == NULL) return;
    memset(eg, 0, sizeof *eg);
}

static int eg_find(CnetDcEGraph *eg, int c) {
    if (eg == NULL || c < 0 || c >= eg->n_classes) return c;
    if (eg->classes[c].parent != c)
        eg->classes[c].parent = eg_find(eg, eg->classes[c].parent);
    return eg->classes[c].parent;
}

static int eg_find_c(const CnetDcEGraph *eg, int c) {
    if (eg == NULL || c < 0 || c >= eg->n_classes) return c;
    while (eg->classes[c].parent != c) c = eg->classes[c].parent;
    return c;
}

static int eg_new_class(CnetDcEGraph *eg) {
    int c;
    if (eg == NULL || eg->n_classes >= CNET_DC_EG_MAX) return -1;
    c = eg->n_classes++;
    eg->classes[c].parent = c;
    eg->classes[c].rank = 0;
    eg->classes[c].trace_mask = 0;
    eg->classes[c].root_mask = 0;
    eg->classes[c].proper_mask = 0;
    eg->classes[c].candidate = 0;
    return c;
}

static void eg_merge_raw(CnetDcEGraph *eg, int a, int b) {
    int ra, rb;
    if (eg == NULL) return;
    ra = eg_find(eg, a);
    rb = eg_find(eg, b);
    if (ra < 0 || rb < 0 || ra == rb) return;
    if (eg->classes[ra].rank < eg->classes[rb].rank) {
        int t = ra;
        ra = rb;
        rb = t;
    }
    eg->classes[rb].parent = ra;
    if (eg->classes[ra].rank == eg->classes[rb].rank) eg->classes[ra].rank++;
    eg->classes[ra].trace_mask |= eg->classes[rb].trace_mask;
    eg->classes[ra].root_mask |= eg->classes[rb].root_mask;
    eg->classes[ra].proper_mask |= eg->classes[rb].proper_mask;
    eg->classes[ra].candidate |= eg->classes[rb].candidate;
}

static int enode_same(CnetDcEGraph *eg, const CnetDcENode *a,
                      const CnetDcENode *b) {
    if (a == NULL || b == NULL || a->kind != b->kind) return 0;
    if (a->kind == CNET_DC_EN_ATOM) return strcmp(a->atom, b->atom) == 0;
    if (a->kind == CNET_DC_EN_APP)
        return eg_find(eg, a->child_a) == eg_find(eg, b->child_a) &&
               eg_find(eg, a->child_b) == eg_find(eg, b->child_b);
    if (a->kind == CNET_DC_EN_LAM)
        return strcmp(a->atom, b->atom) == 0 &&
               eg_find(eg, a->child_a) == eg_find(eg, b->child_a);
    return 0;
}

static void eg_rebuild(CnetDcEGraph *eg) {
    int guard, changed;
    if (eg == NULL) return;
    for (guard = 0; guard < 32; ++guard) {
        int i, j;
        changed = 0;
        for (i = 0; i < eg->n_nodes; ++i) {
            for (j = i + 1; j < eg->n_nodes; ++j) {
                if (!enode_same(eg, &eg->nodes[i], &eg->nodes[j])) continue;
                if (eg_find(eg, eg->nodes[i].eclass) ==
                    eg_find(eg, eg->nodes[j].eclass))
                    continue;
                eg_merge_raw(eg, eg->nodes[i].eclass, eg->nodes[j].eclass);
                changed = 1;
            }
        }
        if (!changed) break;
    }
}

static void eg_merge(CnetDcEGraph *eg, int a, int b) {
    eg_merge_raw(eg, a, b);
    eg_rebuild(eg);
}

static int eg_hashcons(CnetDcEGraph *eg, int kind, const char *name, int ca,
                       int cb) {
    int i, node, cls, fa, fb;
    if (eg == NULL) return -1;
    fa = (ca >= 0) ? eg_find(eg, ca) : -1;
    fb = (cb >= 0) ? eg_find(eg, cb) : -1;
    for (i = 0; i < eg->n_nodes; ++i) {
        CnetDcENode *n = &eg->nodes[i];
        if (n->kind != kind) continue;
        if (kind == CNET_DC_EN_ATOM) {
            if (name != NULL && strcmp(n->atom, name) == 0)
                return eg_find(eg, n->eclass);
        } else if (kind == CNET_DC_EN_APP) {
            if (eg_find(eg, n->child_a) == fa && eg_find(eg, n->child_b) == fb)
                return eg_find(eg, n->eclass);
        } else if (kind == CNET_DC_EN_LAM) {
            if (name != NULL && strcmp(n->atom, name) == 0 &&
                eg_find(eg, n->child_a) == fa)
                return eg_find(eg, n->eclass);
        }
    }
    if (eg->n_nodes >= CNET_DC_EG_MAX) return -1;
    cls = eg_new_class(eg);
    if (cls < 0) return -1;
    node = eg->n_nodes++;
    eg->nodes[node].kind = kind;
    eg->nodes[node].atom[0] = '\0';
    if (name != NULL)
        snprintf(eg->nodes[node].atom, sizeof eg->nodes[node].atom, "%s", name);
    eg->nodes[node].child_a = fa;
    eg->nodes[node].child_b = fb;
    eg->nodes[node].eclass = cls;
    return cls;
}

static int ast_add(CnetDcAst *ast, int kind, const char *name, int left,
                   int right) {
    int id;
    if (ast == NULL || ast->n >= CNET_DC_AST_MAX) return -1;
    id = ast->n++;
    ast->nodes[id].kind = kind;
    ast->nodes[id].name[0] = '\0';
    if (name != NULL) {
        size_t k = 0;
        while (name[k] != '\0' && k + 1 < sizeof ast->nodes[id].name) {
            ast->nodes[id].name[k] = name[k];
            k++;
        }
        ast->nodes[id].name[k] = '\0';
    }
    ast->nodes[id].left = left;
    ast->nodes[id].right = right;
    ast->nodes[id].eclass = -1;
    return id;
}

static const char *skip_ws(const char *s) {
    while (s != NULL && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r'))
        ++s;
    return s;
}

static int parse_atom(CnetDcAst *ast, const char *s, const char **end) {
    char name[CNET_DC_NAME_MAX];
    int n = 0;
    s = skip_ws(s);
    if (s == NULL || *s == '\0') return -1;
    while (s[n] != '\0' && s[n] != ' ' && s[n] != '\t' && s[n] != '(' &&
           s[n] != ')') {
        if (n + 1 >= (int)sizeof name) return -1;
        name[n] = s[n];
        n++;
    }
    if (n == 0) return -1;
    name[n] = '\0';
    if (end != NULL) *end = s + n;
    return ast_add(ast, CNET_DC_EN_ATOM, name, -1, -1);
}

static int parse_term(CnetDcAst *ast, const char *s, const char **end);

static int parse_term(CnetDcAst *ast, const char *s, const char **end) {
    int fn, arg, id;
    s = skip_ws(s);
    if (s == NULL || *s == '\0') return -1;
    if (*s != '(') return parse_atom(ast, s, end);
    ++s;
    s = skip_ws(s);
    if (s[0] == 'l' && s[1] == 'a' && s[2] == 'm' &&
        (s[3] == ' ' || s[3] == '\t' || s[3] == '(')) {
        int var, body;
        s += 3;
        s = skip_ws(s);
        if (*s != '(') return -1;
        ++s;
        var = parse_atom(ast, s, &s);
        if (var < 0) return -1;
        s = skip_ws(s);
        if (*s != ')') return -1;
        ++s;
        body = parse_term(ast, s, &s);
        if (body < 0) return -1;
        s = skip_ws(s);
        if (*s != ')') return -1;
        ++s;
        id = ast_add(ast, CNET_DC_EN_LAM, ast->nodes[var].name, body, -1);
        if (end != NULL) *end = s;
        return id;
    }
    fn = parse_term(ast, s, &s);
    if (fn < 0) return -1;
    arg = parse_term(ast, s, &s);
    if (arg < 0) return -1;
    s = skip_ws(s);
    if (*s != ')') return -1;
    ++s;
    id = ast_add(ast, CNET_DC_EN_APP, "", fn, arg);
    if (end != NULL) *end = s;
    return id;
}

static int parse_closed(CnetDcAst *ast, const char *text) {
    const char *end = NULL;
    int id;
    if (ast == NULL || text == NULL || text[0] == '\0') return -1;
    id = parse_term(ast, text, &end);
    if (id < 0) return -1;
    end = skip_ws(end);
    if (end == NULL || *end != '\0') return -1;
    return id;
}

static int ast_has_name(const CnetDcAst *ast, int id, const char *name) {
    if (ast == NULL || id < 0 || id >= ast->n || name == NULL) return 0;
    if (strcmp(ast->nodes[id].name, name) == 0) return 1;
    if (ast->nodes[id].kind == CNET_DC_EN_APP)
        return ast_has_name(ast, ast->nodes[id].left, name) ||
               ast_has_name(ast, ast->nodes[id].right, name);
    if (ast->nodes[id].kind == CNET_DC_EN_LAM)
        return ast_has_name(ast, ast->nodes[id].left, name);
    return 0;
}

static void fresh_var(const CnetDcAst *ast, int root, char *out, size_t cap) {
    int n;
    if (out == NULL || cap == 0) return;
    for (n = 0; n < 16; ++n) {
        if (n == 0)
            snprintf(out, cap, "x");
        else
            snprintf(out, cap, "x%d", n);
        if (!ast_has_name(ast, root, out)) return;
    }
    snprintf(out, cap, "x");
}

static int ast_subst(CnetDcAst *ast, int id, int target, const char *var) {
    int L, R;
    if (ast == NULL || id < 0 || id >= ast->n) return -1;
    if (id == target) return ast_add(ast, CNET_DC_EN_ATOM, var, -1, -1);
    if (ast->nodes[id].kind == CNET_DC_EN_ATOM)
        return ast_add(ast, CNET_DC_EN_ATOM, ast->nodes[id].name, -1, -1);
    if (ast->nodes[id].kind == CNET_DC_EN_LAM) {
        L = ast_subst(ast, ast->nodes[id].left, target, var);
        if (L < 0) return -1;
        return ast_add(ast, CNET_DC_EN_LAM, ast->nodes[id].name, L, -1);
    }
    L = ast_subst(ast, ast->nodes[id].left, target, var);
    R = ast_subst(ast, ast->nodes[id].right, target, var);
    if (L < 0 || R < 0) return -1;
    return ast_add(ast, CNET_DC_EN_APP, "", L, R);
}

static int ast_copy(CnetDcAst *ast, int id) {
    return ast_subst(ast, id, -1, "");
}

static void ast_collect(const CnetDcAst *ast, int id, int *ids, int *n, int cap,
                        int skip) {
    if (ast == NULL || id < 0 || ids == NULL || n == NULL || *n >= cap) return;
    if (!skip) {
        ids[*n] = id;
        (*n)++;
    }
    if (ast->nodes[id].kind == CNET_DC_EN_APP) {
        ast_collect(ast, ast->nodes[id].left, ids, n, cap, 0);
        ast_collect(ast, ast->nodes[id].right, ids, n, cap, 0);
    } else if (ast->nodes[id].kind == CNET_DC_EN_LAM) {
        ast_collect(ast, ast->nodes[id].left, ids, n, cap, 0);
    }
}

static int eg_insert_ast(CnetDcEGraph *eg, CnetDcAst *ast, int id) {
    int a, b, cls;
    if (eg == NULL || ast == NULL || id < 0 || id >= ast->n) return -1;
    if (ast->nodes[id].kind == CNET_DC_EN_ATOM) {
        cls = eg_hashcons(eg, CNET_DC_EN_ATOM, ast->nodes[id].name, -1, -1);
    } else if (ast->nodes[id].kind == CNET_DC_EN_LAM) {
        a = eg_insert_ast(eg, ast, ast->nodes[id].left);
        if (a < 0) return -1;
        cls = eg_hashcons(eg, CNET_DC_EN_LAM, ast->nodes[id].name, a, -1);
    } else {
        a = eg_insert_ast(eg, ast, ast->nodes[id].left);
        b = eg_insert_ast(eg, ast, ast->nodes[id].right);
        if (a < 0 || b < 0) return -1;
        cls = eg_hashcons(eg, CNET_DC_EN_APP, "", a, b);
    }
    if (cls < 0) return -1;
    ast->nodes[id].eclass = eg_find(eg, cls);
    return ast->nodes[id].eclass;
}

static void eg_mark_ast(CnetDcEGraph *eg, CnetDcAst *ast, int id, int bit,
                        int is_root) {
    int cls;
    if (eg == NULL || ast == NULL || id < 0 || id >= ast->n || bit == 0) return;
    cls = eg_find(eg, ast->nodes[id].eclass);
    if (cls < 0 || cls >= eg->n_classes) return;
    eg->classes[cls].trace_mask |= bit;
    if (is_root)
        eg->classes[cls].root_mask |= bit;
    else
        eg->classes[cls].proper_mask |= bit;
    if (ast->nodes[id].kind == CNET_DC_EN_APP) {
        eg_mark_ast(eg, ast, ast->nodes[id].left, bit, 0);
        eg_mark_ast(eg, ast, ast->nodes[id].right, bit, 0);
    } else if (ast->nodes[id].kind == CNET_DC_EN_LAM) {
        eg_mark_ast(eg, ast, ast->nodes[id].left, bit, 0);
    }
}

static int print_en(const CnetDcEGraph *eg, int node, char *out, size_t cap,
                    int depth);

static int print_cls(const CnetDcEGraph *eg, int cls, char *out, size_t cap,
                     int depth) {
    int i, pick = -1;
    if (eg == NULL || out == NULL || cap == 0) return -1;
    out[0] = '\0';
    cls = eg_find_c(eg, cls);
    for (i = 0; i < eg->n_nodes; ++i) {
        if (eg_find_c(eg, eg->nodes[i].eclass) != cls) continue;
        if (pick < 0) pick = i;
        if (eg->nodes[i].kind == CNET_DC_EN_LAM) {
            pick = i;
            break;
        }
    }
    if (pick < 0) return -1;
    return print_en(eg, pick, out, cap, depth);
}

static int print_en(const CnetDcEGraph *eg, int node, char *out, size_t cap,
                    int depth) {
    char a[CNET_DC_TERM_MAX], b[CNET_DC_TERM_MAX];
    const CnetDcENode *n;
    int w;
    if (eg == NULL || out == NULL || cap == 0 || node < 0 ||
        node >= eg->n_nodes)
        return -1;
    out[0] = '\0';
    if (depth > 12) {
        snprintf(out, cap, "?");
        return 0;
    }
    n = &eg->nodes[node];
    if (n->kind == CNET_DC_EN_ATOM) {
        w = snprintf(out, cap, "%s", n->atom);
        return (w < 0 || (size_t)w >= cap) ? -1 : 0;
    }
    a[0] = '\0';
    b[0] = '\0';
    if (n->kind == CNET_DC_EN_LAM) {
        if (print_cls(eg, n->child_a, a, sizeof a, depth + 1) != 0) return -1;
        w = snprintf(out, cap, "(lam (%s) %s)", n->atom, a);
        return (w < 0 || (size_t)w >= cap) ? -1 : 0;
    }
    if (print_cls(eg, n->child_a, a, sizeof a, depth + 1) != 0) return -1;
    if (print_cls(eg, n->child_b, b, sizeof b, depth + 1) != 0) return -1;
    w = snprintf(out, cap, "(%s %s)", a, b);
    return (w < 0 || (size_t)w >= cap) ? -1 : 0;
}

static int is_prim_name(const CnetDcGrammar *g, const char *text) {
    int i;
    if (g == NULL || text == NULL) return 0;
    for (i = 0; i < g->n_prims; ++i) {
        if (strcmp(g->prims[i].name, text) == 0) return 1;
    }
    return 0;
}

static int popcount_bits(int m) {
    int n = 0;
    while (m != 0) {
        n += m & 1;
        m >>= 1;
    }
    return n;
}

static int inv_beta_one(CnetDcEGraph *eg, CnetDcAst *ast, int root, int sub,
                        int bit) {
    char var[CNET_DC_NAME_MAX];
    int body, lam, arg, app, orig, neu;
    if (eg == NULL || ast == NULL || root < 0 || sub < 0 || root == sub)
        return -1;
    fresh_var(ast, root, var, sizeof var);
    body = ast_subst(ast, root, sub, var);
    if (body < 0) return -1;
    lam = ast_add(ast, CNET_DC_EN_LAM, var, body, -1);
    arg = ast_copy(ast, sub);
    if (lam < 0 || arg < 0) return -1;
    app = ast_add(ast, CNET_DC_EN_APP, "", lam, arg);
    if (app < 0) return -1;
    neu = eg_insert_ast(eg, ast, app);
    if (neu < 0) return -1;
    eg_mark_ast(eg, ast, app, bit, 1);
    orig = ast->nodes[root].eclass;
    eg_merge(eg, orig, neu);
    ast->nodes[root].eclass = eg_find(eg, orig);
    return 0;
}

static int insert_certified(CnetDcEGraph *eg, CnetDcAst *ast, const char *text,
                            int bit) {
    int root, subs[64], n_subs = 0, i, bound;
    if (eg == NULL || ast == NULL || text == NULL || text[0] == '\0') return -1;
    root = parse_closed(ast, text);
    if (root < 0) return -1;
    if (eg_insert_ast(eg, ast, root) < 0) return -1;
    eg_mark_ast(eg, ast, root, bit, 1);
    if (CNET_DC_INV_BETA_BOUND < 1) return 0;
    ast_collect(ast, root, subs, &n_subs, 64, 1);
    bound = CNET_DC_INV_BETA_BOUND;
    (void)bound;
    for (i = 0; i < n_subs; ++i) {
        if (inv_beta_one(eg, ast, root, subs[i], bit) != 0) return -1;
    }
    return 0;
}

static int insert_candidate(CnetDcEGraph *eg, CnetDcAst *ast, const char *text) {
    int root, cls;
    if (eg == NULL || ast == NULL || text == NULL || text[0] == '\0') return -1;
    root = parse_closed(ast, text);
    if (root < 0) return -1;
    cls = eg_insert_ast(eg, ast, root);
    if (cls < 0) return -1;
    cls = eg_find(eg, cls);
    eg->classes[cls].candidate = 1;
    return 0;
}

static int build_graph(CnetDcEGraph *eg, CnetDcAst *ast,
                       const CnetDcMissRow *rows, int n_rows) {
    int i, cert_i = 0;
    if (eg == NULL || ast == NULL) return -1;
    eg_init(eg);
    if (ast->n != 0) memset(ast, 0, sizeof *ast);
    if (rows == NULL || n_rows <= 0) return 0;
    for (i = 0; i < n_rows; ++i) {
        if (!rows[i].certified || rows[i].term[0] == '\0') continue;
        if (cert_i >= 31) break;
        if (insert_certified(eg, ast, rows[i].term, 1 << cert_i) != 0)
            return -1;
        cert_i++;
    }
    for (i = 0; i < n_rows; ++i) {
        if (rows[i].unused_invent_term[0] == '\0') continue;
        /* Candidates only. Never equate to a gold out (out is not a term). */
        if (insert_candidate(eg, ast, rows[i].unused_invent_term) != 0)
            return -1;
    }
    return 0;
}

static int extract_from_graph(CnetDcGrammar *g, CnetDcEGraph *eg, char *name,
                              size_t name_cap, CnetDcTerm *brick) {
    int c, best = -1, best_len = 0, type = 0;
    char text[CNET_DC_TERM_MAX], brick_name[CNET_DC_NAME_MAX];
    CnetDcTerm local;
    if (brick != NULL) memset(brick, 0, sizeof *brick);
    if (name != NULL && name_cap > 0) name[0] = '\0';
    if (g == NULL || eg == NULL) return -1;
    for (c = 0; c < eg->n_classes; ++c) {
        int cls, len;
        if (eg_find(eg, c) != c) continue;
        cls = c;
        if (popcount_bits(eg->classes[cls].trace_mask) < 2) continue;
        if (eg->classes[cls].proper_mask == 0) continue;
        if (print_cls(eg, cls, text, sizeof text, 0) != 0) continue;
        if (text[0] != '(') continue;
        if (is_prim_name(g, text)) continue;
        len = (int)strlen(text);
        if (len > best_len) {
            best_len = len;
            best = cls;
        }
    }
    if (best < 0) return 1;
    if (print_cls(eg, best, text, sizeof text, 0) != 0) return -1;
    memset(&local, 0, sizeof local);
    snprintf(local.text, sizeof local.text, "%s", text);
    local.type = type;
    snprintf(brick_name, sizeof brick_name, "ebrick%d", g->n_prims);
    if (cnet_dc_sleep_invent(g, brick_name, &local) != 0) return -1;
    if (brick != NULL) *brick = local;
    if (name != NULL && name_cap > 0)
        snprintf(name, name_cap, "%s", brick_name);
    return 0;
}

static int json_key(const char *line, const char *key, const char **val) {
    char pat[40];
    const char *p;
    if (line == NULL || key == NULL || val == NULL) return 0;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(line, pat);
    if (p == NULL) return 0;
    p = strchr(p + strlen(pat), ':');
    if (p == NULL) return 0;
    ++p;
    while (*p == ' ' || *p == '\t') ++p;
    *val = p;
    return 1;
}

static int json_string(const char *p, char *out, size_t cap) {
    size_t n = 0;
    if (p == NULL || out == NULL || cap == 0) return 0;
    out[0] = '\0';
    if (*p != '"') {
        if (strncmp(p, "none", 4) == 0 || strncmp(p, "null", 4) == 0) {
            snprintf(out, cap, "none");
            return 1;
        }
        return 0;
    }
    ++p;
    while (*p != '\0' && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1] != '\0') ++p;
        out[n++] = *p++;
    }
    out[n] = '\0';
    return 1;
}

static int json_long(const char *p, long *out) {
    if (p == NULL || out == NULL) return 0;
    if (*p != '-' && (*p < '0' || *p > '9')) return 0;
    *out = strtol(p, NULL, 10);
    return 1;
}

static void parse_row(const char *line, CnetDcMissRow *row) {
    const char *v;
    char buf[CNET_DC_TERM_MAX];
    long n = 0;
    if (row == NULL) return;
    memset(row, 0, sizeof *row);
    if (line == NULL) return;
    if (json_key(line, "goal_type", &v))
        json_string(v, row->goal_type, sizeof row->goal_type);
    if (json_key(line, "in", &v) && json_long(v, &n)) {
        cnet_dc_value_int(&row->in, n);
        row->has_in = 1;
    }
    if (json_key(line, "out", &v)) {
        if (json_long(v, &n)) {
            cnet_dc_value_int(&row->out, n);
            row->has_out = 1;
        } else if (json_string(v, buf, sizeof buf) &&
                   (strcmp(buf, "none") == 0 || buf[0] == '\0')) {
            row->has_out = 0;
            row->out.kind = CNET_DC_VAL_NONE;
        }
    }
    if (json_key(line, "abstain_reason", &v))
        json_string(v, row->abstain_reason, sizeof row->abstain_reason);
    if (json_key(line, "unused_invent_term", &v))
        json_string(v, row->unused_invent_term, sizeof row->unused_invent_term);
    if (json_key(line, "term", &v)) json_string(v, row->term, sizeof row->term);
    if (json_key(line, "trace_id", &v))
        json_string(v, row->trace_id, sizeof row->trace_id);
    if (json_key(line, "certified", &v)) {
        if (*v == '1' || strncmp(v, "true", 4) == 0) row->certified = 1;
    }
}

int cnet_dc_misslog_load(const char *path, CnetDcMissRow *rows, int cap,
                         int *n_rows) {
    FILE *f;
    char line[512];
    if (n_rows == NULL) return -1;
    *n_rows = 0;
    if (path == NULL || rows == NULL || cap <= 0) return -1;
    if (cnet_dc_banned_mine_path(path)) return -1;
    f = fopen(path, "r");
    if (f == NULL) return 0;
    while (fgets(line, sizeof line, f) != NULL && *n_rows < cap) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        parse_row(line, &rows[*n_rows]);
        (*n_rows)++;
    }
    fclose(f);
    return 0;
}

int cnet_dc_misslog_append(const char *path, const CnetDcMissRow *row) {
    FILE *f;
    const char *out_s;
    char out_num[32];
    if (path == NULL || row == NULL) return -1;
    if (cnet_dc_banned_mine_path(path)) return -1;
    f = fopen(path, "a");
    if (f == NULL) return -1;
    if (row->has_out) {
        snprintf(out_num, sizeof out_num, "%ld", row->out.num);
        out_s = out_num;
    } else {
        out_s = "\"none\"";
    }
    fprintf(f,
            "{\"goal_type\":\"%s\",\"in\":%ld,\"out\":%s,"
            "\"abstain_reason\":\"%s\",\"unused_invent_term\":\"%s\","
            "\"term\":\"%s\",\"trace_id\":\"%s\",\"certified\":%d}\n",
            row->goal_type[0] ? row->goal_type : "int",
            row->has_in ? row->in.num : 0L, out_s, row->abstain_reason,
            row->unused_invent_term, row->term, row->trace_id, row->certified);
    fclose(f);
    return 0;
}

int cnet_dc_misslog_extract_rows(CnetDcGrammar *g, const CnetDcMissRow *rows,
                                 int n_rows, char *name, size_t name_cap,
                                 CnetDcTerm *brick) {
    CnetDcEGraph eg;
    CnetDcAst ast;
    int rc;
    if (brick != NULL) memset(brick, 0, sizeof *brick);
    if (name != NULL && name_cap > 0) name[0] = '\0';
    if (g == NULL) return -1;
    if (rows == NULL || n_rows < 2) return 1;
    memset(&ast, 0, sizeof ast);
    if (build_graph(&eg, &ast, rows, n_rows) != 0) return -1;
    rc = extract_from_graph(g, &eg, name, name_cap, brick);
    /* Propose only. specialist_admit is not called; counter stays 0. */
    return rc;
}

int cnet_dc_misslog_extract(CnetDcGrammar *g, const char *path, char *name,
                            size_t name_cap, CnetDcTerm *brick) {
    CnetDcMissRow rows[CNET_DC_MISSLOG_MAX];
    int n = 0, rc;
    if (brick != NULL) memset(brick, 0, sizeof *brick);
    if (name != NULL && name_cap > 0) name[0] = '\0';
    rc = cnet_dc_misslog_load(path, rows, CNET_DC_MISSLOG_MAX, &n);
    if (rc != 0) return rc;
    if (n == 0) return 1;
    return cnet_dc_misslog_extract_rows(g, rows, n, name, name_cap, brick);
}

int cnet_dc_misslog_eclass_shared(const CnetDcMissRow *rows, int n_rows,
                                  const char *a, const char *b) {
    CnetDcEGraph eg;
    CnetDcAst ast;
    int ia, ib, ca, cb;
    if (rows == NULL || a == NULL || b == NULL) return 0;
    memset(&ast, 0, sizeof ast);
    if (build_graph(&eg, &ast, rows, n_rows) != 0) return 0;
    ia = parse_closed(&ast, a);
    ib = parse_closed(&ast, b);
    if (ia < 0 || ib < 0) return 0;
    ca = eg_insert_ast(&eg, &ast, ia);
    cb = eg_insert_ast(&eg, &ast, ib);
    if (ca < 0 || cb < 0) return 0;
    return eg_find(&eg, ca) == eg_find(&eg, cb) ? 1 : 0;
}
