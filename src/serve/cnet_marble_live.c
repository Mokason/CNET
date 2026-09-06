/* Marble Live wiring — see include/cnet_marble_live.h */
#define _POSIX_C_SOURCE 200809L
#include "cnet_marble_live.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

static int ml_is_dir(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int ml_mkdir_p(const char *path) {
    char tmp[CNET_ML_PATH];
    char *p;
    if (!path || !path[0]) return -1;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s", path) >= sizeof tmp) return -1;
    for (p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && !ml_is_dir(tmp)) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && !ml_is_dir(tmp)) return -1;
    return 0;
}

int cnet_ml_episodic_path(char *out, size_t cap, const char *fallback_root) {
    const char *env = getenv("CNET_EPISODIC_PATH");
    const char *min = getenv("CNET_MINIMAL_ROOT");
    char dir[CNET_ML_PATH];
    char *slash;
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (env && env[0]) {
        if ((size_t)snprintf(out, cap, "%s", env) >= cap) return -1;
    } else if (min && min[0]) {
        if ((size_t)snprintf(out, cap, "%s/var/marble_episodic.jsonl", min) >= cap) return -1;
    } else if (fallback_root && fallback_root[0]) {
        if ((size_t)snprintf(out, cap, "%s/var/marble_episodic.jsonl", fallback_root) >= cap)
            return -1;
    } else {
        if ((size_t)snprintf(out, cap, "var/marble_episodic.jsonl") >= cap) return -1;
    }
    if ((size_t)snprintf(dir, sizeof dir, "%s", out) >= sizeof dir) return -1;
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (dir[0] && ml_mkdir_p(dir) != 0) return -1;
    }
    return 0;
}

int cnet_ml_sanitize_unit(const char *in, char *out, size_t cap) {
    size_t o = 0;
    int last_us = 1;   /* suppress a leading underscore */
    if (!out || cap < 2) return -1;
    out[0] = 0;
    if (!in) return -1;
    for (; *in && o + 1 < cap && o < CNET_ML_UNIT - 1; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c)) {
            out[o++] = (char)tolower(c);
            last_us = 0;
        } else if (c == '-' || c == '_' || isspace(c)) {
            if (!last_us && o) { out[o++] = '_'; last_us = 1; }
        }
        /* everything else (quotes, ;, /, $, backticks) is dropped outright */
    }
    while (o > 0 && out[o - 1] == '_') o--;
    out[o] = 0;
    return out[0] ? 0 : -1;
}

int cnet_ml_extract_unit(const char *query, char *out, size_t cap) {
    static const char *leads[] = {"propose capsule for ", "propose capsule ",
                                  "make a capsule for ", "make a capsule ",
                                  "export capsule for ", "export capsule ", NULL};
    char low[CNET_ML_TEXT];
    size_t i;
    int k;
    if (!out || cap == 0) return -1;
    out[0] = 0;
    if (!query) return -1;
    for (i = 0; query[i] && i + 1 < sizeof low; i++)
        low[i] = (char)tolower((unsigned char)query[i]);
    low[i] = 0;
    for (k = 0; leads[k]; k++) {
        const char *p = strstr(low, leads[k]);
        if (!p) continue;
        p += strlen(leads[k]);
        while (*p == ' ') p++;
        if (!*p) continue;
        return cnet_ml_sanitize_unit(p, out, cap);
    }
    return -1;
}

/* Shell-single-quote; used only on values already through sanitize_unit. */
static int ml_shq(const char *in, char *out, size_t cap) {
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

static int ml_run(const char *cmd, char *out, size_t cap) {
    FILE *p = popen(cmd, "r");
    size_t n;
    int rc;
    if (out && cap) out[0] = 0;
    if (!p) return -1;
    n = out && cap ? fread(out, 1, cap - 1, p) : 0;
    if (out && cap) out[n] = 0;
    rc = pclose(p);
    return rc;
}

int cnet_ml_propose_capsule(const char *unit, char *out, size_t cap) {
    char clean[CNET_ML_UNIT], q[CNET_ML_UNIT * 4], cmd[CNET_ML_PATH * 2];
    const char *bin = getenv("CNET_CAPSULE_PROPOSE_BIN");
    char raw[CNET_ML_TEXT];
    int rc;
    if (out && cap) out[0] = 0;
    /* Re-sanitise even if the caller already did: this is the argv door. */
    if (cnet_ml_sanitize_unit(unit, clean, sizeof clean) != 0) return -1;
    if (!bin || !bin[0]) bin = "bin/cnet_capsule_propose";
    if (ml_shq(clean, q, sizeof q) != 0) return -1;
    if ((size_t)snprintf(cmd, sizeof cmd, "%s --unit %s 2>&1", bin, q) >= sizeof cmd)
        return -1;
    rc = ml_run(cmd, raw, sizeof raw);
    if (out && cap) {
        char *nl = strstr(raw, "CAPSULE_PROPOSE ");
        snprintf(out, cap, "%s", nl ? nl : raw);
        for (nl = out; *nl; nl++)
            if (*nl == '\n' || *nl == '\r') { *nl = 0; break; }
    }
    return rc == 0 ? 0 : -1;
}

int cnet_ml_stage_enabled(void) {
    const char *v = getenv("CNET_STAGE_RESIDUAL");
    return v && v[0] == '1' && v[1] == 0;
}

static int ml_ci(const char *h, const char *n) {
    size_t i, j, nl, hl;
    if (!h || !n || !n[0]) return 0;
    nl = strlen(n);
    hl = strlen(h);
    if (nl > hl) return 0;
    for (i = 0; i + nl <= hl; i++) {
        for (j = 0; j < nl; j++) {
            if (tolower((unsigned char)h[i + j]) != tolower((unsigned char)n[j]))
                break;
        }
        if (j == nl) return 1;
    }
    return 0;
}

static void ml_json_get(const char *line, const char *key, char *out, size_t cap) {
    char pat[80];
    const char *p;
    size_t i = 0;
    out[0] = 0;
    snprintf(pat, sizeof pat, "\"%s\":\"", key);
    p = strstr(line, pat);
    if (!p) return;
    p += strlen(pat);
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            out[i++] = *p++;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = 0;
}

int cnet_ml_context_pack(const char *query, char *out, size_t cap) {
    const char *min, *packs, *kbe;
    char path[CNET_ML_PATH];
    char line[2048], pat[256], ans[512];
    FILE *f;
    int n = 0;
    size_t o = 0;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!query || !query[0]) return 0;

    /* Standing host frame — residual otherwise invents a generic assistant
     * or the consumer news site. Not a sealed FAQ; claimed_cert stays 0. */
    o += (size_t)snprintf(out + o, cap - o,
                          "[c%d] host Marble on this machine. This CNET is Mokason's "
                          "CERT/ASI pack system, not the news site. Speak as that peer. "
                          "Sealed skills are CERT; this draft is not. Do not list generic "
                          "cloud-chatbot features.\n",
                          n + 1);
    n++;

    kbe = getenv("CNET_KNOWLEDGE_PATH");
    min = getenv("CNET_MINIMAL_ROOT");

    /* Last episodic notes — recall-before-talk. claimed_cert stays 0. */
    {
        char epath[CNET_ML_PATH];
        const char *ep = getenv("CNET_EPISODIC_PATH");
        char last[2][240];
        int ln = 0, ei;
        if (ep && ep[0])
            snprintf(epath, sizeof epath, "%s", ep);
        else {
            epath[0] = 0;
            (void)cnet_ml_episodic_path(epath, sizeof epath, min);
        }
        if (epath[0]) {
            f = fopen(epath, "r");
            if (f) {
                while (fgets(line, sizeof line, f)) {
                    char note[240];
                    ml_json_get(line, "note", note, sizeof note);
                    if (!note[0]) continue;
                    if (ln < 2) {
                        snprintf(last[ln], sizeof last[ln], "%s", note);
                        ln++;
                    } else {
                        snprintf(last[0], sizeof last[0], "%s", last[1]);
                        snprintf(last[1], sizeof last[1], "%s", note);
                    }
                }
                fclose(f);
                for (ei = 0; ei < ln && n < 8 && o + 80 < cap; ei++) {
                    o += (size_t)snprintf(out + o, cap - o,
                                          "[c%d] episodic %s\n", n + 1,
                                          last[ei]);
                    n++;
                }
            }
        }
    }

    if (kbe && kbe[0])
        snprintf(path, sizeof path, "%s", kbe);
    else if (min && min[0])
        snprintf(path, sizeof path, "%s/var/marble_knowledge.jsonl", min);
    else
        snprintf(path, sizeof path, "var/marble_knowledge.jsonl");
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f) && n < 4) {
            ml_json_get(line, "chunk", ans, sizeof ans);
            if (!ans[0]) continue;
            {
                const char *q = query;
                int hit = ml_ci(ans, query) || ml_ci(query, ans);
                char tok[48];
                int ti = 0;
                while (*q && !hit) {
                    if (isalnum((unsigned char)*q)) {
                        if (ti + 1 < (int)sizeof tok) tok[ti++] = *q;
                    } else {
                        tok[ti] = 0;
                        if (ti >= 4 && ml_ci(ans, tok)) hit = 1;
                        ti = 0;
                    }
                    q++;
                }
                tok[ti] = 0;
                if (ti >= 4 && ml_ci(ans, tok)) hit = 1;
                if (!hit) continue;
            }
            o += (size_t)snprintf(out + o, cap - o, "[c%d] note %s\n", n + 1, ans);
            n++;
            if (o + 80 >= cap) break;
        }
        fclose(f);
    }

    packs = getenv("CNET_PACKS_ROOT");
    if (packs && packs[0])
        snprintf(path, sizeof path, "%s/pack_english_basic/catalog.jsonl", packs);
    else if (min && min[0])
        snprintf(path, sizeof path,
                 "%s/data/roe_daily_packs/pack_english_basic/catalog.jsonl", min);
    else
        snprintf(path, sizeof path,
                 "artifacts/roe_daily_packs/pack_english_basic/catalog.jsonl");
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f) && n < 6) {
            ml_json_get(line, "pattern", pat, sizeof pat);
            ml_json_get(line, "answer", ans, sizeof ans);
            if (!ans[0]) continue;
            if (!(ml_ci(query, pat) || ml_ci(query, ans) || ml_ci(pat, query))) {
                continue;
            }
            o += (size_t)snprintf(out + o, cap - o, "[c%d] english %s\n", n + 1, ans);
            n++;
            if (o + 80 >= cap) break;
        }
        fclose(f);
    }

    /* Soul identity snippets when the query overlaps the pack. */
    if (packs && packs[0])
        snprintf(path, sizeof path, "%s/pack_soul_marble/catalog.jsonl", packs);
    else if (min && min[0])
        snprintf(path, sizeof path,
                 "%s/data/roe_daily_packs/pack_soul_marble/catalog.jsonl", min);
    else
        snprintf(path, sizeof path,
                 "artifacts/roe_daily_packs/pack_soul_marble/catalog.jsonl");
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f) && n < 8) {
            ml_json_get(line, "pattern", pat, sizeof pat);
            ml_json_get(line, "answer", ans, sizeof ans);
            if (!ans[0]) continue;
            if (!(ml_ci(query, pat) || ml_ci(query, ans) || ml_ci(pat, query) ||
                  ml_ci(query, "you") || ml_ci(query, "what can")))
                continue;
            /* Only attach soul rows that actually match, except identity asks. */
            if (!(ml_ci(query, pat) || ml_ci(query, ans) || ml_ci(pat, query))) {
                if (!(ml_ci(query, "who are") || ml_ci(query, "what can") ||
                      ml_ci(query, "who is") || ml_ci(query, "yourself")))
                    continue;
            }
            o += (size_t)snprintf(out + o, cap - o, "[c%d] soul %s\n", n + 1, ans);
            n++;
            if (o + 80 >= cap) break;
        }
        fclose(f);
    }
    return n;
}

int cnet_ml_stage_draft(const char *query, char *out, size_t cap) {
    return cnet_ml_stage_draft_ctx(query, NULL, out, cap);
}

/* Residual stage: improv draft only, and only the caller's miss path may ask.
 * Deliberately does NOT reuse cnet_held_model_* — that global endpoint is CERT
 * plumbing shared with other planes, and the stage must not be able to steer
 * it. Separate transport keeps the two provably apart. */
int cnet_ml_stage_draft_ctx(const char *query, const char *ctx, char *out, size_t cap) {
    char url[CNET_ML_PATH];
    char esc[CNET_ML_TEXT * 2];
    char ctxesc[CNET_ML_TEXT * 2];
    char payload[CNET_ML_TEXT * 6];
    char pq[CNET_ML_TEXT * 8], cmd[CNET_ML_TEXT * 10];
    char resp[CNET_ML_TEXT * 8];
    const char *base, *model;
    const char *p;
    size_t o = 0, i;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    if (!cnet_ml_stage_enabled()) return 0;
    if (!query || !query[0]) return 0;

    base = getenv("CNET_STAGE_HTTP");
    if (!base || !base[0]) base = getenv("CNET_RESIDUAL_HTTP");
    if (!base || !base[0]) base = "http://127.0.0.1:8081";
    if (strstr(base, "/v1/") || strstr(base, "/completion"))
        snprintf(url, sizeof url, "%s", base);
    else
        snprintf(url, sizeof url, "%s/v1/chat/completions", base);
    model = getenv("CNET_STAGE_MODEL");
    if (!model || !model[0]) model = "held";

    o = 0;
    for (i = 0; query[i] && o + 8 < sizeof esc; i++) {
        unsigned char c = (unsigned char)query[i];
        if (c == '"' || c == '\\') { esc[o++] = '\\'; esc[o++] = (char)c; }
        else if (c == '\n' || c == '\r') { esc[o++] = ' '; }
        else if (c < 0x20) { o += (size_t)snprintf(esc + o, 8, "\\u%04x", c); }
        else esc[o++] = (char)c;
    }
    esc[o] = 0;
    o = 0;
    ctxesc[0] = 0;
    if (ctx && ctx[0]) {
        for (i = 0; ctx[i] && o + 8 < sizeof ctxesc; i++) {
            unsigned char c = (unsigned char)ctx[i];
            if (c == '"' || c == '\\') { ctxesc[o++] = '\\'; ctxesc[o++] = (char)c; }
            else if (c == '\n' || c == '\r') { ctxesc[o++] = ' '; }
            else if (c < 0x20) continue;
            else ctxesc[o++] = (char)c;
        }
        ctxesc[o] = 0;
    }

    if (ctxesc[0]) {
        snprintf(payload, sizeof payload,
                 "{\"model\":\"%s\",\"messages\":["
                 "{\"role\":\"system\",\"content\":\"You are Marble. Residual mouth, never CERT. "
                 "If they ask you to pick, pick and say why in one breath. Else riff or ask back. "
                 "Do not explain facts, products, or architecture. Do not mention CNET, CERT, ASI, "
                 "ROE, Mokason, Brain, or floors. Never claim sealed knowledge.\"},"
                 "{\"role\":\"user\",\"content\":\"CONTEXT: %s  QUERY: %s\"}],"
                 "\"temperature\":0.6,\"max_tokens\":220,\"stream\":false}",
                 model, ctxesc, esc);
    } else {
        snprintf(payload, sizeof payload,
                 "{\"model\":\"%s\",\"messages\":["
                 "{\"role\":\"system\",\"content\":\"You are Marble. Residual play mouth only. "
                 "If they ask you to pick, pick one and say why in one breath. Do not refuse the choice. "
                 "Riff in English. Never explain encyclopedias, products, or architecture. Never mention "
                 "CNET, CERT, ASI, ROE, Mokason, Brain, floors, or hashtables. 1-3 short sentences.\"},"
                 "{\"role\":\"user\",\"content\":\"%s\"}],"
                 "\"temperature\":0.7,\"max_tokens\":220,\"stream\":false}",
                 model, esc);
    }
    if (ml_shq(payload, pq, sizeof pq) != 0) return 0;
    if ((size_t)snprintf(cmd, sizeof cmd,
                         "curl -sS --max-time %s -H 'Content-Type: application/json' "
                         "-X POST -d %s '%s' 2>/dev/null",
                         getenv("CNET_STAGE_TIMEOUT") ? getenv("CNET_STAGE_TIMEOUT") : "20",
                         pq, url) >= sizeof cmd)
        return 0;
    if (ml_run(cmd, resp, sizeof resp) != 0 || !resp[0]) return 0;

    /* pull "content":"..." out of the first choice */
    p = strstr(resp, "\"content\"");
    if (!p) return 0;
    p = strchr(p, ':');
    if (!p) return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return 0;
    p++;
    o = 0;
    while (*p && *p != '"' && o + 2 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': case 'r': case 't': out[o++] = ' '; break;
            case 'u':
                if (isxdigit((unsigned char)p[1])) { p += 4; out[o++] = ' '; }
                else out[o++] = 'u';
                break;
            default: out[o++] = *p; break;
            }
            p++;
        } else {
            out[o++] = *p++;
        }
    }
    out[o] = 0;
    while (o > 0 && (out[o - 1] == ' ')) out[--o] = 0;
    return out[0] ? 1 : 0;
}

int cnet_ml_selftest(void) {
    char b[CNET_ML_PATH];
    int fail = 0, n = 0;
#define T(ok, m) do { n++; printf("  %-56s %s\n", m, (ok) ? "PASS" : "FAIL"); \
                      if (!(ok)) fail++; } while (0)
    printf("=== marble_live wiring selftest ===\n");

    /* argv door: chat is untrusted, nothing but [a-z0-9_] may survive */
    T(cnet_ml_sanitize_unit("gap health", b, sizeof b) == 0 && !strcmp(b, "gap_health"),
      "unit: spaces -> underscore");
    T(cnet_ml_sanitize_unit("Gap-Health", b, sizeof b) == 0 && !strcmp(b, "gap_health"),
      "unit: case folded, dash kept as separator");
    T(cnet_ml_sanitize_unit("a; rm -rf /", b, sizeof b) == 0 &&
          strchr(b, ';') == NULL && strchr(b, '/') == NULL && strchr(b, ' ') == NULL,
      "unit: shell metacharacters stripped");
    T(cnet_ml_sanitize_unit("$(id)`id`", b, sizeof b) == 0 &&
          strchr(b, '$') == NULL && strchr(b, '`') == NULL && strchr(b, '(') == NULL,
      "unit: command substitution stripped");
    T(cnet_ml_sanitize_unit("...", b, sizeof b) != 0, "unit: punctuation-only rejected");
    T(cnet_ml_sanitize_unit("", b, sizeof b) != 0, "unit: empty rejected");

    T(cnet_ml_extract_unit("propose capsule for gap health", b, sizeof b) == 0 &&
          !strcmp(b, "gap_health"), "extract: propose capsule for X");
    T(cnet_ml_extract_unit("please make a capsule for dec add", b, sizeof b) == 0 &&
          !strcmp(b, "dec_add"), "extract: make a capsule for X");
    T(cnet_ml_extract_unit("who are you", b, sizeof b) != 0,
      "extract: unrelated query yields no unit");

    /* stage is off unless explicitly turned on, and off means no draft */
    unsetenv("CNET_STAGE_RESIDUAL");
    T(!cnet_ml_stage_enabled(), "stage: disabled by default");
    T(cnet_ml_stage_draft("anything", b, sizeof b) == 0 && b[0] == 0,
      "stage: disabled produces no draft");
    setenv("CNET_STAGE_RESIDUAL", "0", 1);
    T(!cnet_ml_stage_enabled(), "stage: CNET_STAGE_RESIDUAL=0 stays off");
    setenv("CNET_STAGE_RESIDUAL", "1", 1);
    T(cnet_ml_stage_enabled(), "stage: CNET_STAGE_RESIDUAL=1 turns it on");
    setenv("CNET_STAGE_HTTP", "http://127.0.0.1:9/v1/chat/completions", 1);
    T(cnet_ml_stage_draft("hello", b, sizeof b) == 0,
      "stage: unreachable model yields no draft (never invents one)");
    unsetenv("CNET_STAGE_HTTP");
    unsetenv("CNET_STAGE_RESIDUAL");

    setenv("CNET_EPISODIC_PATH", "/tmp/cnet_ml_selftest/ep.jsonl", 1);
    T(cnet_ml_episodic_path(b, sizeof b, NULL) == 0 &&
          !strcmp(b, "/tmp/cnet_ml_selftest/ep.jsonl") && ml_is_dir("/tmp/cnet_ml_selftest"),
      "episodic: explicit path, parent dir created");
    unsetenv("CNET_EPISODIC_PATH");
    setenv("CNET_MINIMAL_ROOT", "/tmp/cnet_ml_selftest/min", 1);
    T(cnet_ml_episodic_path(b, sizeof b, NULL) == 0 &&
          !strcmp(b, "/tmp/cnet_ml_selftest/min/var/marble_episodic.jsonl"),
      "episodic: default under CNET_MINIMAL_ROOT/var");
    unsetenv("CNET_MINIMAL_ROOT");

    {
        const char *ep = "/tmp/cnet_ml_selftest/ep_notes.jsonl";
        FILE *ef;
        char ctx[512];
        mkdir("/tmp/cnet_ml_selftest", 0755);
        ef = fopen(ep, "w");
        if (ef) {
            fputs("{\"ts\":1,\"peer\":\"u\",\"note\":\"deploy branch is master\"}\n", ef);
            fclose(ef);
        }
        setenv("CNET_EPISODIC_PATH", ep, 1);
        T(cnet_ml_context_pack("xyzzy", ctx, sizeof ctx) > 0 &&
              strstr(ctx, "episodic") != NULL && strstr(ctx, "master") != NULL,
          "recall-before-talk packs last episodic note");
        unsetenv("CNET_EPISODIC_PATH");
    }

    printf("\nchecks=%d failures=%d\n", n, fail);
    if (fail) { printf("MARBLE_LIVE_WIRING_FAIL\n"); return 1; }
    printf("MARBLE_LIVE_WIRING_PASS\n");
    return 0;
#undef T
}
