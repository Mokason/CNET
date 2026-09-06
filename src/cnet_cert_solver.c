/* CERT solver loop — operators until bind or budget. Never LM. Never FAQ. */
#include "../include/cnet_cert_solver.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    int64_t n;
    int64_t d;
} CsFrac;

typedef struct {
    int n;
    int parent;
    char op;
    uint64_t a, b, res;
    uint64_t bag[CNET_CERT_SOLVER_MAX_NUMS];
} CsINode;

typedef struct {
    int n;
    int parent;
    char op;
    CsFrac a, b, res;
    CsFrac bag[4];
} CsRNode;

static void scopy(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    if (!s) {
        d[0] = 0;
        return;
    }
    for (i = 0; s[i] && i + 1 < cap; i++) d[i] = s[i];
    d[i] = 0;
}

static int starts_word(const char *s, const char *w, const char **rest) {
    size_t n;
    if (!s || !w) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    n = strlen(w);
    if (strncasecmp(s, w, n) != 0) return 0;
    if (s[n] && !isspace((unsigned char)s[n]) && s[n] != ':' && s[n] != ',')
        return 0;
    s += n;
    while (*s == ' ' || *s == '\t' || *s == ':' || *s == ',') s++;
    if (rest) *rest = s;
    return 1;
}

static int parse_i64(const char **ps, int64_t *out) {
    const char *s;
    char *end;
    long long v;
    if (!ps || !*ps || !out) return 0;
    s = *ps;
    while (*s == ' ' || *s == '\t' || *s == ',' || *s == ';') s++;
    if (!isdigit((unsigned char)*s)) return 0;
    v = strtoll(s, &end, 10);
    if (end == s) return 0;
    *out = (int64_t)v;
    *ps = end;
    return 1;
}

static int parse_nums(const char *s, int64_t *nums, int maxn, int *nout) {
    int n = 0;
    if (!s || !nums || !nout) return 0;
    while (*s && n < maxn) {
        int64_t v;
        if (!parse_i64(&s, &v)) break;
        if (v < 0) return 0;
        nums[n++] = v;
    }
    while (*s == ' ' || *s == '\t' || *s == ',' || *s == '\n') s++;
    if (*s) return 0;
    *nout = n;
    return n > 0;
}

int cnet_cert_solver_shaped(const char *turn) {
    const char *rest = NULL;
    int64_t nums[CNET_CERT_SOLVER_MAX_NUMS];
    int n = 0;
    int64_t target;
    if (!turn || !turn[0]) return 0;
    if (starts_word(turn, "gcd", &rest)) {
        if (!parse_nums(rest, nums, 2, &n)) return 0;
        return n == 2;
    }
    if (starts_word(turn, "make", &rest) || starts_word(turn, "countdown", &rest)) {
        if (!parse_i64(&rest, &target)) return 0;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (starts_word(rest, "from", &rest) || starts_word(rest, "with", &rest) ||
            starts_word(rest, "using", &rest)) {
            /* rest advanced */
        }
        if (!parse_nums(rest, nums, CNET_CERT_SOLVER_MAX_NUMS, &n)) return 0;
        return n >= 2;
    }
    return 0;
}

static uint64_t fnv_u64(uint64_t h, uint64_t x) {
    int k;
    for (k = 0; k < 8; k++) {
        h ^= (x & 0xffu);
        h *= 1099511628211ULL;
        x >>= 8;
    }
    return h;
}

static int64_t iabs64(int64_t x) { return x < 0 ? -x : x; }

static int64_t gcd_i64(int64_t a, int64_t b) {
    a = iabs64(a);
    b = iabs64(b);
    while (b) {
        int64_t t = a % b;
        a = b;
        b = t;
    }
    return a ? a : 1;
}

static void frac_reduce(CsFrac *f) {
    int64_t g;
    if (!f) return;
    if (f->d < 0) {
        f->n = -f->n;
        f->d = -f->d;
    }
    if (f->n == 0) {
        f->d = 1;
        return;
    }
    if (f->d == 0) return;
    g = gcd_i64(f->n, f->d);
    f->n /= g;
    f->d /= g;
}

static int frac_eq_int(CsFrac f, int64_t t) {
    int64_t prod;
    if (f.d == 0) return 0;
    frac_reduce(&f);
    if (__builtin_mul_overflow(t, f.d, &prod)) return 0;
    return f.n == prod;
}

static int frac_op(char op, CsFrac a, CsFrac b, CsFrac *out) {
    int64_t n, d, t1, t2;
    if (!out || a.d == 0 || b.d == 0) return -1;
    frac_reduce(&a);
    frac_reduce(&b);
    switch (op) {
    case '+':
        if (__builtin_mul_overflow(a.n, b.d, &t1)) return -1;
        if (__builtin_mul_overflow(b.n, a.d, &t2)) return -1;
        if (__builtin_add_overflow(t1, t2, &n)) return -1;
        if (__builtin_mul_overflow(a.d, b.d, &d)) return -1;
        break;
    case '-':
        if (__builtin_mul_overflow(a.n, b.d, &t1)) return -1;
        if (__builtin_mul_overflow(b.n, a.d, &t2)) return -1;
        if (__builtin_sub_overflow(t1, t2, &n)) return -1;
        if (__builtin_mul_overflow(a.d, b.d, &d)) return -1;
        break;
    case '*':
        if (__builtin_mul_overflow(a.n, b.n, &n)) return -1;
        if (__builtin_mul_overflow(a.d, b.d, &d)) return -1;
        break;
    case '/':
        if (b.n == 0) return -1;
        if (__builtin_mul_overflow(a.n, b.d, &n)) return -1;
        if (__builtin_mul_overflow(a.d, b.n, &d)) return -1;
        break;
    default:
        return -1;
    }
    out->n = n;
    out->d = d;
    frac_reduce(out);
    if (out->d == 0) return -1;
    return 0;
}

static int frac_cmp(CsFrac a, CsFrac b) {
    frac_reduce(&a);
    frac_reduce(&b);
    if (a.n < b.n) return -1;
    if (a.n > b.n) return 1;
    if (a.d < b.d) return -1;
    if (a.d > b.d) return 1;
    return 0;
}

static void frac_sort(CsFrac *bag, int n) {
    int i, j;
    for (i = 1; i < n; i++) {
        CsFrac key = bag[i];
        j = i - 1;
        while (j >= 0 && frac_cmp(bag[j], key) > 0) {
            bag[j + 1] = bag[j];
            j--;
        }
        bag[j + 1] = key;
    }
}

static uint64_t bag_hash_u64(const uint64_t *bag, int n) {
    uint64_t h = 14695981039346656037ULL;
    int i;
    h = fnv_u64(h, (uint64_t)n);
    for (i = 0; i < n; i++) h = fnv_u64(h, bag[i]);
    return h;
}

static uint64_t bag_hash_frac(const CsFrac *bag, int n) {
    uint64_t h = 14695981039346656037ULL;
    int i;
    h = fnv_u64(h, (uint64_t)n);
    for (i = 0; i < n; i++) {
        h = fnv_u64(h, (uint64_t)bag[i].n);
        h = fnv_u64(h, (uint64_t)bag[i].d);
    }
    return h;
}

static int seen_insert(uint64_t *tab, int cap, uint64_t h) {
    int i, idx;
    if (!tab || cap <= 0) return 0;
    if (h == 0) h = 1;
    idx = (int)(h % (uint64_t)cap);
    for (i = 0; i < cap; i++) {
        int p = (idx + i) % cap;
        if (tab[p] == 0) {
            tab[p] = h;
            return 1;
        }
        if (tab[p] == h) return 0;
    }
    return 0;
}

static void sort_u64(uint64_t *a, int n) {
    int i, j;
    for (i = 1; i < n; i++) {
        uint64_t key = a[i];
        j = i - 1;
        while (j >= 0 && a[j] > key) {
            a[j + 1] = a[j];
            j--;
        }
        a[j + 1] = key;
    }
}

static void fmt_u64(char *d, size_t cap, uint64_t v) {
    snprintf(d, cap, "%llu", (unsigned long long)v);
}

static void fmt_frac(char *d, size_t cap, CsFrac f) {
    frac_reduce(&f);
    if (f.d == 1)
        snprintf(d, cap, "%lld", (long long)f.n);
    else
        snprintf(d, cap, "%lld/%lld", (long long)f.n, (long long)f.d);
}

static int show_append(char *show, size_t cap, const char *line) {
    size_t n, m;
    if (!show || !cap || !line) return -1;
    n = strlen(show);
    m = strlen(line);
    if (n + m + 2 >= cap) return -1;
    memcpy(show + n, line, m);
    show[n + m] = '\n';
    show[n + m + 1] = 0;
    return 0;
}

static void finish_spoken(CnetCertSolverResult *out) {
    if (!out) return;
    if (out->bound) {
        snprintf(out->spoken, sizeof out->spoken, "%s", out->value);
        out->claimed_cert = 1;
    } else {
        out->claimed_cert = 0;
        out->value[0] = 0;
        scopy(out->spoken, sizeof out->spoken, "ABSTAIN: no bind.");
        if (!out->show[0])
            scopy(out->show, sizeof out->show, "SHOW\nABSTAIN no_bind\n");
    }
}

static int gcd_solve(int64_t a, int64_t b, CnetCertSolverResult *out) {
    int64_t x = iabs64(a), y = iabs64(b);
    char line[160];
    out->hit = 1;
    scopy(out->show, sizeof out->show, "SHOW\n");
    if (x == 0 && y == 0) {
        show_append(out->show, sizeof out->show, "ABSTAIN gcd(0,0)");
        out->bound = 0;
        finish_spoken(out);
        return 0;
    }
    while (y) {
        int64_t t = x % y;
        snprintf(line, sizeof line, "HOP %d TABLE %lld %% %lld = %lld",
                 out->n_hops + 1, (long long)x, (long long)y, (long long)t);
        if (show_append(out->show, sizeof out->show, line) != 0) break;
        out->n_hops++;
        x = y;
        y = t;
        if (out->n_hops >= CNET_CERT_SOLVER_MAX_HOPS) break;
    }
    if (y != 0) {
        show_append(out->show, sizeof out->show, "ABSTAIN budget");
        out->budget_trip = 1;
        out->bound = 0;
        finish_spoken(out);
        return 0;
    }
    snprintf(out->value, sizeof out->value, "%lld", (long long)x);
    snprintf(line, sizeof line, "BIND %lld", (long long)x);
    show_append(out->show, sizeof out->show, line);
    out->bound = 1;
    finish_spoken(out);
    return 0;
}

static int reconstruct_int(const CsINode *pool, int idx, CnetCertSolverResult *out) {
    int chain[CNET_CERT_SOLVER_MAX_HOPS];
    int n = 0, i;
    char line[192], as[32], bs[32], rs[32];
    while (idx >= 0 && pool[idx].parent >= 0 && n < CNET_CERT_SOLVER_MAX_HOPS) {
        chain[n++] = idx;
        idx = pool[idx].parent;
    }
    scopy(out->show, sizeof out->show, "SHOW\n");
    for (i = n - 1; i >= 0; i--) {
        const CsINode *nd = &pool[chain[i]];
        fmt_u64(as, sizeof as, nd->a);
        fmt_u64(bs, sizeof bs, nd->b);
        fmt_u64(rs, sizeof rs, nd->res);
        snprintf(line, sizeof line, "HOP %d %s %c %s = %s", n - i, as, nd->op, bs,
                 rs);
        if (show_append(out->show, sizeof out->show, line) != 0) return -1;
        out->n_hops++;
    }
    return 0;
}

static int reconstruct_rat(const CsRNode *pool, int idx, CnetCertSolverResult *out) {
    int chain[CNET_CERT_SOLVER_MAX_HOPS];
    int n = 0, i;
    char line[192], as[40], bs[40], rs[40];
    while (idx >= 0 && pool[idx].parent >= 0 && n < CNET_CERT_SOLVER_MAX_HOPS) {
        chain[n++] = idx;
        idx = pool[idx].parent;
    }
    scopy(out->show, sizeof out->show, "SHOW\n");
    for (i = n - 1; i >= 0; i--) {
        const CsRNode *nd = &pool[chain[i]];
        fmt_frac(as, sizeof as, nd->a);
        fmt_frac(bs, sizeof bs, nd->b);
        fmt_frac(rs, sizeof rs, nd->res);
        snprintf(line, sizeof line, "HOP %d %s %c %s = %s", n - i, as, nd->op, bs,
                 rs);
        if (show_append(out->show, sizeof out->show, line) != 0) return -1;
        out->n_hops++;
    }
    return 0;
}

static int int_bfs(const int64_t *nums, int n, int64_t target,
                   CnetCertSolverResult *out) {
    CsINode *pool;
    int *q;
    uint64_t *seen;
    int qh = 0, qt = 0, used = 0, i, found = -1;
    int seen_cap = CNET_CERT_SOLVER_MAX_NODES * 2 + 7;
    if (n < 2 || n > CNET_CERT_SOLVER_MAX_NUMS) return 1;
    pool = (CsINode *)calloc((size_t)CNET_CERT_SOLVER_MAX_NODES, sizeof *pool);
    q = (int *)malloc((size_t)CNET_CERT_SOLVER_MAX_NODES * sizeof *q);
    seen = (uint64_t *)calloc((size_t)seen_cap, sizeof *seen);
    if (!pool || !q || !seen) {
        free(pool);
        free(q);
        free(seen);
        out->budget_trip = 1;
        scopy(out->show, sizeof out->show, "SHOW\nABSTAIN oom\n");
        finish_spoken(out);
        return 0;
    }
    pool[0].n = n;
    pool[0].parent = -1;
    for (i = 0; i < n; i++) pool[0].bag[i] = (uint64_t)nums[i];
    sort_u64(pool[0].bag, n);
    (void)seen_insert(seen, seen_cap, bag_hash_u64(pool[0].bag, n));
    q[qt++] = 0;
    used = 1;
    if (n == 1 && (int64_t)pool[0].bag[0] == target) found = 0;
    while (qh < qt && found < 0) {
        int cur = q[qh++];
        CsINode *nd = &pool[cur];
        int a, b;
        out->n_nodes++;
        if (nd->n == 1 && (int64_t)nd->bag[0] == target) {
            found = cur;
            break;
        }
        for (a = 0; a < nd->n && found < 0; a++) {
            for (b = 0; b < nd->n && found < 0; b++) {
                char ops[4];
                int oi, nop;
                if (a == b) continue;
                if (a < b) {
                    ops[0] = '+';
                    ops[1] = '*';
                    nop = 2;
                } else {
                    nop = 0;
                }
                ops[nop++] = '-';
                ops[nop++] = '/';
                for (oi = 0; oi < nop && found < 0; oi++) {
                    uint64_t va = nd->bag[a], vb = nd->bag[b], vr = 0;
                    char op = ops[oi];
                    CsINode *nw;
                    int k, m, ok = 0;
                    uint64_t h;
                    if (op == '+') {
                        if (!__builtin_add_overflow(va, vb, &vr)) ok = 1;
                    } else if (op == '*') {
                        if (!__builtin_mul_overflow(va, vb, &vr)) ok = 1;
                    } else if (op == '-') {
                        if (va > vb) {
                            vr = va - vb;
                            ok = 1;
                        }
                    } else if (op == '/') {
                        if (vb != 0 && va % vb == 0) {
                            vr = va / vb;
                            ok = 1;
                        }
                    }
                    if (!ok || vr == 0) continue;
                    if (used >= CNET_CERT_SOLVER_MAX_NODES) {
                        out->budget_trip = 1;
                        found = -2;
                        break;
                    }
                    nw = &pool[used];
                    memset(nw, 0, sizeof *nw);
                    m = 0;
                    for (k = 0; k < nd->n; k++) {
                        if (k == a || k == b) continue;
                        nw->bag[m++] = nd->bag[k];
                    }
                    nw->bag[m++] = vr;
                    nw->n = m;
                    sort_u64(nw->bag, m);
                    h = bag_hash_u64(nw->bag, m);
                    if (!seen_insert(seen, seen_cap, h)) continue;
                    nw->parent = cur;
                    nw->op = op;
                    nw->a = va;
                    nw->b = vb;
                    nw->res = vr;
                    q[qt++] = used;
                    if (m == 1 && (int64_t)vr == target) found = used;
                    used++;
                }
            }
        }
        if (found == -2) break;
    }
    out->n_nodes = used;
    if (found >= 0) {
        reconstruct_int(pool, found, out);
        snprintf(out->value, sizeof out->value, "%lld", (long long)target);
        {
            char line[64];
            snprintf(line, sizeof line, "BIND %lld", (long long)target);
            show_append(out->show, sizeof out->show, line);
        }
        out->bound = 1;
    } else {
        scopy(out->show, sizeof out->show,
              out->budget_trip ? "SHOW\nABSTAIN budget\n"
                               : "SHOW\nABSTAIN no_bind\n");
        out->bound = 0;
    }
    free(pool);
    free(q);
    free(seen);
    finish_spoken(out);
    return 0;
}

static int rat_bfs(const int64_t *nums, int n, int64_t target,
                   CnetCertSolverResult *out) {
    CsRNode *pool;
    int *q;
    uint64_t *seen;
    int qh = 0, qt = 0, used = 0, i, found = -1;
    int maxn = 20000;
    int seen_cap = maxn * 2 + 7;
    if (n < 2 || n > 4) return 1;
    pool = (CsRNode *)calloc((size_t)maxn, sizeof *pool);
    q = (int *)malloc((size_t)maxn * sizeof *q);
    seen = (uint64_t *)calloc((size_t)seen_cap, sizeof *seen);
    if (!pool || !q || !seen) {
        free(pool);
        free(q);
        free(seen);
        out->budget_trip = 1;
        scopy(out->show, sizeof out->show, "SHOW\nABSTAIN oom\n");
        finish_spoken(out);
        return 0;
    }
    pool[0].n = n;
    pool[0].parent = -1;
    for (i = 0; i < n; i++) {
        pool[0].bag[i].n = nums[i];
        pool[0].bag[i].d = 1;
    }
    frac_sort(pool[0].bag, n);
    (void)seen_insert(seen, seen_cap, bag_hash_frac(pool[0].bag, n));
    q[qt++] = 0;
    used = 1;
    while (qh < qt && found < 0) {
        int cur = q[qh++];
        CsRNode *nd = &pool[cur];
        int a, b;
        out->n_nodes++;
        if (nd->n == 1 && frac_eq_int(nd->bag[0], target)) {
            found = cur;
            break;
        }
        for (a = 0; a < nd->n && found < 0; a++) {
            for (b = 0; b < nd->n && found < 0; b++) {
                const char ops[] = {'+', '-', '*', '/'};
                int oi;
                if (a == b) continue;
                for (oi = 0; oi < 4 && found < 0; oi++) {
                    CsFrac vr;
                    CsRNode *nw;
                    int k, m;
                    uint64_t h;
                    if (frac_op(ops[oi], nd->bag[a], nd->bag[b], &vr) != 0)
                        continue;
                    if (used >= maxn) {
                        out->budget_trip = 1;
                        found = -2;
                        break;
                    }
                    nw = &pool[used];
                    memset(nw, 0, sizeof *nw);
                    m = 0;
                    for (k = 0; k < nd->n; k++) {
                        if (k == a || k == b) continue;
                        nw->bag[m++] = nd->bag[k];
                    }
                    nw->bag[m++] = vr;
                    nw->n = m;
                    frac_sort(nw->bag, m);
                    h = bag_hash_frac(nw->bag, m);
                    if (!seen_insert(seen, seen_cap, h)) continue;
                    nw->parent = cur;
                    nw->op = ops[oi];
                    nw->a = nd->bag[a];
                    nw->b = nd->bag[b];
                    nw->res = vr;
                    q[qt++] = used;
                    if (m == 1 && frac_eq_int(vr, target)) found = used;
                    used++;
                }
            }
        }
        if (found == -2) break;
    }
    out->n_nodes = used;
    if (found >= 0) {
        reconstruct_rat(pool, found, out);
        snprintf(out->value, sizeof out->value, "%lld", (long long)target);
        {
            char line[64];
            snprintf(line, sizeof line, "BIND %lld", (long long)target);
            show_append(out->show, sizeof out->show, line);
        }
        out->bound = 1;
    } else if (!out->bound) {
        scopy(out->show, sizeof out->show,
              out->budget_trip ? "SHOW\nABSTAIN budget\n"
                               : "SHOW\nABSTAIN no_bind\n");
        out->bound = 0;
    }
    free(pool);
    free(q);
    free(seen);
    finish_spoken(out);
    return 0;
}

int cnet_cert_solver_ask(const char *turn, CnetCertSolverResult *out) {
    const char *rest = NULL;
    int64_t nums[CNET_CERT_SOLVER_MAX_NUMS];
    int n = 0;
    int64_t target;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!turn || !turn[0]) return 1;
    if (starts_word(turn, "gcd", &rest)) {
        if (!parse_nums(rest, nums, 2, &n) || n != 2) return 1;
        out->hit = 1;
        return gcd_solve(nums[0], nums[1], out);
    }
    if (starts_word(turn, "make", &rest) || starts_word(turn, "countdown", &rest)) {
        if (!parse_i64(&rest, &target)) return 1;
        while (*rest == ' ' || *rest == '\t') rest++;
        if (starts_word(rest, "from", &rest) || starts_word(rest, "with", &rest) ||
            starts_word(rest, "using", &rest)) {
        }
        if (!parse_nums(rest, nums, CNET_CERT_SOLVER_MAX_NUMS, &n) || n < 2)
            return 1;
        out->hit = 1;
        if (int_bfs(nums, n, target, out) != 0) return 1;
        if (out->bound) return 0;
        if (n <= 4) {
            memset(out, 0, sizeof *out);
            out->hit = 1;
            return rat_bfs(nums, n, target, out);
        }
        return 0;
    }
    return 1;
}

static int expect(int cond, const char *name) {
    printf("%s %s\n", name, cond ? "PASS" : "FAIL");
    return cond ? 0 : 1;
}

int cnet_cert_solver_selftest(void) {
    CnetCertSolverResult r;
    int fails = 0;
    fails += expect(cnet_cert_solver_ask("hello world", &r) == 1 && r.hit == 0,
                    "not_shaped");
    fails += expect(cnet_cert_solver_shaped("gcd 48 18") == 1, "gcd_shaped");
    fails += expect(cnet_cert_solver_ask("gcd 48 18", &r) == 0 && r.bound &&
                        r.claimed_cert == 1 && strcmp(r.value, "6") == 0 &&
                        strstr(r.show, "SHOW") && strstr(r.show, "BIND 6") &&
                        r.n_hops >= 1,
                    "gcd_48_18");
    fails += expect(cnet_cert_solver_ask("gcd 0 0", &r) == 0 && r.hit &&
                        r.claimed_cert == 0 && !r.bound,
                    "gcd_zero_abstain");
    fails += expect(cnet_cert_solver_ask("gcd 123456789 987654321", &r) == 0 &&
                        r.claimed_cert == 1 && strcmp(r.value, "9") == 0,
                    "gcd_large");
    fails += expect(cnet_cert_solver_ask("make 24 from 8 8 3 3", &r) == 0 &&
                        r.claimed_cert == 1 && strcmp(r.value, "24") == 0 &&
                        strstr(r.show, "HOP") && strstr(r.show, "BIND 24"),
                    "make_24_8833");
    fails += expect(cnet_cert_solver_ask("make 24 from 1 3 4 6", &r) == 0 &&
                        r.claimed_cert == 1 && strcmp(r.value, "24") == 0,
                    "make_24_1346_not_faq");
    fails += expect(cnet_cert_solver_ask("make 5 from 2 2", &r) == 0 && r.hit &&
                        r.claimed_cert == 0 && !r.bound,
                    "make_no_solution");
    fails += expect(cnet_cert_solver_ask("make 719 from 2 3 4 6 8 10", &r) == 0 &&
                        r.claimed_cert == 1 && strcmp(r.value, "719") == 0 &&
                        strstr(r.show, "HOP"),
                    "make_719_search");
    fails += expect(cnet_cert_solver_ask("10 plus 11", &r) == 1 && r.hit == 0,
                    "arith_not_solver");
    fails += expect(cnet_cert_solver_ask("make 952 from 25 50 75 100 3 6", &r) ==
                            0 &&
                        r.claimed_cert == 1 && strcmp(r.value, "952") == 0,
                    "make_952_countdown");
    if (fails == 0)
        printf("CERT_SOLVER_PASS\n");
    else
        printf("CERT_SOLVER_FAIL fails=%d\n", fails);
    return fails ? 1 : 0;
}
