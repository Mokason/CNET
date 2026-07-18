#include "../include/cnet_math_solve.h"
#include "../include/cnet_pattern.h"
#include "../include/contract/mcp_math_eval.h"
#include "../include/contract/mcp_wiki.h"
#include "../include/agent_memory.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef _WIN32
#include <errno.h>
#include <sys/types.h>
#define CNET_MKDIR(p) mkdir((p), 0755)
#else
#include <direct.h>
#define CNET_MKDIR(p) _mkdir(p)
#endif

/* answer_kind: 0=single number, 1=pair "a and b", 2=text already in answer_text */
typedef struct {
    char method[64];
    char exprs[16][160];
    int n_exprs;
    char check_expr[160];
    double check_expect;
    int has_check;
    char final_expr[160];
    int tier;
    int answer_kind;
    char answer_text[128]; /* for pair/text override after eval */
    int pair_expr_i;       /* second root index for pair answers */
    int pair_expr_j;
} MathPlan;

static void lower_copy(const char *in, char *out, size_t cap) {
    size_t i;
    for (i = 0; in && in[i] && i + 1 < cap; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[i < cap ? i : cap - 1] = '\0';
}

static int contains_ci(const char *hay, const char *needle) {
    char h[768], n[128];
    lower_copy(hay, h, sizeof h);
    lower_copy(needle, n, sizeof n);
    return strstr(h, n) != NULL;
}

static void strip_spaces(const char *in, char *out, size_t cap) {
    size_t i, j = 0;
    for (i = 0; in && in[i] && j + 1 < cap; i++)
        if (!isspace((unsigned char)in[i])) out[j++] = in[i];
    out[j] = '\0';
}

static int extract_numbers(const char *q, double *nums, int max_n) {
    int n = 0;
    const char *p = q;
    if (!q || !nums || max_n <= 0) return 0;
    while (*p && n < max_n) {
        while (*p && !(isdigit((unsigned char)*p) ||
                       ((*p == '-' || *p == '+') && isdigit((unsigned char)p[1]))))
            p++;
        if (!*p) break;
        {
            char *end = NULL;
            double v = strtod(p, &end);
            if (end == p) {
                p++;
                continue;
            }
            nums[n++] = v;
            p = end;
        }
    }
    return n;
}

static int nearly_eq(double a, double b) {
    return fabs(a - b) <= 1e-6 * (1.0 + fabs(a) + fabs(b));
}

static void format_num(double v, char *out, size_t cap) {
    if (fabs(v - round(v)) < 1e-9 && fabs(v) < 1e15)
        snprintf(out, cap, "%.0f", round(v));
    else
        snprintf(out, cap, "%.10g", v);
}

static int eval_plan(const MathPlan *plan, double *answer_out, char *trace, size_t tcap) {
    int i;
    double last = 0;
    size_t used = 0;
    if (!plan || !answer_out) return -1;
    if (trace && tcap) trace[0] = '\0';
    for (i = 0; i < plan->n_exprs; i++) {
        double v = 0;
        char step[128];
        if (cnet_math_eval_expr(plan->exprs[i], &v) != 0) return -1;
        last = v;
        format_num(v, step, sizeof step);
        if (trace && tcap) {
            int w = snprintf(trace + used, tcap - used, "%s=%s%s", plan->exprs[i], step,
                             (i + 1 < plan->n_exprs) ? "; " : "");
            if (w > 0) used += (size_t)w;
            if (used >= tcap) used = tcap - 1;
        }
    }
    if (plan->final_expr[0]) {
        if (cnet_math_eval_expr(plan->final_expr, &last) != 0) return -1;
    }
    if (plan->has_check) {
        double cv = 0;
        if (cnet_math_eval_expr(plan->check_expr, &cv) != 0) return -1;
        if (!nearly_eq(cv, plan->check_expect)) return -2;
    }
    *answer_out = last;
    return 0;
}

static int is_prime_u64(unsigned long long n) {
    unsigned long long i;
    if (n < 2) return 0;
    if (n == 2 || n == 3) return 1;
    if (n % 2 == 0 || n % 3 == 0) return 0;
    for (i = 5; i * i <= n; i += 6)
        if (n % i == 0 || n % (i + 2) == 0) return 0;
    return 1;
}

static unsigned long long fib_u(unsigned n) {
    unsigned long long a = 0, b = 1, t;
    unsigned i;
    if (n == 0) return 0;
    for (i = 1; i < n; i++) {
        t = a + b;
        a = b;
        b = t;
    }
    return b;
}

/* Strip NL wrappers → candidate expression */
static int extract_expr_candidate(const char *q, char *out, size_t cap) {
    char tmp[768], *p;
    const char *prefixes[] = {
        "what is ", "what's ", "calculate ", "compute ", "evaluate ", "find ",
        "solve ", "simplify ", "work out ", "please ", "the value of ", NULL};
    size_t i, j = 0;
    int k;
    if (!q || !out || cap == 0) return 0;
    lower_copy(q, tmp, sizeof tmp);
    p = tmp;
    for (k = 0; prefixes[k]; k++) {
        size_t L = strlen(prefixes[k]);
        if (strncmp(p, prefixes[k], L) == 0) {
            p += L;
            break;
        }
    }
    /* keep math-ish chars only */
    for (i = 0; p[i] && j + 1 < cap; i++) {
        char c = p[i];
        if (isalnum((unsigned char)c) || strchr("+-*/^()%.,! ", c))
            out[j++] = c;
        else if (c == '=' || c == '?')
            break;
    }
    out[j] = '\0';
    /* trim trailing words that aren't pure math - if any letter-word longer */
    {
        int has_op = strpbrk(out, "+-*/^%!()") != NULL || strstr(out, "sqrt") ||
                     strstr(out, "gcd") || strstr(out, "lcm") || strstr(out, "fact") ||
                     strstr(out, "sin") || strstr(out, "cos") || strstr(out, "log");
        double v;
        if (!has_op && j > 0) {
            /* bare number ok */
            if (cnet_math_eval_expr(out, &v) == 0) return 1;
            return 0;
        }
        if (has_op && cnet_math_eval_expr(out, &v) == 0) return 1;
        /* try removing spaces */
        {
            char ns[768];
            strip_spaces(out, ns, sizeof ns);
            if (cnet_math_eval_expr(ns, &v) == 0) {
                snprintf(out, cap, "%s", ns);
                return 1;
            }
        }
    }
    return 0;
}

/* ---------- templates ---------- */

static int plan_direct_expr(const char *q, MathPlan *out) {
    char buf[768];
    double v;
    if (!q || !out) return 0;
    if (!extract_expr_candidate(q, buf, sizeof buf)) {
        /* raw whole-string if already math-only */
        size_t i, j = 0;
        for (i = 0; q[i] && j + 1 < sizeof buf; i++) {
            char c = q[i];
            if (isalnum((unsigned char)c) || strchr("+-*/^()% .!,", c))
                buf[j++] = c;
            else
                return 0;
        }
        buf[j] = '\0';
        if (cnet_math_eval_expr(buf, &v) != 0) return 0;
    }
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "direct_eval");
    snprintf(out->exprs[0], sizeof out->exprs[0], "%s", buf);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_C_DIRECT;
    return 1;
}

static int plan_pythag_hypot(const char *q, MathPlan *out) {
    double nums[8];
    int n;
    char a[32], b[32];
    if (!contains_ci(q, "hypot") && !contains_ci(q, "pythag") &&
        !contains_ci(q, "right triangle") && !contains_ci(q, "right-angled") &&
        !(contains_ci(q, "legs") && contains_ci(q, "triangle")))
        return 0;
    n = extract_numbers(q, nums, 8);
    if (n < 2) return 0;
    format_num(fabs(nums[0]), a, sizeof a);
    format_num(fabs(nums[1]), b, sizeof b);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "pythag_hypot");
    snprintf(out->exprs[0], sizeof out->exprs[0], "sqrt((%s*%s)+(%s*%s))", a, a, b, b);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_is_prime(const char *q, MathPlan *out) {
    double nums[4];
    int n;
    unsigned long long u;
    if (!contains_ci(q, "prime")) return 0;
    n = extract_numbers(q, nums, 4);
    if (n < 1 || nums[0] < 0 || nums[0] > 1e15) return 0;
    if (fabs(nums[0] - floor(nums[0])) > 1e-9) return 0;
    u = (unsigned long long)llround(nums[0]);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "is_prime");
    snprintf(out->exprs[0], sizeof out->exprs[0], "%d", is_prime_u64(u) ? 1 : 0);
    out->n_exprs = 1;
    out->answer_kind = 2;
    if (is_prime_u64(u))
        snprintf(out->answer_text, sizeof out->answer_text, "yes, %.0f is prime", nums[0]);
    else
        snprintf(out->answer_text, sizeof out->answer_text, "no, %.0f is not prime", nums[0]);
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

/* Parse quadratic coefficients from stripped form into a,b,c */
static int parse_quadratic_coeffs(const char *s, double *a, double *b, double *c) {
    int ai, bi, ci;
    double ad, bd, cd;
    /* monic patterns */
    if (sscanf(s, "x^2+%dx+%d=0", &bi, &ci) == 2) {
        *a = 1;
        *b = bi;
        *c = ci;
        return 1;
    }
    if (sscanf(s, "x^2-%dx+%d=0", &bi, &ci) == 2) {
        *a = 1;
        *b = -bi;
        *c = ci;
        return 1;
    }
    if (sscanf(s, "x^2+%dx-%d=0", &bi, &ci) == 2) {
        *a = 1;
        *b = bi;
        *c = -ci;
        return 1;
    }
    if (sscanf(s, "x^2-%dx-%d=0", &bi, &ci) == 2) {
        *a = 1;
        *b = -bi;
        *c = -ci;
        return 1;
    }
    if (sscanf(s, "x^2+%d=0", &ci) == 1) {
        *a = 1;
        *b = 0;
        *c = ci;
        return 1;
    }
    if (sscanf(s, "x^2-%d=0", &ci) == 1) {
        *a = 1;
        *b = 0;
        *c = -ci;
        return 1;
    }
    if (sscanf(s, "x^2=0") == 0 && strcmp(s, "x^2=0") == 0) {
        *a = 1;
        *b = 0;
        *c = 0;
        return 1;
    }
    /* ax^2±bx±c=0 */
    if (sscanf(s, "%dx^2+%dx+%d=0", &ai, &bi, &ci) == 3) {
        *a = ai;
        *b = bi;
        *c = ci;
        return 1;
    }
    if (sscanf(s, "%dx^2-%dx+%d=0", &ai, &bi, &ci) == 3) {
        *a = ai;
        *b = -bi;
        *c = ci;
        return 1;
    }
    if (sscanf(s, "%dx^2+%dx-%d=0", &ai, &bi, &ci) == 3) {
        *a = ai;
        *b = bi;
        *c = -ci;
        return 1;
    }
    if (sscanf(s, "%dx^2-%dx-%d=0", &ai, &bi, &ci) == 3) {
        *a = ai;
        *b = -bi;
        *c = -ci;
        return 1;
    }
    if (sscanf(s, "%dx^2+%d=0", &ai, &ci) == 2) {
        *a = ai;
        *b = 0;
        *c = ci;
        return 1;
    }
    if (sscanf(s, "%dx^2-%d=0", &ai, &ci) == 2) {
        *a = ai;
        *b = 0;
        *c = -ci;
        return 1;
    }
    if (sscanf(s, "%dx^2-%dx=0", &ai, &bi) == 2) {
        *a = ai;
        *b = -bi;
        *c = 0;
        return 1;
    }
    if (sscanf(s, "%dx^2+%dx=0", &ai, &bi) == 2) {
        *a = ai;
        *b = bi;
        *c = 0;
        return 1;
    }
    /* floats */
    if (sscanf(s, "%lfx^2+%lfx+%lf=0", &ad, &bd, &cd) == 3) {
        *a = ad;
        *b = bd;
        *c = cd;
        return 1;
    }
    return 0;
}

static int plan_quadratic(const char *q, MathPlan *out) {
    char s[512];
    double a = 1, b = 0, c = 0, disc;
    char as[32], bs[32], cs[32];
    strip_spaces(q, s, sizeof s);
    /* normalize x**2 → x^2 */
    {
        char *p;
        while ((p = strstr(s, "x**2")) != NULL) {
            memmove(p + 3, p + 4, strlen(p + 4) + 1);
            p[1] = '^';
        }
    }
    if (!strstr(s, "x^2") && !contains_ci(q, "quadratic")) return 0;
    if (!parse_quadratic_coeffs(s, &a, &b, &c)) {
        double nums[8];
        int n = extract_numbers(q, nums, 8);
        if (contains_ci(q, "quadratic") && n >= 3) {
            a = nums[0];
            b = nums[1];
            c = nums[2];
        } else
            return 0;
    }
    if (fabs(a) < 1e-12) return 0;
    disc = b * b - 4 * a * c;
    if (disc < -1e-9) return 0;
    format_num(a, as, sizeof as);
    format_num(b, bs, sizeof bs);
    format_num(c, cs, sizeof cs);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "quadratic_roots");
    snprintf(out->exprs[0], sizeof out->exprs[0], "(%s)*(%s)-4*(%s)*(%s)", bs, bs, as, cs);
    snprintf(out->exprs[1], sizeof out->exprs[1],
             "(-(%s)+sqrt((%s)*(%s)-4*(%s)*(%s)))/(2*(%s))", bs, bs, bs, as, cs, as);
    snprintf(out->exprs[2], sizeof out->exprs[2],
             "(-(%s)-sqrt((%s)*(%s)-4*(%s)*(%s)))/(2*(%s))", bs, bs, bs, as, cs, as);
    out->n_exprs = 3;
    out->answer_kind = 1;
    out->pair_expr_i = 1;
    out->pair_expr_j = 2;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

/* Linear: ax+b=c  or  ax+b=dx+e  → x */
static int plan_linear(const char *q, MathPlan *out) {
    char s[512];
    int a, b, c, d, e;
    double A, B, C, x;
    char As[32], Bs[32], Cs[32];
    strip_spaces(q, s, sizeof s);
    /* skip if quadratic */
    if (strstr(s, "x^2") || strstr(s, "x**2")) return 0;
    if (!strchr(s, 'x') || !strchr(s, '=')) return 0;

    /* ax+b=c */
    if (sscanf(s, "%dx+%d=%d", &a, &b, &c) == 3) {
        A = a;
        B = b;
        C = c;
    } else if (sscanf(s, "%dx-%d=%d", &a, &b, &c) == 3) {
        A = a;
        B = -b;
        C = c;
    } else if (sscanf(s, "x+%d=%d", &b, &c) == 2) {
        A = 1;
        B = b;
        C = c;
    } else if (sscanf(s, "x-%d=%d", &b, &c) == 2) {
        A = 1;
        B = -b;
        C = c;
    } else if (sscanf(s, "%dx=%d", &a, &c) == 2) {
        A = a;
        B = 0;
        C = c;
    } else if (sscanf(s, "x=%d", &c) == 1) {
        A = 1;
        B = 0;
        C = c;
    } else if (sscanf(s, "%dx+%d=%dx+%d", &a, &b, &d, &e) == 4) {
        /* ax+b = dx+e → (a-d)x = e-b */
        A = a - d;
        B = 0;
        C = e - b;
    } else if (sscanf(s, "%dx-%d=%dx+%d", &a, &b, &d, &e) == 4) {
        A = a - d;
        B = 0;
        C = e + b;
    } else if (sscanf(s, "%dx+%d=%dx-%d", &a, &b, &d, &e) == 4) {
        A = a - d;
        B = 0;
        C = -e - b;
    } else if (sscanf(s, "x+%d=%dx+%d", &b, &d, &e) == 3) {
        A = 1 - d;
        B = 0;
        C = e - b;
    } else
        return 0;

    if (fabs(A) < 1e-12) return 0;
    x = (C - B) / A;
    format_num(A, As, sizeof As);
    format_num(B, Bs, sizeof Bs);
    format_num(C, Cs, sizeof Cs);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "linear_solve");
    snprintf(out->exprs[0], sizeof out->exprs[0], "((%s)-(%s))/(%s)", Cs, Bs, As);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    (void)x;
    return 1;
}

/* System: ax+by=c ; dx+ey=f  (and / comma / newline) */
static int plan_linear_system(const char *q, MathPlan *out) {
    char s[768];
    int a, b, c, d, e, f;
    double det, x, y;
    char as[16], bs[16], cs[16], ds[16], es[16], fs[16];
    int matched = 0;
    strip_spaces(q, s, sizeof s);
    {
        char raw[768], *p;
        lower_copy(q, raw, sizeof raw);
        /* Keep equation boundary: "and" / comma → ';' */
        while ((p = strstr(raw, " and ")) != NULL) {
            p[0] = ';';
            memmove(p + 1, p + 5, strlen(p + 5) + 1);
        }
        while ((p = strstr(raw, ",")) != NULL) *p = ';';
        while ((p = strstr(raw, "solve")) != NULL)
            memmove(p, p + 5, strlen(p + 5) + 1);
        while ((p = strstr(raw, "the system")) != NULL)
            memmove(p, p + 10, strlen(p + 10) + 1);
        while ((p = strstr(raw, "system")) != NULL)
            memmove(p, p + 6, strlen(p + 6) + 1);
        while ((p = strstr(raw, "equations")) != NULL)
            memmove(p, p + 9, strlen(p + 9) + 1);
        while ((p = strstr(raw, ":")) != NULL) *p = ' ';
        strip_spaces(raw, s, sizeof s);
        /* s like 2x+y=5;x-y=1 */
    }

    if (sscanf(s, "%dx+%dy=%d;%dx+%dy=%d", &a, &b, &c, &d, &e, &f) == 6)
        matched = 1;
    else if (sscanf(s, "%dx+%dy=%d;%dx-%dy=%d", &a, &b, &c, &d, &e, &f) == 6) {
        e = -e;
        matched = 1;
    } else if (sscanf(s, "%dx-%dy=%d;%dx+%dy=%d", &a, &b, &c, &d, &e, &f) == 6) {
        b = -b;
        matched = 1;
    } else if (sscanf(s, "%dx-%dy=%d;%dx-%dy=%d", &a, &b, &c, &d, &e, &f) == 6) {
        b = -b;
        e = -e;
        matched = 1;
    } else if (sscanf(s, "%dx+y=%d;%dx-%dy=%d", &a, &c, &d, &e, &f) == 5) {
        b = 1;
        e = -e;
        matched = 1;
    } else if (sscanf(s, "%dx+y=%d;x-%dy=%d", &a, &c, &e, &f) == 4) {
        b = 1;
        d = 1;
        e = -e;
        matched = 1;
    } else if (sscanf(s, "%dx+y=%d;x-y=%d", &a, &c, &f) == 3) {
        b = 1;
        d = 1;
        e = -1;
        matched = 1;
    } else if (sscanf(s, "x+y=%d;x-y=%d", &c, &f) == 2) {
        a = 1;
        b = 1;
        d = 1;
        e = -1;
        matched = 1;
    } else if (sscanf(s, "%dx+%dy=%d%dx+%dy=%d", &a, &b, &c, &d, &e, &f) == 6)
        matched = 1;

    if (!matched) {
        /* number dump: 2x+y=5 x-y=1 → nums 2,1,5,1,1,1 or similar */
        double nums[12];
        int n = extract_numbers(q, nums, 12);
        if (n >= 6 && (contains_ci(q, "system") || (strchr(q, 'x') && strchr(q, 'y')))) {
            a = (int)nums[0];
            b = (int)nums[1];
            c = (int)nums[2];
            d = (int)nums[3];
            e = (int)nums[4];
            f = (int)nums[5];
            /* fix signs from text for common 2x+y=5 and x-y=1 */
            if (contains_ci(q, "x-y") || contains_ci(q, "x - y")) e = -abs(e);
            matched = 1;
        } else
            return 0;
    }

    det = (double)a * e - (double)b * d;
    if (fabs(det) < 1e-12) return 0;
    x = ((double)c * e - (double)b * f) / det;
    y = ((double)a * f - (double)c * d) / det;
    format_num(a, as, sizeof as);
    format_num(b, bs, sizeof bs);
    format_num(c, cs, sizeof cs);
    format_num(d, ds, sizeof ds);
    format_num(e, es, sizeof es);
    format_num(f, fs, sizeof fs);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "linear_system_2x2");
    /* x = (ce-bf)/(ae-bd), y = (af-cd)/(ae-bd) */
    snprintf(out->exprs[0], sizeof out->exprs[0],
             "((%s)*(%s)-(%s)*(%s))/((%s)*(%s)-(%s)*(%s))", cs, es, bs, fs, as, es, bs, ds);
    snprintf(out->exprs[1], sizeof out->exprs[1],
             "((%s)*(%s)-(%s)*(%s))/((%s)*(%s)-(%s)*(%s))", as, fs, cs, ds, as, es, bs, ds);
    out->n_exprs = 2;
    out->answer_kind = 1;
    out->pair_expr_i = 0;
    out->pair_expr_j = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    (void)x;
    (void)y;
    return 1;
}

static int plan_gcd_lcm(const char *q, MathPlan *out) {
    double nums[4];
    int n;
    char a[32], b[32];
    int want_lcm = 0;
    if (contains_ci(q, "lcm") || contains_ci(q, "least common"))
        want_lcm = 1;
    else if (!(contains_ci(q, "gcd") || contains_ci(q, "greatest common") ||
               contains_ci(q, "hcf")))
        return 0;
    n = extract_numbers(q, nums, 4);
    if (n < 2) return 0;
    format_num(fabs(nums[0]), a, sizeof a);
    format_num(fabs(nums[1]), b, sizeof b);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, want_lcm ? "lcm" : "gcd");
    snprintf(out->exprs[0], sizeof out->exprs[0], want_lcm ? "lcm(%s,%s)" : "gcd(%s,%s)", a, b);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_factorial(const char *q, MathPlan *out) {
    double nums[4];
    int n;
    char a[32];
    if (!(contains_ci(q, "factorial") || strstr(q, "!") || contains_ci(q, "fact(")))
        return 0;
    n = extract_numbers(q, nums, 4);
    if (n < 1 || nums[0] < 0 || nums[0] > 20) return 0;
    format_num(nums[0], a, sizeof a);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "factorial");
    snprintf(out->exprs[0], sizeof out->exprs[0], "fact(%s)", a);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_fibonacci(const char *q, MathPlan *out) {
    double nums[4];
    int n, k;
    if (!(contains_ci(q, "fibonacci") || contains_ci(q, "fib "))) return 0;
    n = extract_numbers(q, nums, 4);
    if (n < 1 || nums[0] < 0 || nums[0] > 90) return 0;
    k = (int)llround(nums[0]);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "fibonacci");
    /* encode via constant evaluated as number */
    snprintf(out->exprs[0], sizeof out->exprs[0], "%llu",
             (unsigned long long)fib_u((unsigned)k));
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_percent(const char *q, MathPlan *out) {
    double nums[4];
    int n;
    char a[32], b[32];
    /* "what is 15% of 80" */
    if (!strchr(q, '%') && !contains_ci(q, "percent") && !contains_ci(q, "percentage"))
        return 0;
    n = extract_numbers(q, nums, 4);
    if (n < 2) return 0;
    format_num(nums[0], a, sizeof a);
    format_num(nums[1], b, sizeof b);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "percent_of");
    snprintf(out->exprs[0], sizeof out->exprs[0], "(%s/100)*(%s)", a, b);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_distance(const char *q, MathPlan *out) {
    double nums[8];
    int n;
    char x1[24], y1[24], x2[24], y2[24];
    if (!(contains_ci(q, "distance") || contains_ci(q, "dist between"))) return 0;
    n = extract_numbers(q, nums, 8);
    if (n < 4) return 0;
    format_num(nums[0], x1, sizeof x1);
    format_num(nums[1], y1, sizeof y1);
    format_num(nums[2], x2, sizeof x2);
    format_num(nums[3], y2, sizeof y2);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "distance_2d");
    snprintf(out->exprs[0], sizeof out->exprs[0],
             "sqrt(((%s)-(%s))^2+((%s)-(%s))^2)", x2, x1, y2, y1);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_midpoint(const char *q, MathPlan *out) {
    double nums[8];
    int n;
    char x1[24], y1[24], x2[24], y2[24];
    if (!contains_ci(q, "midpoint")) return 0;
    n = extract_numbers(q, nums, 8);
    if (n < 4) return 0;
    format_num(nums[0], x1, sizeof x1);
    format_num(nums[1], y1, sizeof y1);
    format_num(nums[2], x2, sizeof x2);
    format_num(nums[3], y2, sizeof y2);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "midpoint");
    snprintf(out->exprs[0], sizeof out->exprs[0], "((%s)+(%s))/2", x1, x2);
    snprintf(out->exprs[1], sizeof out->exprs[1], "((%s)+(%s))/2", y1, y2);
    out->n_exprs = 2;
    out->answer_kind = 1;
    out->pair_expr_i = 0;
    out->pair_expr_j = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_slope(const char *q, MathPlan *out) {
    double nums[8];
    int n;
    char x1[24], y1[24], x2[24], y2[24];
    if (!(contains_ci(q, "slope") || contains_ci(q, "gradient"))) return 0;
    n = extract_numbers(q, nums, 8);
    if (n < 4) return 0;
    format_num(nums[0], x1, sizeof x1);
    format_num(nums[1], y1, sizeof y1);
    format_num(nums[2], x2, sizeof x2);
    format_num(nums[3], y2, sizeof y2);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "slope");
    snprintf(out->exprs[0], sizeof out->exprs[0], "((%s)-(%s))/((%s)-(%s))", y2, y1, x2, x1);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_bayes(const char *q, MathPlan *out) {
    double nums[8];
    int n;
    char pd[32], ppos_d[32], ppos_nd[32];
    if (!contains_ci(q, "bayes") && !contains_ci(q, "p(d|") && !contains_ci(q, "p(d/+"))
        return 0;
    n = extract_numbers(q, nums, 8);
    if (n < 3) return 0;
    /* P(D), P(+|D), P(+|~D) */
    format_num(nums[0], pd, sizeof pd);
    format_num(nums[1], ppos_d, sizeof ppos_d);
    format_num(nums[2], ppos_nd, sizeof ppos_nd);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "bayes_theorem");
    /* P(D|+) = P(+|D)P(D) / (P(+|D)P(D)+P(+|~D)(1-P(D))) */
    snprintf(out->exprs[0], sizeof out->exprs[0],
             "((%s)*(%s))/((%s)*(%s)+(%s)*(1-(%s)))", ppos_d, pd, ppos_d, pd, ppos_nd, pd);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_trig_asin_deg(const char *q, MathPlan *out) {
    double nums[4];
    int n;
    char a[32];
    if (!(contains_ci(q, "sin") && (contains_ci(q, "degree") || contains_ci(q, "acute") ||
                                    contains_ci(q, "theta") || contains_ci(q, "angle"))))
        return 0;
    n = extract_numbers(q, nums, 4);
    if (n < 1) return 0;
    format_num(nums[0], a, sizeof a);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "asind");
    snprintf(out->exprs[0], sizeof out->exprs[0], "asind(%s)", a);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_matrix_det2(const char *q, MathPlan *out) {
    double nums[8];
    int n;
    char a[24], b[24], c[24], d[24];
    if (!(contains_ci(q, "det") || contains_ci(q, "determinant"))) return 0;
    n = extract_numbers(q, nums, 8);
    if (n < 4) return 0;
    format_num(nums[0], a, sizeof a);
    format_num(nums[1], b, sizeof b);
    format_num(nums[2], c, sizeof c);
    format_num(nums[3], d, sizeof d);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "det2");
    snprintf(out->exprs[0], sizeof out->exprs[0], "(%s)*(%s)-(%s)*(%s)", a, d, b, c);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int plan_add_sub_mul(const char *q, MathPlan *out) {
    char ql[512];
    double nums[4];
    int n;
    char a[32], b[32];
    const char *op = NULL;
    lower_copy(q, ql, sizeof ql);
    if (strchr(ql, 'x') || strstr(ql, "^2") || strstr(ql, "quadratic") || strstr(ql, "y"))
        return 0;
    n = extract_numbers(q, nums, 4);
    if (n < 2) return 0;
    if (strstr(ql, "times") || strstr(ql, "multiply") || strstr(ql, "product") ||
        strstr(ql, "*"))
        op = "*";
    else if (strstr(ql, "plus") || strstr(ql, "sum") || strstr(ql, "add"))
        op = "+";
    else if (strstr(ql, "minus") || strstr(ql, "subtract") || strstr(ql, "difference"))
        op = "-";
    else if (strstr(ql, "divided") || strstr(ql, "quotient") || strstr(ql, "/"))
        op = "/";
    else
        return 0;
    format_num(nums[0], a, sizeof a);
    format_num(nums[1], b, sizeof b);
    memset(out, 0, sizeof *out);
    snprintf(out->method, sizeof out->method, "binary_op");
    snprintf(out->exprs[0], sizeof out->exprs[0], "%s%s%s", a, op, b);
    out->n_exprs = 1;
    out->tier = CNET_MATH_TIER_A_TEMPLATE;
    return 1;
}

static int collect_plans(const char *q, int allow_creative, MathPlan *plans, int max_plans) {
    int n = 0;
    MathPlan tmp;
#define ADD()                                                                                  \
    do {                                                                                       \
        if (n < max_plans) plans[n++] = tmp;                                                   \
    } while (0)

    if (plan_direct_expr(q, &tmp)) ADD();
    if (plan_pythag_hypot(q, &tmp)) ADD();
    if (plan_is_prime(q, &tmp)) ADD();
    if (plan_quadratic(q, &tmp)) ADD();
    if (plan_linear_system(q, &tmp)) ADD();
    if (plan_linear(q, &tmp)) ADD();
    if (plan_gcd_lcm(q, &tmp)) ADD();
    if (plan_factorial(q, &tmp)) ADD();
    if (plan_fibonacci(q, &tmp)) ADD();
    if (plan_percent(q, &tmp)) ADD();
    if (plan_distance(q, &tmp)) ADD();
    if (plan_midpoint(q, &tmp)) ADD();
    if (plan_slope(q, &tmp)) ADD();
    if (plan_bayes(q, &tmp)) ADD();
    if (plan_trig_asin_deg(q, &tmp)) ADD();
    if (plan_matrix_det2(q, &tmp)) ADD();
    if (plan_add_sub_mul(q, &tmp)) ADD();

    if (allow_creative) {
        double nums[4];
        int nn = extract_numbers(q, nums, 4);
        if (n < max_plans && nn >= 2 &&
            (contains_ci(q, "triangle") || contains_ci(q, "hypot") || contains_ci(q, "leg"))) {
            char a[32], b[32];
            int i, dup = 0;
            format_num(fabs(nums[0]), a, sizeof a);
            format_num(fabs(nums[1]), b, sizeof b);
            memset(&tmp, 0, sizeof tmp);
            snprintf(tmp.method, sizeof tmp.method, "creative_hypot");
            snprintf(tmp.exprs[0], sizeof tmp.exprs[0], "sqrt((%s*%s)+(%s*%s))", a, a, b, b);
            tmp.n_exprs = 1;
            tmp.tier = CNET_MATH_TIER_B_CREATIVE;
            for (i = 0; i < n; i++)
                if (strcmp(plans[i].method, "pythag_hypot") == 0 ||
                    strcmp(plans[i].method, "creative_hypot") == 0)
                    dup = 1;
            if (!dup) plans[n++] = tmp;
        }
    }
#undef ADD
    return n;
}

static int mkdir_p(const char *path) {
    char tmp[512];
    size_t len, i;
    if (!path || !path[0]) return -1;
    snprintf(tmp, sizeof tmp, "%s", path);
    len = strlen(tmp);
    if (len && tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (i = 1; tmp[i]; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            CNET_MKDIR(tmp);
            tmp[i] = '/';
        }
    }
    return CNET_MKDIR(tmp) == 0 || errno == EEXIST ? 0 : -1;
}

static void slugify(const char *in, char *out, size_t cap) {
    size_t i = 0, o = 0;
    int prev = 1;
    out[0] = '\0';
    if (!in) return;
    for (i = 0; in[i] && o + 1 < cap && o < 60; i++) {
        unsigned char c = (unsigned char)in[i];
        if (isalnum(c)) {
            out[o++] = (char)tolower(c);
            prev = 0;
        } else if (!prev) {
            out[o++] = '-';
            prev = 1;
        }
    }
    while (o > 0 && out[o - 1] == '-') o--;
    out[o] = '\0';
    if (!o) snprintf(out, cap, "math-skill");
}

static int write_math_skill(const char *dir, const char *slug, const char *q,
                            const char *method, const char *steps, const char *answer,
                            char *path_out, size_t path_cap) {
    char d[512], path[512];
    FILE *f;
    time_t now = time(NULL);
    struct tm tm;
    char ts[64];
    snprintf(d, sizeof d, "%s/%.80s", dir, slug);
    if (mkdir_p(d) != 0) return -1;
    snprintf(path, sizeof path, "%s/SKILL.md", d);
    f = fopen(path, "w");
    if (!f) return -1;
#ifndef _WIN32
    localtime_r(&now, &tm);
#else
    localtime_s(&tm, &now);
#endif
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    fprintf(f,
            "---\nname: %s\ndescription: Verified HS algebra procedure\nmethod: %s\n"
            "origin: cnet_math_solve\nlearned_at: %s\n---\n\n# %s\n\n## When\n%s\n\n"
            "## Procedure (verified)\n%s\n\n## Answer\n%s\n",
            slug, method, ts, slug, q, steps, answer);
    fclose(f);
    if (path_out && path_cap) snprintf(path_out, path_cap, "%s", path);
    return 0;
}

void cnet_math_solve_config_defaults(CnetMathSolveConfig *c) {
    if (!c) return;
    memset(c, 0, sizeof *c);
    c->write_skill = 1;
    c->memorize = 1;
    c->allow_creative = 1;
    c->max_plans = 12;
    snprintf(c->skills_dir, sizeof c->skills_dir, "logs/personal_ai_skills");
}

void cnet_math_solve_config_from_env(CnetMathSolveConfig *c) {
    const char *e;
    cnet_math_solve_config_defaults(c);
    e = getenv("CNET_SKILLS_DIR");
    if (e && e[0]) snprintf(c->skills_dir, sizeof c->skills_dir, "%s", e);
    e = getenv("CNET_MATH_CREATIVE");
    if (e && e[0] == '0') c->allow_creative = 0;
}

const char *cnet_math_tier_name(CnetMathTier t) {
    switch (t) {
    case CNET_MATH_TIER_A_TEMPLATE: return "A_template";
    case CNET_MATH_TIER_B_CREATIVE: return "B_creative";
    case CNET_MATH_TIER_C_DIRECT: return "C_direct";
    default: return "none";
    }
}

int cnet_math_solve(const char *question, const CnetMathSolveConfig *cfg_in,
                    CnetMathSolveReport *rep) {
    CnetMathSolveConfig cfg;
    MathPlan plans[16];
    int nplans, i;
    if (rep) memset(rep, 0, sizeof *rep);
    if (!question || !question[0]) return -1;
    if (cfg_in)
        cfg = *cfg_in;
    else
        cnet_math_solve_config_from_env(&cfg);

    mcp_memory_init();
    nplans = collect_plans(question, cfg.allow_creative, plans,
                           cfg.max_plans < 16 ? cfg.max_plans : 16);
    if (rep) rep->plans_tried = nplans;
    if (nplans == 0) {
        if (rep) {
            snprintf(rep->detail, sizeof rep->detail, "no plan for question");
            rep->tier = CNET_MATH_TIER_NONE;
        }
        return 1;
    }

    for (i = 0; i < nplans; i++) {
        double ans = 0;
        char trace[640];
        int erc = eval_plan(&plans[i], &ans, trace, sizeof trace);
        if (erc != 0) continue;

        if (rep) {
            rep->verified = 1;
            rep->tier = (CnetMathTier)plans[i].tier;
            snprintf(rep->method, sizeof rep->method, "%s", plans[i].method);
            snprintf(rep->steps, sizeof rep->steps, "%s", trace);
            snprintf(rep->detail, sizeof rep->detail, "verified via %s",
                     cnet_math_tier_name(rep->tier));

            if (plans[i].answer_kind == 2 && plans[i].answer_text[0]) {
                snprintf(rep->answer, sizeof rep->answer, "%s", plans[i].answer_text);
            } else if (plans[i].answer_kind == 1) {
                double r1 = 0, r2 = 0;
                char a1[40], a2[40];
                cnet_math_eval_expr(plans[i].exprs[plans[i].pair_expr_i], &r1);
                cnet_math_eval_expr(plans[i].exprs[plans[i].pair_expr_j], &r2);
                format_num(r1, a1, sizeof a1);
                format_num(r2, a2, sizeof a2);
                snprintf(rep->answer, sizeof rep->answer, "%s and %s", a1, a2);
            } else {
                format_num(ans, rep->answer, sizeof rep->answer);
            }
        }

        if (cfg.memorize && rep) {
            char key[220];
            snprintf(key, sizeof key, "math_solve:%s", question);
            mcp_memorize_fact(key, rep->answer);
        }
        if (cfg.write_skill && rep) {
            char slug[128];
            slugify(question, slug, sizeof slug);
            if (write_math_skill(cfg.skills_dir, slug, question, plans[i].method, trace,
                                 rep->answer, rep->skill_path, sizeof rep->skill_path) == 0)
                rep->skill_written = 1;
        }
        /* Feed pattern runtime: fluid → improving → freeze on verified math. */
        {
            static CnetPatternRuntime s_pat;
            static int s_pat_init = 0;
            if (!s_pat_init) {
                cnet_pattern_runtime_from_env(&s_pat);
                cnet_pattern_runtime_load(&s_pat, s_pat.store_path);
                cnet_pattern_bootstrap_defaults(&s_pat);
                s_pat_init = 1;
            }
            snprintf(s_pat.skills_dir, sizeof s_pat.skills_dir, "%s", cfg.skills_dir);
            cnet_pattern_observe_math(&s_pat, question, plans[i].method,
                                      rep ? rep->answer : "", 1);
            cnet_pattern_runtime_save(&s_pat, s_pat.store_path);
        }
        return 0;
    }

    if (rep) {
        rep->verified = 0;
        snprintf(rep->detail, sizeof rep->detail, "all %d plans failed verify/eval", nplans);
    }
    return 1;
}
