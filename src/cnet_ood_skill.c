#include "cnet_ood_skill.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if CNET_HAVE_CURL
#include "cnet_lookup.h"
#endif

static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n;
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) n = cap - 1u;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int has_word_ci(const char *text, const char *word) {
    const char *cursor;
    size_t n, i;
    if (text == NULL || word == NULL || word[0] == '\0') return 0;
    n = strlen(word);
    for (cursor = text; *cursor != '\0'; ++cursor) {
        int left, right, match = 1;
        if (cursor > text && isalnum((unsigned char)cursor[-1])) continue;
        for (i = 0; i < n; ++i) {
            if (cursor[i] == '\0' ||
                tolower((unsigned char)cursor[i]) !=
                    tolower((unsigned char)word[i])) {
                match = 0;
                break;
            }
        }
        if (!match) continue;
        right = !isalnum((unsigned char)cursor[n]);
        left = (cursor == text) || !isalnum((unsigned char)cursor[-1]);
        if (left && right) return 1;
    }
    return 0;
}

static int is_stop(const char *word) {
    static const char *const stops[] = {
        "what", "who", "when", "where", "why", "how", "is", "was", "were",
        "are",  "am",  "do",   "does",  "did", "the", "a",  "an",  "to",
        "of",   "for", "in",   "on",    "at",  "by",  "from", "with", "about",
        "please", "tell", "me", "you", "year", "created", "founded",
        "incorporated", "established", "lookup", "token", "line", "integer",
        "plus", "minus", "times", "add", "sum", NULL
    };
    size_t i;
    for (i = 0; stops[i] != NULL; ++i)
        if (strcmp(word, stops[i]) == 0) return 1;
    return 0;
}

static int all_digits(const char *s) {
    if (s == NULL || s[0] == '\0') return 0;
    for (; *s != '\0'; ++s)
        if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

int cnet_ood_subject(const char *turn, char *title, size_t cap) {
    char lower[CNET_OOD_TITLE];
    size_t o = 0, i, n;
    int any = 0;
    const char *p;
    if (title == NULL || cap == 0) return -1;
    title[0] = '\0';
    if (turn == NULL || turn[0] == '\0') return 1;
    for (p = turn; *p != '\0';) {
        if (!isalnum((unsigned char)*p)) {
            ++p;
            continue;
        }
        n = 0;
        while (p[n] != '\0' && isalnum((unsigned char)p[n]) &&
               n + 1u < sizeof lower)
            ++n;
        if (n == 0 || (p[n] != '\0' && isalnum((unsigned char)p[n]))) {
            while (*p != '\0' && isalnum((unsigned char)*p)) ++p;
            continue;
        }
        for (i = 0; i < n; ++i) lower[i] = (char)tolower((unsigned char)p[i]);
        lower[n] = '\0';
        p += n;
        if (is_stop(lower) || all_digits(lower)) continue;
        if (any) {
            if (o + 1u >= cap) return 1;
            title[o++] = '_';
        }
        if (o + n >= cap) return 1;
        title[o++] = (char)toupper((unsigned char)lower[0]);
        for (i = 1; i < n; ++i) title[o++] = lower[i];
        any = 1;
    }
    title[o] = '\0';
    return any ? 0 : 1;
}

int cnet_ood_parse_arith(const char *turn, unsigned long *a, unsigned long *b,
                         int *op) {
    const char *p;
    unsigned long nums[2];
    int n = 0, found = 0;
    if (turn == NULL || a == NULL || b == NULL || op == NULL) return -1;
    for (p = turn; *p != '\0';) {
        if (isdigit((unsigned char)*p)) {
            char *end = NULL;
            unsigned long v;
            if (p > turn && isalnum((unsigned char)p[-1])) {
                ++p;
                continue;
            }
            v = strtoul(p, &end, 10);
            if (end == p) {
                ++p;
                continue;
            }
            if (n >= 2) return 1;
            nums[n++] = v;
            p = end;
            continue;
        }
        if (n == 1 && found == 0) {
            if (*p == '+') {
                found = CNET_OOD_OP_ADD;
                ++p;
                continue;
            }
            if (*p == '*') {
                found = CNET_OOD_OP_MUL;
                ++p;
                continue;
            }
            if (*p == '-' &&
                (p[1] == '\0' || p[1] == ' ' || isdigit((unsigned char)p[1]))) {
                found = CNET_OOD_OP_SUB;
                ++p;
                continue;
            }
        }
        ++p;
    }
    if (n != 2) return 1;
    if (found == 0) {
        if (has_word_ci(turn, "plus") || has_word_ci(turn, "add") ||
            has_word_ci(turn, "sum"))
            found = CNET_OOD_OP_ADD;
        else if (has_word_ci(turn, "minus"))
            found = CNET_OOD_OP_SUB;
        else if (has_word_ci(turn, "times") || has_word_ci(turn, "multiplied"))
            found = CNET_OOD_OP_MUL;
        else
            return 1;
    }
    *a = nums[0];
    *b = nums[1];
    *op = found;
    return 0;
}

int cnet_ood_json_extract(const char *body, char *out, size_t cap) {
    const char *key = "\"extract\":\"";
    const char *src;
    size_t o = 0;
    if (out == NULL || cap == 0) return -1;
    out[0] = '\0';
    if (body == NULL) return 1;
    if (strstr(body, "\"type\":\"disambiguation\"") != NULL) return 1;
    src = strstr(body, key);
    if (src == NULL) return 1;
    src += 11;
    while (*src != '\0' && *src != '"' && o + 1u < cap) {
        if (*src == '\\' && src[1] != '\0') {
            ++src;
            if (*src == 'n' || *src == 'r' || *src == 't')
                out[o++] = ' ';
            else
                out[o++] = *src;
            ++src;
            continue;
        }
        out[o++] = *src++;
    }
    out[o] = '\0';
    return o > 0u ? 0 : 1;
}

static int commit_compute(CnetSkillLaneResult *out, const char *skill,
                          unsigned long value) {
    if (out == NULL || skill == NULL) return -1;
    memset(out, 0, sizeof *out);
    copy_text(out->skill, sizeof out->skill, skill);
    snprintf(out->value, sizeof out->value, "%lu", value);
    copy_text(out->spoken, sizeof out->spoken, out->value);
    out->bound = 1;
    out->kind = CNET_SKILL_LANE_EXACT;
    out->claimed_cert = 1;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    return 0;
}

int cnet_ood_try_add(const char *turn, CnetSkillLaneResult *out) {
    unsigned long a, b, result;
    int op;
    const char *skill;
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (cnet_ood_parse_arith(turn, &a, &b, &op) != 0) return 1;
    if (a > 4294967295UL || b > 4294967295UL) return 1;
    if (op == CNET_OOD_OP_ADD) {
        if (a > 4294967295UL - b) return 1;
        result = a + b;
        skill = CNET_OOD_ADD;
    } else if (op == CNET_OOD_OP_SUB) {
        if (b > a) return 1;
        result = a - b;
        skill = CNET_OOD_SUB;
    } else if (op == CNET_OOD_OP_MUL) {
        if (a != 0ul && b > 4294967295UL / a) return 1;
        result = a * b;
        skill = CNET_OOD_MUL;
    } else {
        return 1;
    }
    return commit_compute(out, skill, result);
}

int cnet_ood_try_wiki_url(const char *url, unsigned flags, int year_cue,
                          const char *subject, CnetSkillLaneResult *out) {
#if !CNET_HAVE_CURL
    (void)url;
    (void)flags;
    (void)year_cue;
    (void)subject;
    if (out != NULL) memset(out, 0, sizeof *out);
    return 1;
#else
    CnetLookupReport report;
    CnetLookupBind bind;
    char token[CNET_OOD_TITLE];
    const char *cut;
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (url == NULL || url[0] == '\0') return 1;
    bind = year_cue ? CNET_LOOKUP_BIND_YEAR : CNET_LOOKUP_BIND_EXTRACT;
    if (cnet_lookup_execute_flags(url, bind, flags, &report) != 0 ||
        !report.bound || report.value[0] == '\0')
        return 1;
    token[0] = '\0';
    if (subject != NULL && subject[0] != '\0') {
        copy_text(token, sizeof token, subject);
        cut = strchr(token, '_');
        if (cut != NULL) token[cut - token] = '\0';
    }
    if (!year_cue && token[0] != '\0' &&
        !has_word_ci(report.value, token))
        return 1;
    copy_text(out->skill, sizeof out->skill, CNET_OOD_WIKI);
    copy_text(out->value, sizeof out->value, report.value);
    if (year_cue)
        copy_text(out->spoken, sizeof out->spoken, report.value);
    else
        copy_text(out->spoken, sizeof out->spoken, report.value);
    out->bound = 1;
    out->kind = CNET_SKILL_LANE_EXACT;
    out->claimed_cert = 1;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    return 0;
#endif
}

static int year_cue(const char *turn) {
    return has_word_ci(turn, "year") || has_word_ci(turn, "created") ||
           has_word_ci(turn, "founded") || has_word_ci(turn, "incorporated") ||
           has_word_ci(turn, "when");
}

static void wiki_url(char *url, size_t cap, const char *title, int want_year) {
    if (want_year)
        snprintf(url, cap,
                 "https://en.wikipedia.org/w/api.php?action=query&prop=revisions"
                 "&rvprop=content&rvslots=main&format=json&redirects=1&titles=%s",
                 title);
    else
        snprintf(url, cap, "https://en.wikipedia.org/api/rest_v1/page/summary/%s",
                 title);
}

static int title_parts(const char *title) {
    int n = 1;
    const char *p;
    if (title == NULL || title[0] == '\0') return 0;
    for (p = title; *p != '\0'; ++p)
        if (*p == '_') n++;
    return n;
}

/* Deterministic title forms. Never first-word-only (that binds the wrong page). */
static int wiki_title_form(const char *title, int which, char *out, size_t cap) {
    char buf[CNET_OOD_TITLE];
    char *toks[12];
    int n = 0, i;
    size_t o = 0;
    if (title == NULL || out == NULL || cap == 0) return 1;
    out[0] = '\0';
    copy_text(buf, sizeof buf, title);
    toks[n++] = buf;
    for (i = 0; buf[i] != '\0' && n < 12; ++i) {
        if (buf[i] == '_') {
            buf[i] = '\0';
            if (buf[i + 1] != '\0') toks[n++] = &buf[i + 1];
        }
    }
    if (n == 0) return 1;
    if (which == 0) {
        copy_text(out, cap, title);
        return out[0] ? 0 : 1;
    }
    if (which == 1) {
        for (i = 0; i < n; ++i) {
            size_t k, len = strlen(toks[i]);
            if (i && o + 1u < cap) out[o++] = '_';
            for (k = 0; k < len && o + 1u < cap; ++k) {
                unsigned char c = (unsigned char)toks[i][k];
                out[o++] = (char)((i == 0 && k == 0) ? toupper(c) : tolower(c));
            }
        }
        out[o] = '\0';
        return o ? 0 : 1;
    }
    if (which == 2 || which == 3) {
        char first[CNET_OOD_TITLE];
        char last[CNET_OOD_TITLE];
        size_t flen, llen;
        copy_text(first, sizeof first, toks[0]);
        flen = strlen(first);
        if (flen > 3u && (first[flen - 1u] == 's' || first[flen - 1u] == 'S'))
            first[flen - 1u] = '\0';
        toks[0] = first;
        if (which == 3 && n >= 2) {
            copy_text(last, sizeof last, toks[n - 1]);
            llen = strlen(last);
            if (llen > 3u && (last[llen - 1u] == 'e' || last[llen - 1u] == 'E') &&
                o + llen + 4u < sizeof last) {
                last[llen - 1u] = '\0';
                memcpy(last + llen - 1u, "ion", 4);
                toks[n - 1] = last;
            }
        }
        for (i = 0; i < n; ++i) {
            size_t k, len = strlen(toks[i]);
            if (i && o + 1u < cap) out[o++] = '_';
            for (k = 0; k < len && o + 1u < cap; ++k) {
                unsigned char c = (unsigned char)toks[i][k];
                out[o++] = (char)((i == 0 && k == 0) ? toupper(c) : tolower(c));
            }
        }
        out[o] = '\0';
        return o ? 0 : 1;
    }
    return 1;
}

int cnet_ood_try_wiki(const char *turn, unsigned flags, CnetSkillLaneResult *out) {
    char title[CNET_OOD_TITLE];
    char form[CNET_OOD_TITLE];
    char url[640];
    int want_year, which;
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (cnet_ood_subject(turn, title, sizeof title) != 0) return 1;
    want_year = year_cue(turn);
    /* One token + year is usually the wrong page (person vs company). */
    if (want_year && title_parts(title) < 2) return 1;
    for (which = 0; which < 4; ++which) {
        if (wiki_title_form(title, which, form, sizeof form) != 0) continue;
        if (which > 0 && strcmp(form, title) == 0) continue;
        wiki_url(url, sizeof url, form, want_year);
        if (cnet_ood_try_wiki_url(url, flags, want_year, form, out) == 0)
            return 0;
    }
    return 1;
}

int cnet_ood_try_held(const char *turn, CnetSkillLaneResult *out) {
    char spoken[CNET_SKILL_LANE_TEXT];
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (cnet_held_model_ask(turn, spoken, sizeof spoken) != 0 ||
        spoken[0] == '\0')
        return 1;
    copy_text(out->skill, sizeof out->skill, CNET_HELD_CONTRACT);
    copy_text(out->spoken, sizeof out->spoken, spoken);
    copy_text(out->value, sizeof out->value, spoken);
    out->bound = 1;
    out->kind = CNET_SKILL_LANE_HELD;
    out->claimed_cert = 0;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    return 0;
}

int cnet_ood_handle(const char *turn, CnetSkillLaneResult *out) {
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (cnet_ood_try_add(turn, out) == 0) return 0;
#if CNET_HAVE_CURL
    if (cnet_ood_try_wiki(turn, 0, out) == 0) return 0;
#endif
    if (cnet_ood_try_held(turn, out) == 0) return 0;
    return 1;
}
