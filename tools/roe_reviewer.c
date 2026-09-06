/* ROE external REVIEWER — pure C.
 *
 * Teacher proposes. Reviewer gates. Never the same role in one step.
 *
 *   bin/roe_reviewer <query> <candidate_answer>
 *     -> prints exactly one line: "APPROVE: <reason>" or "REJECT: <reason>"
 *     -> exit 0 on a decision, 1 on a hard usage error
 *
 * This is the ROE_REVIEWER_BIN contract consumed by bin/roe_evolve_tick. It
 * replaces the retired tools/roe_reviewer.py: CNET product path is C only.
 * HTTP is done by shelling out to curl (no interpreter, no new link deps).
 *
 * FAIL CLOSED: any transport error, empty body, or unparseable verdict is a
 * REJECT when ROE_REVIEW_FAIL_CLOSED is 1 (the default). A reviewer that
 * cannot reach its model must never wave an answer through to CERT.
 *
 * Env (config/roe-reviewer-ollama-cloud.env is auto-loaded when present):
 *   ROE_REVIEW_URL         default http://127.0.0.1:11434/api/generate
 *   ROE_REVIEW_MODEL       default deepseek-v4-flash:cloud
 *   ROE_REVIEW_THINK       default 0
 *   ROE_REVIEW_TIMEOUT_MS  default 120000
 *   ROE_REVIEW_FAIL_CLOSED default 1
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define RPATH 1024
#define RBUF  16384
static const char *env_or(const char *k, const char *d) {
    const char *v = getenv(k);
    return (v && v[0]) ? v : d;
}

static int env_truthy(const char *k, int dflt) {
    const char *v = getenv(k);
    if (!v || !v[0]) return dflt;
    return (!strcmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes"));
}

/* KEY=VALUE lines; existing environment wins (setenv overwrite=0). */
static void load_env_file(const char *path) {
    FILE *f = fopen(path, "r");
    char line[1024];
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq, *k, *v, *e;
        line[strcspn(line, "\r\n")] = 0;
        k = line;
        while (*k && isspace((unsigned char)*k)) k++;
        if (!*k || *k == '#') continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        v = eq + 1;
        for (e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        while (*v && isspace((unsigned char)*v)) v++;
        for (e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        if ((*v == '"' && e > v + 1 && e[-1] == '"') ||
            (*v == '\'' && e > v + 1 && e[-1] == '\'')) { v++; e[-1] = 0; }
        if (*k) setenv(k, v, 0);
    }
    fclose(f);
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0, i;
    for (i = 0; in && in[i] && o + 8 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) { snprintf(out + o, 8, "\\u%04x", c); o += 6; }
            else out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

/* Extract a top-level "key":"..." string value, decoding common escapes. */
static int json_str(const char *body, const char *key, char *out, size_t cap) {
    char pat[64];
    const char *p;
    size_t o = 0;
    out[0] = 0;
    if (snprintf(pat, sizeof pat, "\"%s\"", key) >= (int)sizeof pat) return -1;
    p = strstr(body, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return -1;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"' && o + 2 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[o++] = '\n'; break;
            case 't': out[o++] = '\t'; break;
            case 'r': out[o++] = '\r'; break;
            case 'u': {
                int k;
                for (k = 1; k <= 4 && isxdigit((unsigned char)p[k]); k++) ;
                if (k == 5) { p += 4; out[o++] = ' '; } else out[o++] = 'u';
                break;
            }
            default: out[o++] = *p; break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    return 0;
}

static int sh_quote(const char *in, char *out, size_t cap) {
    size_t o = 0, i;
    if (cap < 3) return -1;
    out[o++] = '\'';
    for (i = 0; in[i]; i++) {
        if (in[i] == '\'') {
            if (o + 4 >= cap) return -1;
            memcpy(out + o, "'\\''", 4);
            o += 4;
        } else {
            if (o + 2 >= cap) return -1;
            out[o++] = in[i];
        }
    }
    out[o++] = '\'';
    out[o] = 0;
    return 0;
}

static const char *REVIEW_SYSTEM =
    "You are the ROE-ASI external REVIEWER (not the teacher, not the user). "
    "Judge ONLY whether CANDIDATE_ANSWER is acceptable to cache as a short LOCAL skill. "
    "Reply with EXACTLY one line: APPROVE: <reason> OR REJECT: <reason>. "
    "No markdown. No other text.";

static void decide(const char *text, char *verdict, size_t cap, int *approved) {
    const char *a = strstr(text, "APPROVE");
    const char *r = strstr(text, "REJECT");
    *approved = 0;
    /* REJECT wins when both appear — fail closed. */
    if (r) { snprintf(verdict, cap, "REJECT: %.180s", r); return; }
    if (a) { snprintf(verdict, cap, "APPROVE: %.180s", a + 8); *approved = 1; return; }
    snprintf(verdict, cap, "REJECT: unparseable reviewer output");
}

static int selftest(void) {
    char v[256], esc[128];
    int ok, fails = 0, checks = 0;
#define CHK(c, n) do { checks++; printf("  %-46s %s\n", (n), (c) ? "PASS" : "FAIL"); \
                       if (!(c)) fails++; } while (0)
    printf("=== roe_reviewer selftest ===\n");
    decide("APPROVE: short and on topic", v, sizeof v, &ok);
    CHK(ok, "APPROVE parsed");
    decide("REJECT: off topic", v, sizeof v, &ok);
    CHK(!ok, "REJECT parsed");
    decide("APPROVE but also REJECT", v, sizeof v, &ok);
    CHK(!ok, "REJECT wins when both present");
    decide("mumble", v, sizeof v, &ok);
    CHK(!ok, "unparseable is not approved");
    decide("", v, sizeof v, &ok);
    CHK(!ok, "empty is not approved");
    json_escape("a\"b\nc", esc, sizeof esc);
    CHK(!strcmp(esc, "a\\\"b\\nc"), "json escape");
    {
        char out[64];
        CHK(json_str("{\"response\":\"APPROVE: fine\"}", "response", out, sizeof out) == 0 &&
            !strcmp(out, "APPROVE: fine"), "json_str extracts response");
    }
    printf("\nchecks=%d failures=%d\n", checks, fails);
    if (fails) { printf("ROE_REVIEWER_FAIL\n"); return 1; }
    printf("ROE_REVIEWER_SELFTEST_PASS\n");
    return 0;
#undef CHK
}

int main(int argc, char **argv) {
    char prompt[RBUF], eprompt[RBUF * 2], body[RBUF * 2 + 512];
    char qq[RBUF], aa[RBUF], cmd[RBUF * 4], resp[RBUF], text[RBUF], verdict[256];
    const char *url, *model;
    int approved = 0, timeout_s, think, fail_closed;
    FILE *p;
    size_t n;
    int rc;

    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
    if (argc < 3) {
        fprintf(stderr, "usage: roe_reviewer <query> <candidate_answer>\n"
                        "       roe_reviewer --selftest\n");
        return 1;
    }

    load_env_file("config/roe-reviewer-ollama-cloud.env");
    load_env_file("config/roe-teacher-ollama-cloud.env");
    url   = env_or("ROE_REVIEW_URL", env_or("ROE_LLM_URL", "http://127.0.0.1:11434/api/generate"));
    model = env_or("ROE_REVIEW_MODEL", env_or("ROE_LLM_MODEL", "deepseek-v4-flash:cloud"));
    think = env_truthy("ROE_REVIEW_THINK", 0);
    fail_closed = env_truthy("ROE_REVIEW_FAIL_CLOSED", 1);
    timeout_s = atoi(env_or("ROE_REVIEW_TIMEOUT_MS", "120000")) / 1000;
    if (timeout_s < 5) timeout_s = 5;

    snprintf(prompt, sizeof prompt,
             "%s\n\nQUERY: %.1000s\nCANDIDATE_ANSWER: %.4000s\n\n"
             "APPROVE if: short, on-topic, factual-or-procedural, no seal/CERT claim, "
             "no second-brain/overmind claim, no empty fluff.\n"
             "REJECT if: empty, off-topic, claims self-CERT/seal without gate, "
             "second brain, unsafe, or clearly wrong for the query.\n"
             "When unsure but answer is short and harmless and related: APPROVE.\n"
             "Verdict:",
             REVIEW_SYSTEM, argv[1], argv[2]);

    json_escape(prompt, eprompt, sizeof eprompt);
    snprintf(body, sizeof body,
             "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":false,\"think\":%s,"
             "\"options\":{\"temperature\":0.0,\"num_predict\":128}}",
             model, eprompt, think ? "true" : "false");

    if (sh_quote(body, aa, sizeof aa) != 0 || sh_quote(url, qq, sizeof qq) != 0) {
        printf("REJECT: reviewer_request_too_large\n");
        return 0;
    }
    snprintf(cmd, sizeof cmd,
             "curl -sS --max-time %d -H 'Content-Type: application/json' "
             "-H 'User-Agent: CNET-ROE-REVIEW/1.0' -X POST -d %s %s 2>/dev/null",
             timeout_s, aa, qq);

    resp[0] = 0;
    p = popen(cmd, "r");
    if (!p) {
        printf("REJECT: reviewer_spawn_failed\n");
        return 0;
    }
    n = fread(resp, 1, sizeof resp - 1, p);
    resp[n] = 0;
    rc = pclose(p);

    if (rc != 0 || !resp[0]) {
        printf("REJECT: reviewer_transport_error (fail_closed=%d)\n", fail_closed);
        return 0;
    }
    if (json_str(resp, "response", text, sizeof text) != 0 || !text[0]) {
        if (json_str(resp, "thinking", text, sizeof text) != 0 || !text[0]) {
            printf("REJECT: reviewer_empty_response\n");
            return 0;
        }
    }
    decide(text, verdict, sizeof verdict, &approved);
    {
        char *nl;
        for (nl = verdict; *nl; nl++) if (*nl == '\n' || *nl == '\r') *nl = ' ';
    }
    printf("%s\n", verdict);
    return 0;
}
