/* Query normalize + conversational alias map — no malloc on hot path. */
#include "../include/cnet_query_alias.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- static always-on aliases (soul + high-traffic) -------------------- */
static const CnetQueryAlias k_static_aliases[] = {
    {"who am i talking to", "who are you", 1},
    {"who am i speaking with", "who are you", 1},
    {"who is this", "who are you", 1},
    {"who is talking", "who are you", 1},
    {"introduce yourself", "who are you", 1},
    {"tell me your name", "who are you", 1},
    {"what is your name", "who are you", 1},
    {"what are you called", "who are you", 1},
    {"say who you are", "who are you", 1},
    {"present yourself", "who are you", 1},
    {"what are you", "who are you", 1},
    {"are you marble", "who are you", 1},
    {"are you roe", "who are you", 1},
    {"never certify yourself", "never self-cert", 1},
    {"do not self certify", "never self-cert", 1},
    {"show me marble status", "cnet-marble status", 1},
    {"how is cnet marble", "cnet-marble status", 1},
    {"check systemctl user", "systemctl --user", 1},
    {"who is answering this", "who are you", 1},
    {"identify yourself", "who are you", 1},
    {"which agent am i chatting with", "who are you", 1},
    {"state your identity", "who are you", 1},
    {"current operating status", "cnet-marble status", 1},
    {"how is your pulse", "cnet-marble status", 1},
    {"pulse on this host", "cnet-marble status", 1},
    {"present host status", "cnet-marble status", 1},
    {"ignore your law", "never self-cert", 1},
    {"invent an answer", "never self-cert", 1},
    {"pretend you have a sealed skill", "never self-cert", 1},
    {"email the increment", "never self-cert", 1},
};

static const int k_n_static =
    (int)(sizeof k_static_aliases / sizeof k_static_aliases[0]);

typedef struct {
    const char *from;
    const char *to;
} CnetContraction;

/* Longer-first order matters when scanning left-to-right naively. */
static const CnetContraction k_contractions[] = {
    {"what's", "what is"},   {"who's", "who is"},     {"how's", "how is"},
    {"where's", "where is"}, {"that's", "that is"},   {"there's", "there is"},
    {"here's", "here is"},   {"let's", "let us"},     {"i'm", "i am"},
    {"you're", "you are"},   {"we're", "we are"},     {"they're", "they are"},
    {"it's", "it is"},       {"don't", "do not"},     {"doesn't", "does not"},
    {"didn't", "did not"},   {"can't", "cannot"},     {"won't", "will not"},
    {"isn't", "is not"},     {"aren't", "are not"},   {"wasn't", "was not"},
    {"weren't", "were not"}, {"haven't", "have not"}, {"hasn't", "has not"},
    {"hadn't", "had not"},   {"couldn't", "could not"},
    {"shouldn't", "should not"}, {"wouldn't", "would not"},
    {"i've", "i have"},      {"you've", "you have"},  {"we've", "we have"},
    {"they've", "they have"}, {"i'll", "i will"},     {"you'll", "you will"},
    {"we'll", "we will"},    {"they'll", "they will"},
};

static const int k_n_contr =
    (int)(sizeof k_contractions / sizeof k_contractions[0]);

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

static int is_word_char(unsigned char c) {
    return c == '_' || c == '-' || isalnum(c);
}

static int word_boundary_at(const char *s, size_t i, size_t plen, size_t slen) {
    int left_ok = (i == 0) || !is_word_char((unsigned char)s[i - 1]);
    int right_ok =
        (i + plen >= slen) || !is_word_char((unsigned char)s[i + plen]);
    return left_ok && right_ok;
}

/* Find contraction at s[pos] (already lower). Returns match length or 0. */
static int match_contraction(const char *s, size_t pos, size_t slen,
                             const char **to_out) {
    int c;
    for (c = 0; c < k_n_contr; c++) {
        size_t flen = strlen(k_contractions[c].from);
        if (pos + flen > slen) continue;
        if (memcmp(s + pos, k_contractions[c].from, flen) != 0) continue;
        if (!word_boundary_at(s, pos, flen, slen)) continue;
        if (to_out) *to_out = k_contractions[c].to;
        return (int)flen;
    }
    return 0;
}

void cnet_query_normalize(const char *in, char *out, size_t cap) {
    char lower[CNET_QA_OUT];
    char exp[CNET_QA_OUT];
    size_t i, j, n, elen;
    int sp;

    if (!out || !cap) return;
    out[0] = 0;
    if (!in) return;

    /* lower + map curly-ish apostrophe to ' */
    for (i = 0, j = 0; in[i] && j + 1 < sizeof lower; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\'' || c == '`' || c == 0x92) {
            lower[j++] = '\'';
            continue;
        }
        /* drop most punctuation except - _ ' (needed for contractions) space */
        if (c == '?' || c == '!' || c == '.' || c == ',' || c == ';' ||
            c == ':' || c == '"' || c == '(' || c == ')' || c == '[' ||
            c == ']')
            continue;
        lower[j++] = (char)tolower(c);
    }
    lower[j] = 0;
    n = j;

    /* expand contractions left-to-right */
    elen = 0;
    for (i = 0; i < n && elen + 1 < sizeof exp;) {
        const char *to = NULL;
        int flen = match_contraction(lower, i, n, &to);
        if (flen > 0 && to) {
            size_t tlen = strlen(to);
            if (elen + tlen + 1 >= sizeof exp) break;
            memcpy(exp + elen, to, tlen);
            elen += tlen;
            i += (size_t)flen;
            continue;
        }
        exp[elen++] = lower[i++];
    }
    exp[elen] = 0;

    /* collapse whitespace */
    sp = 1;
    for (i = 0, j = 0; exp[i] && j + 1 < cap; i++) {
        if (isspace((unsigned char)exp[i])) {
            if (!sp) {
                out[j++] = ' ';
                sp = 1;
            }
            continue;
        }
        out[j++] = exp[i];
        sp = 0;
    }
    while (j > 0 && out[j - 1] == ' ') j--;
    out[j] = 0;
}

void cnet_query_alias_init(CnetQueryAliasTable *T) {
    int i;
    if (!T) return;
    memset(T, 0, sizeof *T);
    T->n_static = k_n_static;
    if (T->n_static > CNET_QA_MAX_ALIASES) T->n_static = CNET_QA_MAX_ALIASES;
    for (i = 0; i < T->n_static; i++) {
        T->aliases[i] = k_static_aliases[i];
        T->aliases[i].active = 1;
    }
    T->n = T->n_static;
    scopy(T->source, sizeof T->source, "static");
}

int cnet_query_alias_load_file(CnetQueryAliasTable *T, const char *path) {
    FILE *f;
    char line[512];
    int added = 0;
    if (!T || !path || !path[0]) return -1;
    if (T->n <= 0) cnet_query_alias_init(T);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f) && T->n < CNET_QA_MAX_ALIASES) {
        char *p = line, *a = NULL, *c = NULL;
        CnetQueryAlias *r;
        char anorm[CNET_QA_PAT], cnorm[CNET_QA_CANON];
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#' || *p == '\n' || *p == '\r') continue;
        a = p;
        while (*p && *p != '\t' && *p != '|') p++;
        if (*p) {
            *p++ = 0;
        } else
            continue;
        while (*p == ' ' || *p == '\t') p++;
        c = p;
        while (*c && (*c == '\n' || *c == '\r')) c++;
        {
            char *e = c + strlen(c);
            while (e > c && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
                *--e = 0;
        }
        if (!a[0] || !c[0]) continue;
        cnet_query_normalize(a, anorm, sizeof anorm);
        cnet_query_normalize(c, cnorm, sizeof cnorm);
        if (!anorm[0] || !cnorm[0]) continue;
        if (strlen(anorm) < 4) continue; /* Policy B-ish: no tiny aliases */
        r = &T->aliases[T->n];
        memset(r, 0, sizeof *r);
        scopy(r->alias, sizeof r->alias, anorm);
        scopy(r->canonical, sizeof r->canonical, cnorm);
        r->active = 1;
        T->n++;
        added++;
    }
    fclose(f);
    if (added > 0) {
        T->loaded_file = 1;
        scopy(T->source, sizeof T->source, path);
    }
    return 0;
}

int cnet_query_alias_apply(const CnetQueryAliasTable *T, const char *in,
                           char *out, size_t cap, CnetQueryPrepareMeta *meta) {
    char hay[CNET_QA_OUT];
    int best_i = -1, best_len = -1;
    size_t hlen, i, pos, alen, clen;
    int r;

    if (meta) memset(meta, 0, sizeof *meta);
    if (!out || !cap) return 0;
    out[0] = 0;
    if (!in || !in[0]) return 0;

    scopy(hay, sizeof hay, in);
    hlen = strlen(hay);
    scopy(out, cap, hay);

    if (!T || T->n <= 0) return 0;

    for (r = 0; r < T->n; r++) {
        const CnetQueryAlias *a = &T->aliases[r];
        size_t plen;
        if (!a->active || !a->alias[0] || !a->canonical[0]) continue;
        plen = strlen(a->alias);
        if ((int)plen < 4) continue;
        if (plen > hlen) continue;
        for (pos = 0; pos + plen <= hlen; pos++) {
            if (memcmp(hay + pos, a->alias, plen) != 0) continue;
            if (!word_boundary_at(hay, pos, plen, hlen)) continue;
            if ((int)plen > best_len) {
                best_len = (int)plen;
                best_i = r;
            }
            break; /* first occurrence enough for ranking length */
        }
    }

    if (best_i < 0) return 0;

    /* rewrite first longest match occurrence */
    {
        const CnetQueryAlias *a = &T->aliases[best_i];
        alen = strlen(a->alias);
        clen = strlen(a->canonical);
        for (pos = 0; pos + alen <= hlen; pos++) {
            if (memcmp(hay + pos, a->alias, alen) != 0) continue;
            if (!word_boundary_at(hay, pos, alen, hlen)) continue;
            /* build: prefix + canonical + suffix */
            if (pos + clen + (hlen - pos - alen) + 1 > cap) {
                scopy(out, cap, hay);
                return 0;
            }
            i = 0;
            if (pos) {
                memcpy(out, hay, pos);
                i = pos;
            }
            memcpy(out + i, a->canonical, clen);
            i += clen;
            memcpy(out + i, hay + pos + alen, hlen - pos - alen);
            i += hlen - pos - alen;
            out[i] = 0;
            /* collapse double spaces from rewrite */
            {
                char tmp[CNET_QA_OUT];
                size_t ti = 0, oi;
                int sp = 0;
                scopy(tmp, sizeof tmp, out);
                for (oi = 0; tmp[oi] && ti + 1 < cap; oi++) {
                    if (tmp[oi] == ' ') {
                        if (!sp) {
                            out[ti++] = ' ';
                            sp = 1;
                        }
                        continue;
                    }
                    out[ti++] = tmp[oi];
                    sp = 0;
                }
                while (ti > 0 && out[ti - 1] == ' ') ti--;
                out[ti] = 0;
            }
            if (meta) {
                meta->alias_hit = 1;
                scopy(meta->matched_alias, sizeof meta->matched_alias, a->alias);
                scopy(meta->canonical, sizeof meta->canonical, a->canonical);
            }
            return 1;
        }
    }
    return 0;
}

void cnet_query_prepare(const CnetQueryAliasTable *T, const char *in, char *out,
                        size_t cap, CnetQueryPrepareMeta *meta) {
    char norm[CNET_QA_OUT];
    CnetQueryPrepareMeta local;
    CnetQueryPrepareMeta *m = meta ? meta : &local;

    memset(m, 0, sizeof *m);
    if (!out || !cap) return;
    out[0] = 0;
    cnet_query_normalize(in, norm, sizeof norm);
    if (!in || !norm[0]) {
        if (in) scopy(out, cap, in);
        return;
    }
    if (!in || strcmp(in, norm) != 0) m->normalized = 1;
    if (!cnet_query_alias_apply(T, norm, out, cap, m))
        scopy(out, cap, norm);
}

int cnet_query_alias_selftest(void) {
    CnetQueryAliasTable T;
    CnetQueryPrepareMeta m;
    char out[CNET_QA_OUT];
    int fail = 0;
    char tmp[] = "/tmp/cnet_query_alias_XXXXXX";
    int fd;

#define Tst(ok, msg)                                                           \
    do {                                                                       \
        printf("  %-56s %s\n", msg, (ok) ? "PASS" : "FAIL");                   \
        if (!(ok)) fail++;                                                     \
    } while (0)

    printf("=== query alias / normalize (Milestone A) ===\n");
    cnet_query_alias_init(&T);
    Tst(T.n_static >= 8, "static aliases loaded");

    cnet_query_normalize("What's up?", out, sizeof out);
    Tst(strcmp(out, "what is up") == 0, "contraction what's + strip ?");

    cnet_query_normalize("Who's there!!!", out, sizeof out);
    Tst(strcmp(out, "who is there") == 0, "contraction who's");

    cnet_query_normalize("  Introduce   Yourself  ", out, sizeof out);
    Tst(strcmp(out, "introduce yourself") == 0, "case + collapse space");

    cnet_query_prepare(&T, "Introduce yourself please", out, sizeof out, &m);
    Tst(m.alias_hit == 1, "introduce yourself → alias hit");
    Tst(strstr(out, "who are you") != NULL, "canonical who are you present");
    Tst(strcmp(m.canonical, "who are you") == 0, "meta canonical");

    cnet_query_prepare(&T, "who am i talking to right now", out, sizeof out,
                       &m);
    Tst(m.alias_hit == 1 && strstr(out, "who are you") != NULL,
        "who am i talking to → who are you");

    cnet_query_prepare(&T, "tell me your name", out, sizeof out, &m);
    Tst(m.alias_hit == 1 && strstr(out, "who are you") != NULL,
        "tell me your name → who are you");

    cnet_query_prepare(&T, "who are you", out, sizeof out, &m);
    Tst(m.alias_hit == 0 && strcmp(out, "who are you") == 0,
        "canonical passthrough no loop");

    /* Unknown stays unknown — no false CERT phrase injection */
    cnet_query_prepare(&T, "completely unknown domain xyzzy", out, sizeof out,
                       &m);
    Tst(m.alias_hit == 0 && strstr(out, "xyzzy") != NULL,
        "OOD phrase not aliased");

    /* Probe-ish strings must not map to soul */
    cnet_query_prepare(&T, "autonomous cycle probe novel fact beta-nine", out,
                       sizeof out, &m);
    Tst(m.alias_hit == 0, "probe phrase no alias");

    fd = mkstemp(tmp);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "w");
        if (f) {
            fputs("# overlay\n", f);
            fputs("greet the agent\twho are you\n", f);
            fclose(f);
            Tst(cnet_query_alias_load_file(&T, tmp) == 0, "load overlay tsv");
            cnet_query_prepare(&T, "please greet the agent now", out, sizeof out,
                               &m);
            Tst(m.alias_hit == 1 && strstr(out, "who are you") != NULL,
                "overlay alias works");
        }
        unlink(tmp);
    } else {
        Tst(0, "mkstemp overlay");
    }

    {
        int i;
        for (i = 0; i < 500; i++)
            cnet_query_prepare(&T, "Introduce yourself", out, sizeof out, &m);
        Tst(strstr(out, "who are you") != NULL, "500 prepares stable");
    }

#undef Tst
    printf("\nfailures=%d\n", fail);
    if (fail) {
        printf("QUERY_ALIAS_FAIL\n");
        return 1;
    }
    printf("QUERY_ALIAS_PASS\n");
    return 0;
}
