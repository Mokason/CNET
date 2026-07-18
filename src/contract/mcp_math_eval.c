#include "../../include/contract/mcp_math_eval.h"
#include "../../include/contract/mcp_wiki.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *p;
    int err;
} MathParse;

static void skip_ws(MathParse *m) {
    while (m->p[0] && isspace((unsigned char)m->p[0])) m->p++;
}

static int match_char(MathParse *m, char c) {
    skip_ws(m);
    if (m->p[0] == c) {
        m->p++;
        return 1;
    }
    return 0;
}

static double parse_expr(MathParse *m);
static double parse_term(MathParse *m);
static double parse_power(MathParse *m);
static double parse_unary(MathParse *m);
static double parse_primary(MathParse *m);

static int starts_ident(const char *p) {
    return isalpha((unsigned char)p[0]) || p[0] == '_';
}

static int consume_ident(MathParse *m, char *buf, size_t cap) {
    size_t n = 0;
    skip_ws(m);
    if (!starts_ident(m->p)) return 0;
    while ((isalnum((unsigned char)m->p[0]) || m->p[0] == '_') && n + 1 < cap) {
        buf[n++] = (char)tolower((unsigned char)m->p[0]);
        m->p++;
    }
    buf[n] = '\0';
    return 1;
}

static double parse_number(MathParse *m) {
    char *end = NULL;
    double v;
    skip_ws(m);
    v = strtod(m->p, &end);
    if (end == m->p) {
        m->err = 1;
        return 0;
    }
    m->p = end;
    return v;
}

static unsigned long long gcd_ull(unsigned long long a, unsigned long long b) {
    while (b) {
        unsigned long long t = b;
        b = a % b;
        a = t;
    }
    return a;
}

static double fact_d(double n) {
    unsigned long long u, i, f = 1;
    if (n < 0 || n > 20 || fabs(n - floor(n)) > 1e-9) return NAN;
    u = (unsigned long long)llround(n);
    for (i = 2; i <= u; i++) f *= i;
    return (double)f;
}

static double parse_primary(MathParse *m) {
    char id[32];
    double arg, arg2;
    int has2 = 0;
    skip_ws(m);
    if (match_char(m, '(')) {
        double v = parse_expr(m);
        if (!match_char(m, ')')) m->err = 1;
        /* postfix ! */
        skip_ws(m);
        if (m->p[0] == '!') {
            m->p++;
            v = fact_d(v);
            if (!isfinite(v)) m->err = 1;
        }
        return v;
    }
    if (starts_ident(m->p)) {
        if (!consume_ident(m, id, sizeof id)) {
            m->err = 1;
            return 0;
        }
        if (strcmp(id, "pi") == 0) return 3.14159265358979323846;
        if (strcmp(id, "e") == 0) return 2.71828182845904523536;
        if (!match_char(m, '(')) {
            m->err = 1;
            return 0;
        }
        arg = parse_expr(m);
        skip_ws(m);
        if (match_char(m, ',')) {
            arg2 = parse_expr(m);
            has2 = 1;
        }
        if (!match_char(m, ')')) m->err = 1;
        if (m->err) return 0;

        if (!has2) {
            if (strcmp(id, "sqrt") == 0) {
                if (arg < 0) {
                    m->err = 1;
                    return 0;
                }
                return sqrt(arg);
            }
            if (strcmp(id, "abs") == 0) return fabs(arg);
            if (strcmp(id, "floor") == 0) return floor(arg);
            if (strcmp(id, "ceil") == 0) return ceil(arg);
            if (strcmp(id, "round") == 0) return round(arg);
            if (strcmp(id, "ln") == 0 || strcmp(id, "log") == 0) {
                if (arg <= 0) {
                    m->err = 1;
                    return 0;
                }
                return log(arg);
            }
            if (strcmp(id, "log10") == 0) {
                if (arg <= 0) {
                    m->err = 1;
                    return 0;
                }
                return log10(arg);
            }
            if (strcmp(id, "sin") == 0) return sin(arg);
            if (strcmp(id, "cos") == 0) return cos(arg);
            if (strcmp(id, "tan") == 0) return tan(arg);
            if (strcmp(id, "asin") == 0) return asin(arg);
            if (strcmp(id, "acos") == 0) return acos(arg);
            if (strcmp(id, "atan") == 0) return atan(arg);
            if (strcmp(id, "sind") == 0) return sin(arg * 3.14159265358979323846 / 180.0);
            if (strcmp(id, "cosd") == 0) return cos(arg * 3.14159265358979323846 / 180.0);
            if (strcmp(id, "tand") == 0) return tan(arg * 3.14159265358979323846 / 180.0);
            if (strcmp(id, "asind") == 0)
                return asin(arg) * 180.0 / 3.14159265358979323846;
            if (strcmp(id, "fact") == 0 || strcmp(id, "factorial") == 0) {
                double f = fact_d(arg);
                if (!isfinite(f)) m->err = 1;
                return f;
            }
            m->err = 1;
            return 0;
        }
        /* two-arg */
        if (strcmp(id, "pow") == 0) return pow(arg, arg2);
        if (strcmp(id, "min") == 0) return arg < arg2 ? arg : arg2;
        if (strcmp(id, "max") == 0) return arg > arg2 ? arg : arg2;
        if (strcmp(id, "gcd") == 0) {
            if (arg < 0) arg = -arg;
            if (arg2 < 0) arg2 = -arg2;
            if (fabs(arg - floor(arg)) > 1e-9 || fabs(arg2 - floor(arg2)) > 1e-9) {
                m->err = 1;
                return 0;
            }
            return (double)gcd_ull((unsigned long long)llround(arg),
                                   (unsigned long long)llround(arg2));
        }
        if (strcmp(id, "lcm") == 0) {
            unsigned long long a, b, g;
            if (arg < 0) arg = -arg;
            if (arg2 < 0) arg2 = -arg2;
            if (fabs(arg - floor(arg)) > 1e-9 || fabs(arg2 - floor(arg2)) > 1e-9) {
                m->err = 1;
                return 0;
            }
            a = (unsigned long long)llround(arg);
            b = (unsigned long long)llround(arg2);
            if (a == 0 || b == 0) return 0;
            g = gcd_ull(a, b);
            return (double)(a / g * b);
        }
        if (strcmp(id, "mod") == 0) {
            if (arg2 == 0) {
                m->err = 1;
                return 0;
            }
            return fmod(arg, arg2);
        }
        if (strcmp(id, "atan2") == 0) return atan2(arg, arg2);
        m->err = 1;
        return 0;
    }
    {
        double v = parse_number(m);
        skip_ws(m);
        if (m->p[0] == '!') {
            m->p++;
            v = fact_d(v);
            if (!isfinite(v)) m->err = 1;
        }
        return v;
    }
}

static double parse_unary(MathParse *m) {
    skip_ws(m);
    if (match_char(m, '+')) return parse_unary(m);
    if (match_char(m, '-')) return -parse_unary(m);
    return parse_primary(m);
}

static double parse_power(MathParse *m) {
    double base = parse_unary(m);
    skip_ws(m);
    if (m->p[0] == '^') {
        double expv;
        m->p++;
        expv = parse_power(m);
        if (m->err) return 0;
        return pow(base, expv);
    }
    return base;
}

static double parse_term(MathParse *m) {
    double v = parse_power(m);
    for (;;) {
        skip_ws(m);
        if (match_char(m, '*')) {
            v *= parse_power(m);
        } else if (match_char(m, '/')) {
            double d = parse_power(m);
            if (d == 0.0) {
                m->err = 1;
                return 0;
            }
            v /= d;
        } else if (match_char(m, '%')) {
            double d = parse_power(m);
            if (d == 0.0) {
                m->err = 1;
                return 0;
            }
            v = fmod(v, d);
        } else {
            break;
        }
        if (m->err) return 0;
    }
    return v;
}

static double parse_expr(MathParse *m) {
    double v = parse_term(m);
    for (;;) {
        skip_ws(m);
        if (match_char(m, '+')) {
            v += parse_term(m);
        } else if (match_char(m, '-')) {
            v -= parse_term(m);
        } else {
            break;
        }
        if (m->err) return 0;
    }
    return v;
}

int cnet_math_eval_expr(const char *expr, double *out_value) {
    MathParse m;
    double v;
    char cleaned[768];
    size_t i, j = 0;
    if (!expr || !out_value) return -1;
    for (i = 0; expr[i] && j + 1 < sizeof cleaned; i++) {
        unsigned char c = (unsigned char)expr[i];
        if (c == '\n' || c == '\r') continue;
        /* allow ** as ^ */
        if (c == '*' && expr[i + 1] == '*') {
            cleaned[j++] = '^';
            i++;
            continue;
        }
        cleaned[j++] = (char)c;
    }
    cleaned[j] = '\0';
    m.p = cleaned;
    m.err = 0;
    v = parse_expr(&m);
    skip_ws(&m);
    if (m.err || m.p[0] != '\0') return -1;
    if (!isfinite(v)) return -1;
    *out_value = v;
    return 0;
}

static void format_num(double v, char *out, size_t cap) {
    if (!out || cap == 0) return;
    if (fabs(v - round(v)) < 1e-9 && fabs(v) < 1e15)
        snprintf(out, cap, "%.0f", round(v));
    else
        snprintf(out, cap, "%.10g", v);
}

int port_contract_mcp_math_eval(
    const char *expr,
    char *result_out, size_t cap,
    int *from_mem)
{
    char key[200];
    double v = 0;
    if (!expr || !result_out || cap == 0 || !from_mem) return -1;
    *from_mem = 0;
    result_out[0] = '\0';
    if (snprintf(key, sizeof key, "math_eval:%s", expr) >= (int)sizeof key) {
        snprintf(result_out, cap, "EXPR_TOO_LONG");
        return 0;
    }
    if (mcp_recall_fact(key, result_out, cap)) {
        *from_mem = 1;
        return 0;
    }
    if (cnet_math_eval_expr(expr, &v) != 0) {
        snprintf(result_out, cap, "EVAL_ERROR");
        return 0;
    }
    format_num(v, result_out, cap);
    mcp_memorize_fact(key, result_out);
    return 0;
}
