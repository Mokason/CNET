/* Core metric honesty invariants (from tests/test_metric_honesty.py).
 *
 * Hermetic: gap partition, real_miss_rate formula, Hermes envelope classifier.
 * Prints METRIC_HONESTY_PASS (Makefile greps this).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>

static int failures;

static void expect(int ok, const char *msg) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", msg);
        failures++;
    }
}

/* Ledger partition matching governor_autonomous TestParsersAgree._governor_parse */
static void partition_gaps(const char *text, int *open_n, int *def_n, int *closed_n) {
    const char *p = text;
    int lineno = 0;
    *open_n = *def_n = *closed_n = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        char line[512];
        size_t L;
        char *toks[4];
        int nt = 0;
        char buf[512];
        char *q;
        lineno++;
        L = eol ? (size_t)(eol - p) : strlen(p);
        if (L >= sizeof line) L = sizeof line - 1;
        memcpy(line, p, L);
        line[L] = 0;
        p = eol ? eol + 1 : p + L;
        if (lineno < 3) continue;
        if (strstr(line, "waiting_oracle") || strstr(line, "waiting_charter")) {
            (*def_n)++;
            continue;
        }
        snprintf(buf, sizeof buf, "%s", line);
        q = buf;
        while (nt < 4 && *q) {
            while (*q == ' ' || *q == '\t') q++;
            if (!*q) break;
            toks[nt++] = q;
            while (*q && *q != ' ' && *q != '\t') q++;
            if (*q) *q++ = 0;
        }
        if (nt > 1 && strcmp(toks[1], "2") == 0)
            (*closed_n)++;
        else if (nt > 1 && strcmp(toks[1], "1") == 0)
            (*open_n)++;
    }
}

static double real_miss_rate(int closed, int open_, int waiting) {
    int total = closed + open_ + waiting;
    int outstanding = open_ + waiting;
    if (total <= 0) return 0.0;
    return (double)outstanding / (double)total;
}

static void lower_copy(const char *in, char *out, size_t cap) {
    size_t i;
    for (i = 0; in[i] && i + 1 < cap; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = 0;
}

static int json_str_empty(const char *obj, const char *key) {
    char pat[64];
    const char *p, *q;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(obj, pat);
    if (!p) return 1;
    p = strchr(p + strlen(pat), ':');
    if (!p) return 1;
    while (*p && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (*p == '"') {
        p++;
        q = strchr(p, '"');
        return !q || q == p;
    }
    if (strncmp(p, "null", 4) == 0 || strncmp(p, "false", 5) == 0) return 1;
    return 0;
}

static int json_status_is(const char *obj, const char *want) {
    const char *p = strstr(obj, "\"status\"");
    char val[64];
    size_t n;
    const char *q;
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    while (*p && (*p == ':' || *p == ' ')) p++;
    if (*p != '"') return 0;
    p++;
    q = strchr(p, '"');
    if (!q) return 0;
    n = (size_t)(q - p);
    if (n >= sizeof val) n = sizeof val - 1;
    memcpy(val, p, n);
    val[n] = 0;
    for (n = 0; val[n]; n++) val[n] = (char)tolower((unsigned char)val[n]);
    return strcmp(val, want) == 0;
}

static void hermes_classify(const char *text, char *out, size_t cap) {
    char low[2048];
    const char *s = text ? text : "";
    while (*s == ' ' || *s == '\t') s++;
    if (s[0] == '{') {
        if (json_status_is(s, "pending_approval") || json_status_is(s, "pending")) {
            snprintf(out, cap, "skip");
            return;
        }
        if (json_status_is(s, "error") || json_status_is(s, "failed")) {
            snprintf(out, cap, "fail");
            return;
        }
        if (!json_str_empty(s, "error")) {
            snprintf(out, cap, "fail");
            return;
        }
        {
            const char *p = strstr(s, "\"exit_code\"");
            if (p) {
                int code = atoi(strchr(p, ':') + 1);
                if (code != 0) {
                    snprintf(out, cap, "fail");
                    return;
                }
            }
        }
        snprintf(out, cap, "ok");
        return;
    }
    lower_copy(s, low, sizeof low);
    if (strstr(low, "traceback (most recent call last)")) {
        snprintf(out, cap, "fail");
        return;
    }
    snprintf(out, cap, "ok");
}

static void build_gaps(char *out, size_t cap, int closed, int open_, int waiting) {
    size_t used = 0;
    int n = 0, i;
    used += (size_t)snprintf(out + used, cap - used, "CNET_GAPS 4\n%d\n",
                             closed + open_ + waiting);
    for (i = 0; i < closed && used + 80 < cap; i++) {
        n++;
        used += (size_t)snprintf(out + used, cap - used,
                                 "0 2 1 1 1 256 1 w_cur 1 256 3 tk%dq%d - cce_cond_next - - 0 0\n",
                                 n, n);
    }
    for (i = 0; i < open_ && used + 80 < cap; i++) {
        n++;
        used += (size_t)snprintf(out + used, cap - used,
                                 "0 1 1 1 1 256 1 w_cur 1 256 3 tk%dq%d - cce_cond_next - - 0 0\n",
                                 n, n);
    }
    for (i = 0; i < waiting && used + 100 < cap; i++) {
        n++;
        used += (size_t)snprintf(
            out + used, cap - used,
            "0 1 1 1 1 256 1 w_cur 1 256 3 tk%dq%d - cce_cond_next waiting_oracle - 0 0\n",
            n, n);
    }
}

int main(void) {
    char ledger[256 * 1024];
    char verdict[16];
    int o, d, c;
    double r;
    int i, fails;
    failures = 0;

    /* Miss rate denominator */
    expect(fabs(real_miss_rate(500, 0, 0) - 0.0) < 1e-9, "all closed is zero");
    expect(fabs(real_miss_rate(990, 0, 10) - 0.01) < 1e-6, "successes in denominator");
    expect(real_miss_rate(990, 0, 10) < 0.05, "99% closed not high miss");
    expect(fabs(real_miss_rate(90, 0, 10) - 0.10) < 1e-6, "waiting not double-counted rate");
    {
        double r0 = real_miss_rate(10, 0, 10);
        double r1 = real_miss_rate(100, 0, 10);
        double r2 = real_miss_rate(1000, 0, 10);
        expect(r0 > r1 && r1 > r2, "monotonic in closed gaps");
    }
    expect(fabs(real_miss_rate(0, 5, 5) - 1.0) < 1e-9, "rate bounded at 1");

    /* Parsers partition */
    build_gaps(ledger, sizeof ledger, 200, 15, 7);
    partition_gaps(ledger, &o, &d, &c);
    expect(o == 15 && d == 7 && c == 200, "partition 200/15/7");
    expect(o + d + c == 222, "buckets sum to rows");
    r = real_miss_rate(c, o, d);
    expect(fabs(r - (22.0 / 222.0)) < 1e-9, "outstanding rate matches partition");

    /* Hermes classifier */
    hermes_classify("{\"output\":\"hello\",\"error\":\"\",\"exit_code\":0}", verdict,
                    sizeof verdict);
    expect(strcmp(verdict, "ok") == 0, "empty error is success");
    hermes_classify(
        "{\"output\":\"grep -c error app.log\\n0 errors found\",\"error\":\"\",\"exit_code\":0}",
        verdict, sizeof verdict);
    expect(strcmp(verdict, "ok") == 0, "word error in output still ok");
    hermes_classify(
        "{\"output\":\"PERSONALITY_SELFTEST_PASS checks=6\",\"error\":\"\",\"exit_code\":0}",
        verdict, sizeof verdict);
    expect(strcmp(verdict, "ok") == 0, "selftest pass is success");
    hermes_classify("{\"output\":\"\",\"error\":\"BLOCKED: timed out\",\"exit_code\":-1}",
                     verdict, sizeof verdict);
    expect(strcmp(verdict, "fail") == 0, "populated error is failure");
    hermes_classify("{\"output\":\"\",\"error\":\"\",\"exit_code\":1}", verdict, sizeof verdict);
    expect(strcmp(verdict, "fail") == 0, "nonzero exit is failure");
    hermes_classify(
        "{\"output\":\"\",\"error\":\"\",\"exit_code\":-1,\"status\":\"pending_approval\"}",
        verdict, sizeof verdict);
    expect(strcmp(verdict, "skip") == 0, "pending is skip");
    fails = 0;
    for (i = 0; i < 50; i++) {
        char payload[128];
        snprintf(payload, sizeof payload,
                 "{\"output\":\"line %d\\nerror: none\",\"error\":\"\",\"exit_code\":0}", i);
        hermes_classify(payload, verdict, sizeof verdict);
        if (strcmp(verdict, "fail") == 0) fails++;
    }
    expect(fails == 0, "success corpus yields zero fails");
    hermes_classify("plain tool output, nothing wrong", verdict, sizeof verdict);
    expect(strcmp(verdict, "ok") == 0, "non-json defaults ok");
    hermes_classify("Traceback (most recent call last):\n  File ...", verdict, sizeof verdict);
    expect(strcmp(verdict, "fail") == 0, "traceback is failure");

    /* real_miss must not be max()'d with hermes — formula uses gaps only */
    expect(fabs(real_miss_rate(990, 0, 10) - 0.01) < 1e-6 &&
               fabs(real_miss_rate(990, 0, 10) - 0.9) > 0.5,
           "real_miss independent of hermes 0.9 noise");

    if (failures) {
        printf("METRIC_HONESTY_FAIL\n");
        return 1;
    }
    printf("Ran 18 tests\n");
    printf("METRIC_HONESTY_PASS\n");
    return 0;
}
