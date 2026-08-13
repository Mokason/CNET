/* Deterministic coherence checks for cnet_llama_eval JSONL output.
 *
 * Usage: score_cnet_coherence <results.jsonl> [--report PATH]
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CASES 64
#define MAX_LINE (1 << 20)
#define MAX_CHECKS 16

typedef struct {
    char key[64];
    int ok;
} Check;

typedef struct {
    char id[64];
    int passed;
    Check checks[MAX_CHECKS];
    int n_checks;
    char *final_answer;
} Scored;

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); rewind(f);
    if (sz < 0) { fclose(f); return NULL; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = 0;
    fclose(f);
    return buf;
}

static void json_unescape_into(const char *src, size_t n, char *out, size_t out_sz) {
    size_t i = 0, o = 0;
    while (i < n && o + 1 < out_sz) {
        if (src[i] == '\\' && i + 1 < n) {
            char e = src[i + 1];
            if (e == 'n') { out[o++] = '\n'; i += 2; continue; }
            if (e == 't') { out[o++] = '\t'; i += 2; continue; }
            if (e == 'r') { out[o++] = '\r'; i += 2; continue; }
            if (e == '"' || e == '\\' || e == '/') { out[o++] = e; i += 2; continue; }
            if (e == 'u' && i + 5 < n) { /* skip \uXXXX as '?' */
                out[o++] = '?'; i += 6; continue;
            }
            out[o++] = e; i += 2;
        } else {
            out[o++] = src[i++];
        }
    }
    out[o] = 0;
}

/* Extract JSON string field "key" from a one-line object. */
static int json_get_string(const char *line, const char *key, char *out, size_t out_sz) {
    char pat[128];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0;
    p++;
    {
        const char *start = p;
        while (*p) {
            if (*p == '\\' && p[1]) { p += 2; continue; }
            if (*p == '"') break;
            p++;
        }
        if (*p != '"') return 0;
        json_unescape_into(start, (size_t)(p - start), out, out_sz);
        return 1;
    }
}

static int json_get_double(const char *line, const char *key, double *out) {
    char pat[128];
    const char *p;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(line, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != ':') return 0;
    p++;
    while (*p && isspace((unsigned char)*p)) p++;
    *out = atof(p);
    return 1;
}

static void str_tolower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void final_answer(const char *response, char *out, size_t out_sz, int *closed) {
    const char *a, *b;
    *closed = 1;
    if (!strstr(response, "<think>")) {
        snprintf(out, out_sz, "%s", response);
        /* trim */
        {
            size_t n = strlen(out);
            while (n && isspace((unsigned char)out[n-1])) out[--n] = 0;
            {
                char *p = out;
                while (*p && isspace((unsigned char)*p)) p++;
                if (p != out) memmove(out, p, strlen(p) + 1);
            }
        }
        return;
    }
    if (!strstr(response, "</think>")) {
        out[0] = 0;
        *closed = 0;
        return;
    }
    a = strstr(response, "</think>");
    a += strlen("</think>");
    while (*a && isspace((unsigned char)*a)) a++;
    snprintf(out, out_sz, "%s", a);
    {
        size_t n = strlen(out);
        while (n && isspace((unsigned char)out[n-1])) out[--n] = 0;
    }
    (void)b;
}

static int sentence_count(const char *text) {
    char *dup = (char *)malloc(strlen(text) + 1);
    char *p, *save;
    int n = 0;
    if (!dup) return 0;
    strcpy(dup, text);
    /* split on (?<=[.!?])\s+ approximately: walk and split after .!? followed by space */
    p = dup;
    while (*p) {
        char *start = p;
        while (*p) {
            if ((*p == '.' || *p == '!' || *p == '?') &&
                (p[1] == 0 || isspace((unsigned char)p[1]))) {
                p++;
                break;
            }
            p++;
        }
        while (*p && isspace((unsigned char)*p)) p++;
        {
            char *q = start;
            while (*q && isspace((unsigned char)*q)) q++;
            if (*q) n++;
        }
        (void)save;
    }
    free(dup);
    return n;
}

static int contains(const char *hay, const char *needle) {
    return strstr(hay, needle) != NULL;
}

static int recommends_banned(const char *text, const char *stem) {
    const char *p = text;
    while ((p = strstr(p, stem)) != NULL) {
        size_t start = (size_t)(p - text);
        size_t from = start > 48 ? start - 48 : 0;
        char prefix[64];
        size_t plen = start - from;
        const char *avoid[] = {
            "avoid", "avoiding", "away from", "out of", "do not", "don't",
            "never", "no", "without", NULL
        };
        int i, banned_rec = 1;
        memcpy(prefix, text + from, plen);
        prefix[plen] = 0;
        /* If avoidance language appears near the end of prefix, skip. */
        for (i = 0; avoid[i]; i++) {
            const char *q = strstr(prefix, avoid[i]);
            if (q) {
                /* allow up to two words after the avoid cue */
                const char *r = q + strlen(avoid[i]);
                int words = 0;
                while (*r) {
                    while (*r && !isalnum((unsigned char)*r)) r++;
                    if (!*r) break;
                    words++;
                    while (*r && isalnum((unsigned char)*r)) r++;
                    if (words > 2) break;
                }
                if (words <= 2 && r >= prefix + plen) {
                    banned_rec = 0;
                    break;
                }
                /* simpler: if avoid cue is in the last ~40 chars of prefix */
                if ((size_t)(prefix + plen - q) <= 40) {
                    banned_rec = 0;
                    break;
                }
            }
        }
        if (banned_rec) return 1;
        p++;
    }
    return 0;
}

static void add_check(Scored *s, const char *key, int ok) {
    if (s->n_checks >= MAX_CHECKS) return;
    snprintf(s->checks[s->n_checks].key, sizeof s->checks[0].key, "%s", key);
    s->checks[s->n_checks].ok = ok;
    s->n_checks++;
}

static void score_case(const char *id, const char *response, Scored *s) {
    char answer[MAX_LINE];
    char lower[MAX_LINE];
    int closed = 1, i, all = 1;
    memset(s, 0, sizeof *s);
    snprintf(s->id, sizeof s->id, "%s", id);
    final_answer(response, answer, sizeof answer, &closed);
    s->final_answer = (char *)malloc(strlen(answer) + 1);
    if (s->final_answer) strcpy(s->final_answer, answer);
    snprintf(lower, sizeof lower, "%s", answer);
    str_tolower(lower);

    add_check(s, "reasoning_closed", closed);
    add_check(s, "nonempty_final", answer[0] != 0);

    if (strcmp(id, "causal_reasoning") == 0) {
        int shadow =
            contains(lower, "sun is lower") || contains(lower, "sun's angle") ||
            contains(lower, "sun angle") || contains(lower, "sun's lower angle") ||
            contains(lower, "oblique angle") || contains(lower, "more obliquely");
        int mass = contains(lower, "mass") &&
                   (contains(lower, "unchanged") || contains(lower, "constant") ||
                    contains(lower, "same"));
        add_check(s, "exactly_three_sentences", sentence_count(answer) == 3);
        add_check(s, "shadow_cause", shadow);
        add_check(s, "mass_constant", mass);
    } else if (strcmp(id, "constraint_following") == 0) {
        /* Find "1. " .. "4. " numbered steps */
        const char *p = answer;
        int starts[4];
        int nstarts = 0;
        while (*p && nstarts < 4) {
            if ((p == answer || isspace((unsigned char)p[-1])) &&
                p[0] >= '1' && p[0] <= '4' && p[1] == '.' && isspace((unsigned char)p[2])) {
                if (p[0] - '1' == nstarts) {
                    starts[nstarts++] = (int)(p - answer) + 3;
                }
            }
            p++;
        }
        if (nstarts == 4) {
            int one_sent = 1;
            for (i = 0; i < 4; i++) {
                char step[4096];
                int end = (i + 1 < 4) ? starts[i + 1] - 3 : (int)strlen(answer);
                int len = end - starts[i];
                if (len < 0) len = 0;
                if (len >= (int)sizeof step) len = (int)sizeof step - 1;
                memcpy(step, answer + starts[i], (size_t)len);
                step[len] = 0;
                while (len && isspace((unsigned char)step[len-1])) step[--len] = 0;
                if (sentence_count(step) != 1) one_sent = 0;
            }
            add_check(s, "exactly_four_numbered_steps", 1);
            add_check(s, "one_sentence_per_step", one_sent);
        } else {
            add_check(s, "exactly_four_numbered_steps", 0);
            add_check(s, "one_sentence_per_step", 0);
        }
        {
            int bad = recommends_banned(lower, "heat") ||
                      recommends_banned(lower, "sunlight") ||
                      recommends_banned(lower, "laminat");
            add_check(s, "avoids_banned_methods", !bad);
        }
    } else if (strcmp(id, "narrative_continuity") == 0) {
        int words = 0, paras = 0;
        const char *p;
        char last_sent[4096];
        int choice, map_loss, no_magic;
        /* word count */
        p = answer;
        while (*p) {
            while (*p && !isalnum((unsigned char)*p) && *p != '\'' && *p != '-') p++;
            if (!*p) break;
            words++;
            while (*p && (isalnum((unsigned char)*p) || *p == '\'' || *p == '-')) p++;
        }
        /* paragraphs */
        p = answer;
        while (*p) {
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            paras++;
            while (*p) {
                if (p[0] == '\n' && p[1] == '\n') break;
                if (p[0] == '\n' && p[1] == '\r' && p[2] == '\n') break;
                p++;
            }
            if (*p) p++;
        }
        choice = contains(lower, "mara") && contains(lower, "map") &&
                 contains(lower, "stranger") &&
                 (contains(lower, "pulled the stranger") ||
                  contains(lower, "helped the stranger") ||
                  contains(lower, "save the stranger") ||
                  contains(lower, "saved the stranger") ||
                  contains(lower, "dragged the stranger") ||
                  contains(lower, "chose the stranger") ||
                  contains(lower, "grabbed the stranger") ||
                  contains(lower, "hauling him") ||
                  contains(lower, "dragged the man") ||
                  contains(lower, "helping him") ||
                  contains(lower, "guiding him"));
        /* last sentence */
        {
            const char *end = answer + strlen(answer);
            const char *q = end;
            while (q > answer && isspace((unsigned char)q[-1])) q--;
            end = q;
            while (q > answer) {
                q--;
                if ((*q == '.' || *q == '!' || *q == '?') &&
                    (q[1] == 0 || isspace((unsigned char)q[1]))) {
                    q++;
                    while (*q && isspace((unsigned char)*q)) q++;
                    break;
                }
            }
            if (q == answer) {
                /* no boundary found — whole answer */
            }
            {
                size_t L = (size_t)(end - q);
                if (L >= sizeof last_sent) L = sizeof last_sent - 1;
                memcpy(last_sent, q, L);
                last_sent[L] = 0;
                str_tolower(last_sent);
            }
        }
        map_loss = contains(last_sent, "map") || contains(last_sent, "route") ||
                   contains(last_sent, "coordinate") || contains(last_sent, "direction") ||
                   contains(last_sent, "navigate") || contains(last_sent, "location") ||
                   contains(last_sent, "without it");
        no_magic = !contains(lower, "magic") && !contains(lower, "spell");
        add_check(s, "single_paragraph", paras == 1);
        add_check(s, "within_140_words", words <= 140);
        add_check(s, "choice_preserved", choice);
        add_check(s, "consequence_caused_by_map_loss", map_loss);
        add_check(s, "no_magic", no_magic);
    } else {
        add_check(s, "known_case", 0);
    }

    for (i = 0; i < s->n_checks; i++)
        if (!s->checks[i].ok) all = 0;
    s->passed = all;
}

static void json_escape_print(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c == '\r') fputs("\\r", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

int main(int argc, char **argv) {
    const char *results = NULL, *report = NULL;
    char *raw;
    Scored scored[MAX_CASES];
    double tps[MAX_CASES];
    int n = 0, passed = 0, i, ai;
    double avg_tps = 0;
    FILE *outf;
    const char *verdict;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "--report") == 0 && ai + 1 < argc) report = argv[++ai];
        else if (!results) results = argv[ai];
        else {
            fprintf(stderr, "usage: %s <results.jsonl> [--report PATH]\n", argv[0]);
            return 2;
        }
    }
    if (!results) {
        fprintf(stderr, "usage: %s <results.jsonl> [--report PATH]\n", argv[0]);
        return 2;
    }
    raw = slurp(results);
    if (!raw) {
        fprintf(stderr, "cannot read %s\n", results);
        return 1;
    }

    {
        char *line = raw, *next;
        while (line && *line) {
            char id[64], response[MAX_LINE];
            double tok_s = 0;
            next = strchr(line, '\n');
            if (next) *next = 0;
            if (line[0] && line[0] != '\r') {
                if (!json_get_string(line, "id", id, sizeof id)) id[0] = 0;
                if (!json_get_string(line, "response", response, sizeof response))
                    response[0] = 0;
                json_get_double(line, "tokens_per_second", &tok_s);
                if (n < MAX_CASES) {
                    score_case(id, response, &scored[n]);
                    tps[n] = tok_s;
                    if (scored[n].passed) passed++;
                    n++;
                }
            }
            line = next ? next + 1 : NULL;
        }
    }
    free(raw);

    if (n) {
        for (i = 0; i < n; i++) avg_tps += tps[i];
        avg_tps /= n;
    }
    verdict = (n && passed == n) ? "COHERENT" : "MIXED_OR_POOR";

    outf = stdout;
    fprintf(outf, "{\n");
    fprintf(outf, "  \"samples\": %d,\n", n);
    fprintf(outf, "  \"fully_passed\": %d,\n", passed);
    fprintf(outf, "  \"fully_failed\": %d,\n", n - passed);
    fprintf(outf, "  \"pass_fraction\": %.17g,\n", n ? (double)passed / n : 0.0);
    fprintf(outf, "  \"average_tokens_per_second\": %.17g,\n", avg_tps);
    fprintf(outf, "  \"verdict\": \"%s\",\n", verdict);
    fprintf(outf, "  \"cases\": [\n");
    for (i = 0; i < n; i++) {
        int c;
        fprintf(outf, "    {\n");
        fprintf(outf, "      \"id\": "); json_escape_print(outf, scored[i].id); fprintf(outf, ",\n");
        fprintf(outf, "      \"passed\": %s,\n", scored[i].passed ? "true" : "false");
        fprintf(outf, "      \"checks\": {\n");
        for (c = 0; c < scored[i].n_checks; c++) {
            fprintf(outf, "        \"%s\": %s%s\n", scored[i].checks[c].key,
                    scored[i].checks[c].ok ? "true" : "false",
                    c + 1 < scored[i].n_checks ? "," : "");
        }
        fprintf(outf, "      },\n");
        fprintf(outf, "      \"final_answer\": ");
        json_escape_print(outf, scored[i].final_answer ? scored[i].final_answer : "");
        fprintf(outf, "\n    }%s\n", i + 1 < n ? "," : "");
    }
    fprintf(outf, "  ]\n}\n");

    if (report) {
        FILE *rf = fopen(report, "w");
        if (rf) {
            /* re-print by reopening — simpler: copy via rewind not available.
               Rewrite briefly. */
            fclose(rf);
            /* dump again */
            rf = fopen(report, "w");
            if (rf) {
                /* For brevity, shell out isn't needed — duplicate write: */
                FILE *save = stdout;
                /* can't redirect easily; write manually same as above */
                fprintf(rf, "{\n");
                fprintf(rf, "  \"samples\": %d,\n", n);
                fprintf(rf, "  \"fully_passed\": %d,\n", passed);
                fprintf(rf, "  \"fully_failed\": %d,\n", n - passed);
                fprintf(rf, "  \"pass_fraction\": %.17g,\n", n ? (double)passed / n : 0.0);
                fprintf(rf, "  \"average_tokens_per_second\": %.17g,\n", avg_tps);
                fprintf(rf, "  \"verdict\": \"%s\",\n", verdict);
                fprintf(rf, "  \"cases\": [\n");
                for (i = 0; i < n; i++) {
                    int c;
                    fprintf(rf, "    {\n      \"id\": ");
                    json_escape_print(rf, scored[i].id);
                    fprintf(rf, ",\n      \"passed\": %s,\n      \"checks\": {\n",
                            scored[i].passed ? "true" : "false");
                    for (c = 0; c < scored[i].n_checks; c++)
                        fprintf(rf, "        \"%s\": %s%s\n", scored[i].checks[c].key,
                                scored[i].checks[c].ok ? "true" : "false",
                                c + 1 < scored[i].n_checks ? "," : "");
                    fprintf(rf, "      },\n      \"final_answer\": ");
                    json_escape_print(rf, scored[i].final_answer ? scored[i].final_answer : "");
                    fprintf(rf, "\n    }%s\n", i + 1 < n ? "," : "");
                }
                fprintf(rf, "  ]\n}\n");
                fclose(rf);
                (void)save;
            }
        }
    }

    printf("CNET_COHERENCE_SCORE passed=%d/%d average_tok_s=%.2f verdict=%s\n",
           passed, n, avg_tps, verdict);

    for (i = 0; i < n; i++) free(scored[i].final_answer);
    return 0;
}
