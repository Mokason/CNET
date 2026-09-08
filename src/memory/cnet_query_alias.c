/* Query normalize + conversational alias map â€” no malloc on hot path. */
#include "../../include/cnet_query_alias.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- static always-on aliases (soul + high-traffic) -------------------- */
static const CnetQueryAlias k_static_aliases[] = {
    /* Greets are presence (cnet_sr_greet_*), never identity. Do not map
     * hi/hey/hello onto who are you — Discord Hey was dumping soul_who. */
    {"very good", "still here", 1},
    {"thanks", "still here", 1},
    {"thank you", "still here", 1},
    {"okay", "still here", 1},
    {"ok", "still here", 1},
    {"lol", "still here", 1},
    {"cool", "still here", 1},
    {"nice", "still here", 1},
    {"got it", "still here", 1},
    {"yeah i know", "still here", 1},
    {"i know", "still here", 1},
    {"yep", "still here", 1},
    {"yup", "still here", 1},
    {"yeah", "still here", 1},
    {"who made you", "who is your operator", 1},
    {"who created you", "who is your operator", 1},
    {"who is mokason", "who is your operator", 1},
    {"who is your operator", "who is your operator", 1},
    {"what is my name", "what do you call me", 1},
    {"what do you call me", "what do you call me", 1},
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
    {"how exactly are you interactiong with roe-asi", "how do you answer without teacher", 1},
    {"how exactly are you interacting with roe-asi", "how do you answer without teacher", 1},
    {"how are you interacting with roe", "how do you answer without teacher", 1},
    {"days of past week", "days of the week", 1},
    {"days of the past week", "days of the week", 1},
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

/* Greets and short acks are whole-utterance only so "hello world" / "that's
 * very good work" never steal soul. */
static int qa_is_whole_utterance(const char *a) {
    if (!a) return 0;
    return strcmp(a, "hi") == 0 || strcmp(a, "hey") == 0 ||
           strcmp(a, "hello") == 0 || strcmp(a, "yo") == 0 ||
           strcmp(a, "sup") == 0 || strcmp(a, "ok") == 0 ||
           strcmp(a, "okay") == 0 || strcmp(a, "lol") == 0 ||
           strcmp(a, "nice") == 0 || strcmp(a, "cool") == 0 ||
           strcmp(a, "thanks") == 0 || strcmp(a, "thank you") == 0 ||
           strcmp(a, "very good") == 0 || strcmp(a, "got it") == 0 ||
           strcmp(a, "yeah i know") == 0 || strcmp(a, "i know") == 0 ||
           strcmp(a, "yep") == 0 || strcmp(a, "yup") == 0 ||
           strcmp(a, "yeah") == 0;
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
    /* Every contraction below expands by less than 2x. Preserve its entire
     * suffix before collapsing whitespace; clipping here can invent identity. */
    char exp[CNET_QA_OUT * 2];
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
        /* Identity/presence describes the whole request, not a substring. */
        if ((!strcmp(a->canonical, "who are you") ||
             !strcmp(a->canonical, "who is your operator") ||
             !strcmp(a->canonical, "still here")) &&
            !cnet_query_phrase_is_whole(in, a->alias)) continue;
        /* Policy: TSV tiny aliases are rejected at load. Short acks
           (ok/yeah/thanks) are whole-query only so in-phrase never steals. */
        if (qa_is_whole_utterance(a->alias) && plen != hlen) continue;
        if ((int)plen < 4 && plen != hlen) continue;
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
    if (strlen(in) >= sizeof norm) {
        scopy(out, cap, norm);
        return; /* Do not turn a truncated prefix into an identity alias. */
    }
    if (!cnet_query_alias_apply(T, norm, out, cap, m))
        scopy(out, cap, norm);
    if (cnet_query_identity_bot(norm)) {
        scopy(out, cap, "who are you");
        m->alias_hit = 1;
        scopy(m->matched_alias, sizeof m->matched_alias, "you+llm");
        scopy(m->canonical, sizeof m->canonical, "who are you");
    }
}

int cnet_query_phrase_is_whole(const char *query, const char *phrase) {
    static const char *const prefixes[] = {
        "please ", "may i ask ", "could you ", "can you ", "so "
    };
    static const char *const suffixes[] = {" please", " right now", " now"};
    char norm[CNET_QA_OUT * 2];
    char *start;
    size_t i, n, p;
    unsigned pass;
    if (!query || !phrase || !phrase[0] || strlen(query) >= CNET_QA_OUT) return 0;
    cnet_query_normalize(query, norm, sizeof norm);
    start = norm;
    for (pass = 0; pass < 3; pass++) {
        if (!strcmp(start, phrase)) return 1;
        if (pass == 2) break;
        for (i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++) {
            p = strlen(prefixes[i]);
            if (!strncmp(start, prefixes[i], p)) { start += p; break; }
        }
        n = strlen(start);
        for (i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
            p = strlen(suffixes[i]);
            if (n > p && !strcmp(start + n - p, suffixes[i])) {
                start[n - p] = 0;
                break;
            }
        }
    }
    return 0;
}

int cnet_query_identity_bot(const char *normalized) {
    static const char *const questions[] = {
        "are you an llm", "are you a llm", "are you llm", "are you chatgpt",
        "are you a chatbot", "are you a language model", "are you an ai",
        "are you a large language model", "you are an llm", "you are chatgpt",
        "you are a chatbot", "you are a language model",
        "you are like brick by brick made llm"
    };
    size_t i;
    for (i = 0; i < sizeof questions / sizeof questions[0]; i++)
        if (cnet_query_phrase_is_whole(normalized, questions[i])) return 1;
    return 0;
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
    Tst(m.alias_hit == 1, "introduce yourself â†’ alias hit");
    Tst(strstr(out, "who are you") != NULL, "canonical who are you present");
    Tst(strcmp(m.canonical, "who are you") == 0, "meta canonical");

    cnet_query_prepare(&T, "who am i talking to right now", out, sizeof out,
                       &m);
    Tst(m.alias_hit == 1 && strstr(out, "who are you") != NULL,
        "who am i talking to â†’ who are you");

    cnet_query_prepare(&T, "tell me your name", out, sizeof out, &m);
    Tst(m.alias_hit == 1 && strstr(out, "who are you") != NULL,
        "tell me your name â†’ who are you");

    cnet_query_prepare(&T, "who are you", out, sizeof out, &m);
    Tst(m.alias_hit == 0 && strcmp(out, "who are you") == 0,
        "canonical passthrough no loop");

    /* Discord leftover: "are you an LLM / brick by brick" is identity, not FAQ. */
    cnet_query_prepare(&T, "so you're like brick by brick made, LLM?", out,
                       sizeof out, &m);
    Tst(m.alias_hit == 1 && strcmp(out, "who are you") == 0,
        "you+llm discord phrase → who are you");
    Tst(cnet_query_identity_bot("so you are like brick by brick made llm") == 1,
        "identity_bot you+llm");
    cnet_query_prepare(&T, "what is an llm", out, sizeof out, &m);
    Tst(m.alias_hit == 0 && strstr(out, "llm") != NULL,
        "what is an llm is not identity");
    Tst(cnet_query_identity_bot("what is an llm") == 0,
        "identity_bot requires you");

    /* Discord: "hello" inside hello-world is not identity. Bare hello is
     * presence, not soul_who. */
    cnet_query_prepare(&T, "can you write hello world in C#?", out, sizeof out,
                       &m);
    Tst(m.alias_hit == 0 && strstr(out, "hello") != NULL,
        "hello world is not identity");
    Tst(strstr(out, "who are you") == NULL,
        "hello world does not inject who are you");
    cnet_query_prepare(&T, "hello", out, sizeof out, &m);
    Tst(m.alias_hit == 0 && strcmp(out, "hello") == 0,
        "bare hello is not who are you");
    cnet_query_prepare(&T, "hey", out, sizeof out, &m);
    Tst(m.alias_hit == 0 && strcmp(out, "hey") == 0,
        "bare hey is not who are you");
    cnet_query_prepare(&T, "yeah i know", out, sizeof out, &m);
    Tst(m.alias_hit == 1 && strcmp(out, "still here") == 0,
        "bare yeah i know → still here");
    cnet_query_prepare(&T, "yeah i know json", out, sizeof out, &m);
    Tst(m.alias_hit == 0 && strstr(out, "json") != NULL,
        "in-phrase yeah i know is not ack");

    /* Personality: operator/address/ack. Whole-utterance acks only. */
    cnet_query_prepare(&T, "who made you", out, sizeof out, &m);
    Tst(m.alias_hit == 1 && strcmp(out, "who is your operator") == 0,
        "who made you → operator");
    cnet_query_prepare(&T, "what is my name", out, sizeof out, &m);
    Tst(m.alias_hit == 1 && strcmp(out, "what do you call me") == 0,
        "what is my name → address");
    cnet_query_prepare(&T, "very good", out, sizeof out, &m);
    Tst(m.alias_hit == 1 && strcmp(out, "still here") == 0,
        "bare very good → still here");
    cnet_query_prepare(&T, "that's very good work", out, sizeof out, &m);
    Tst(m.alias_hit == 0 && strstr(out, "very good") != NULL,
        "in-phrase very good is not ack");

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
