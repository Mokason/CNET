/* Typed English student — discrete slots + hash table + audit SHOW.
 *
 * Working memory is ids. SHOW renders ids to English so an operator can
 * see hostility/anomaly. Empty SHOW is fail-closed. Floats cannot persist.
 */
#include "../include/cnet_typed_en.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    char lemma[CNET_TE_LEMMA];
    char past[CNET_TE_FORM];
    char part[CNET_TE_FORM];
    uint64_t hash;
    int used;
} CnetTeVerb;

static CnetTeVerb g_verbs[CNET_TE_MAX_VERBS];
static int g_nverb;
static int g_ready;

typedef struct {
    char lemma[CNET_TE_LEMMA];
    char plural[CNET_TE_FORM];
    uint64_t hash;
    int used;
} CnetTeNoun;

typedef struct {
    char lemma[CNET_TE_LEMMA];
    char art[8];
    uint64_t hash;
    int used;
} CnetTeArtRow;

static CnetTeNoun g_nouns[CNET_TE_MAX_VERBS];
static int g_nnoun;
static CnetTeArtRow g_arts[CNET_TE_MAX_VERBS];
static int g_nart;
static CnetTeNoun g_ings[CNET_TE_MAX_VERBS];
static int g_ning;
static CnetTeNoun g_s3[CNET_TE_MAX_VERBS];
static int g_ns3;
static CnetTeNoun g_comp[CNET_TE_MAX_VERBS];
static int g_ncomp;
static CnetTeNoun g_super[CNET_TE_MAX_VERBS];
static int g_nsuper;
static CnetTeNoun g_adv[CNET_TE_MAX_VERBS];
static int g_nadv;

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

static uint64_t fnv1a(const char *s) {
    uint64_t h = 14695981039346656037ULL;
    if (!s) return h;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static void stolower_copy(char *d, size_t cap, const char *s) {
    size_t i, j = 0;
    if (!d || !cap) return;
    for (i = 0; s && s[i] && j + 1 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '?' || c == '!' || c == '.' || c == ',' || c == ';' ||
            c == ':' || c == '"' || c == '\'' || c == '(' || c == ')')
            continue;
        d[j++] = (char)tolower(c);
    }
    d[j] = 0;
}

static void squeeze_space(char *s) {
    char *r = s, *w = s;
    int sp = 1;
    if (!s) return;
    while (*r) {
        if (*r == ' ' || *r == '\t' || *r == '\n') {
            if (!sp) {
                *w++ = ' ';
                sp = 1;
            }
        } else {
            *w++ = *r;
            sp = 0;
        }
        r++;
    }
    if (w > s && w[-1] == ' ') w--;
    *w = 0;
}

static int add_verb(const char *lemma, const char *past, const char *part) {
    CnetTeVerb *v;
    if (!lemma || !lemma[0] || g_nverb >= CNET_TE_MAX_VERBS) return -1;
    v = &g_verbs[g_nverb++];
    memset(v, 0, sizeof *v);
    scopy(v->lemma, sizeof v->lemma, lemma);
    scopy(v->past, sizeof v->past, past);
    scopy(v->part, sizeof v->part, part);
    v->hash = fnv1a(lemma);
    v->used = 1;
    return 0;
}

static CnetTeVerb *find_verb(const char *lemma) {
    int i;
    uint64_t h;
    if (!lemma || !lemma[0]) return NULL;
    h = fnv1a(lemma);
    for (i = 0; i < g_nverb; i++) {
        if (!g_verbs[i].used) continue;
        if (g_verbs[i].hash != h) continue;
        if (strcmp(g_verbs[i].lemma, lemma) == 0) return &g_verbs[i];
    }
    for (i = 0; i < g_nverb; i++) {
        if (g_verbs[i].used && strcmp(g_verbs[i].lemma, lemma) == 0)
            return &g_verbs[i];
    }
    return NULL;
}

static int add_noun(const char *lemma, const char *plural) {
    CnetTeNoun *n;
    if (!lemma || !lemma[0] || g_nnoun >= CNET_TE_MAX_VERBS) return -1;
    n = &g_nouns[g_nnoun++];
    memset(n, 0, sizeof *n);
    scopy(n->lemma, sizeof n->lemma, lemma);
    scopy(n->plural, sizeof n->plural, plural ? plural : "");
    n->hash = fnv1a(lemma);
    n->used = 1;
    return 0;
}

static CnetTeNoun *find_noun(const char *lemma) {
    int i;
    uint64_t h;
    if (!lemma || !lemma[0]) return NULL;
    h = fnv1a(lemma);
    for (i = 0; i < g_nnoun; i++) {
        if (!g_nouns[i].used) continue;
        if (g_nouns[i].hash != h) continue;
        if (strcmp(g_nouns[i].lemma, lemma) == 0) return &g_nouns[i];
    }
    return NULL;
}

static int add_art(const char *lemma, const char *art) {
    CnetTeArtRow *a;
    if (!lemma || !lemma[0] || !art || !art[0] || g_nart >= CNET_TE_MAX_VERBS)
        return -1;
    if (strcmp(art, "a") != 0 && strcmp(art, "an") != 0) return -1;
    a = &g_arts[g_nart++];
    memset(a, 0, sizeof *a);
    scopy(a->lemma, sizeof a->lemma, lemma);
    scopy(a->art, sizeof a->art, art);
    a->hash = fnv1a(lemma);
    a->used = 1;
    return 0;
}

static CnetTeArtRow *find_art(const char *lemma) {
    int i;
    uint64_t h;
    if (!lemma || !lemma[0]) return NULL;
    h = fnv1a(lemma);
    for (i = 0; i < g_nart; i++) {
        if (!g_arts[i].used) continue;
        if (g_arts[i].hash != h) continue;
        if (strcmp(g_arts[i].lemma, lemma) == 0) return &g_arts[i];
    }
    return NULL;
}

static int upsert_noun(const char *lemma, const char *plural) {
    CnetTeNoun *n;
    char lem[CNET_TE_LEMMA], pl[CNET_TE_FORM];
    if (!lemma || !lemma[0] || !plural || !plural[0]) return -1;
    stolower_copy(lem, sizeof lem, lemma);
    squeeze_space(lem);
    stolower_copy(pl, sizeof pl, plural);
    squeeze_space(pl);
    if (!lem[0] || !pl[0]) return -1;
    n = find_noun(lem);
    if (!n) return add_noun(lem, pl);
    if (!n->plural[0]) scopy(n->plural, sizeof n->plural, pl);
    return 0;
}

static int upsert_art(const char *lemma, const char *art) {
    CnetTeArtRow *a;
    char lem[CNET_TE_LEMMA], ar[8];
    if (!lemma || !lemma[0] || !art || !art[0]) return -1;
    stolower_copy(lem, sizeof lem, lemma);
    squeeze_space(lem);
    stolower_copy(ar, sizeof ar, art);
    squeeze_space(ar);
    if (!lem[0] || (strcmp(ar, "a") != 0 && strcmp(ar, "an") != 0)) return -1;
    a = find_art(lem);
    if (!a) return add_art(lem, ar);
    if (!a->art[0]) scopy(a->art, sizeof a->art, ar);
    return 0;
}

static int add_form_row(CnetTeNoun *tab, int *n, const char *lemma, const char *form) {
    CnetTeNoun *row;
    if (!lemma || !lemma[0] || !n || *n >= CNET_TE_MAX_VERBS) return -1;
    row = &tab[(*n)++];
    memset(row, 0, sizeof *row);
    scopy(row->lemma, sizeof row->lemma, lemma);
    scopy(row->plural, sizeof row->plural, form ? form : "");
    row->hash = fnv1a(lemma);
    row->used = 1;
    return 0;
}

static CnetTeNoun *find_form_row(CnetTeNoun *tab, int n, const char *lemma) {
    int i;
    uint64_t h;
    if (!lemma || !lemma[0] || !tab) return NULL;
    h = fnv1a(lemma);
    for (i = 0; i < n; i++) {
        if (!tab[i].used) continue;
        if (tab[i].hash != h) continue;
        if (strcmp(tab[i].lemma, lemma) == 0) return &tab[i];
    }
    return NULL;
}

static int upsert_form_row(CnetTeNoun *tab, int *n, const char *lemma,
                           const char *form) {
    CnetTeNoun *row;
    char lem[CNET_TE_LEMMA], fm[CNET_TE_FORM];
    if (!lemma || !lemma[0] || !form || !form[0]) return -1;
    stolower_copy(lem, sizeof lem, lemma);
    squeeze_space(lem);
    stolower_copy(fm, sizeof fm, form);
    squeeze_space(fm);
    if (!lem[0] || !fm[0]) return -1;
    row = find_form_row(tab, *n, lem);
    if (!row) return add_form_row(tab, n, lem, fm);
    if (!row->plural[0]) scopy(row->plural, sizeof row->plural, fm);
    return 0;
}

/* which: 0 = noun, 1 = article, 2 = gerund, 3 = 3sg */
static int load_two_col(const char *path, int which) {
    FILE *f;
    char line[256];
    if (!path || !path[0]) return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *tab, *nl;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        tab++;
        if (!line[0] || !tab[0]) continue;
        if (which == 0)
            (void)upsert_noun(line, tab);
        else if (which == 1)
            (void)upsert_art(line, tab);
        else if (which == 2)
            (void)upsert_form_row(g_ings, &g_ning, line, tab);
        else if (which == 3)
            (void)upsert_form_row(g_s3, &g_ns3, line, tab);
        else if (which == 4)
            (void)upsert_form_row(g_comp, &g_ncomp, line, tab);
        else if (which == 5)
            (void)upsert_form_row(g_super, &g_nsuper, line, tab);
        else if (which == 6)
            (void)upsert_form_row(g_adv, &g_nadv, line, tab);
    }
    fclose(f);
    return 0;
}

static int overlay_which(const char *rel) {
    if (!rel) return 0;
    if (strstr(rel, "article")) return 1;
    if (strstr(rel, "3sg")) return 3;
    if (strstr(rel, "gerund")) return 2;
    if (strstr(rel, "compar")) return 4;
    if (strstr(rel, "superl")) return 5;
    if (strstr(rel, "adverb")) return 6;
    return 0;
}

static void te_try_overlay(const char *rel) {
    char p[512];
    const char *min, *pk;
    int which = overlay_which(rel);
    (void)load_two_col(rel, which);
    min = getenv("CNET_MINIMAL_ROOT");
    if (min && min[0]) {
        int n = snprintf(p, sizeof p, "%s/%s", min, rel);
        if (n > 0 && (size_t)n < sizeof p)
            (void)load_two_col(p, which);
    }
    pk = getenv("CNET_PACKS_ROOT");
    if (pk && pk[0]) {
        const char *base = strrchr(rel, '/');
        base = base ? base + 1 : rel;
        if (snprintf(p, sizeof p, "%s/%s", pk, base) < (int)sizeof p)
            (void)load_two_col(p, which);
    }
}

void cnet_te_init(void) {
    if (g_ready) return;
    memset(g_verbs, 0, sizeof g_verbs);
    g_nverb = 0;
    memset(g_nouns, 0, sizeof g_nouns);
    g_nnoun = 0;
    memset(g_arts, 0, sizeof g_arts);
    g_nart = 0;
    memset(g_ings, 0, sizeof g_ings);
    g_ning = 0;
    memset(g_s3, 0, sizeof g_s3);
    g_ns3 = 0;
    memset(g_comp, 0, sizeof g_comp);
    g_ncomp = 0;
    memset(g_super, 0, sizeof g_super);
    g_nsuper = 0;
    memset(g_adv, 0, sizeof g_adv);
    g_nadv = 0;
    add_verb("go", "went", "gone");
    add_verb("see", "saw", "seen");
    add_verb("eat", "ate", "eaten");
    add_verb("write", "wrote", "written");
    add_verb("come", "came", "come");
    add_verb("take", "took", "taken");
    add_verb("give", "gave", "given");
    add_verb("get", "got", "gotten");
    add_verb("make", "made", "made");
    add_verb("know", "knew", "known");
    add_verb("think", "thought", "thought");
    add_verb("say", "said", "said");
    add_verb("tell", "told", "told");
    add_verb("fly", "flew", "flown");
    add_verb("begin", "began", "begun");
    add_verb("break", "broke", "broken");
    add_verb("bring", "brought", "brought");
    add_verb("build", "built", "built");
    add_verb("buy", "bought", "bought");
    add_verb("catch", "caught", "caught");
    add_verb("choose", "chose", "chosen");
    add_verb("do", "did", "done");
    add_verb("draw", "drew", "drawn");
    add_verb("drink", "drank", "drunk");
    add_verb("drive", "drove", "driven");
    add_verb("fall", "fell", "fallen");
    add_verb("feel", "felt", "felt");
    add_verb("find", "found", "found");
    add_verb("forget", "forgot", "forgotten");
    add_verb("grow", "grew", "grown");
    add_verb("have", "had", "had");
    add_verb("hear", "heard", "heard");
    add_verb("hold", "held", "held");
    add_verb("keep", "kept", "kept");
    add_verb("leave", "left", "left");
    add_verb("lose", "lost", "lost");
    add_verb("meet", "met", "met");
    add_verb("pay", "paid", "paid");
    add_verb("read", "read", "read");
    add_verb("run", "ran", "run");
    add_verb("sit", "sat", "sat");
    add_verb("sleep", "slept", "slept");
    add_verb("speak", "spoke", "spoken");
    add_verb("stand", "stood", "stood");
    add_verb("teach", "taught", "taught");
    add_verb("understand", "understood", "understood");
    add_verb("wear", "wore", "worn");
    add_verb("win", "won", "won");
    g_ready = 1;
    (void)cnet_te_load_tsv("config/en_irregular_verbs.tsv");
    {
        const char *min = getenv("CNET_MINIMAL_ROOT");
        char p[512];
        if (min && min[0]) {
            int n = snprintf(p, sizeof p, "%s/config/en_irregular_verbs.tsv", min);
            if (n > 0 && (size_t)n < sizeof p) (void)cnet_te_load_tsv(p);
        }
    }
    {
        char exe[512], path[512], *slash;
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            slash = strrchr(exe, '/');
            if (slash) {
                *slash = 0;
                slash = strrchr(exe, '/');
                if (slash) *slash = 0;
            }
            if (snprintf(path, sizeof path, "%s/config/en_irregular_verbs.tsv",
                         exe) < (int)sizeof path)
                (void)cnet_te_load_tsv(path);
        }
    }
    {
        const char *pk = getenv("CNET_PACKS_ROOT");
        char p[512];
        if (pk && pk[0]) {
            int n = snprintf(p, sizeof p, "%s/en_irregular_gold.tsv", pk);
            if (n > 0 && (size_t)n < sizeof p) (void)cnet_te_load_tsv(p);
        }
        (void)cnet_te_load_tsv("artifacts/roe_daily_packs/en_irregular_gold.tsv");
    }
    te_try_overlay("config/en_plurals.tsv");
    te_try_overlay("config/en_articles.tsv");
    te_try_overlay("config/en_gerunds.tsv");
    te_try_overlay("config/en_3sg.tsv");
    te_try_overlay("config/en_comparatives.tsv");
    te_try_overlay("config/en_superlatives.tsv");
    te_try_overlay("config/en_adverbs.tsv");
    {
        const char *pk = getenv("CNET_PACKS_ROOT");
        char p[512];
        if (pk && pk[0]) {
            if (snprintf(p, sizeof p, "%s/en_plurals_gold.tsv", pk) < (int)sizeof p)
                (void)load_two_col(p, 0);
            if (snprintf(p, sizeof p, "%s/en_articles_gold.tsv", pk) < (int)sizeof p)
                (void)load_two_col(p, 1);
            if (snprintf(p, sizeof p, "%s/en_gerunds_gold.tsv", pk) < (int)sizeof p)
                (void)load_two_col(p, 2);
            if (snprintf(p, sizeof p, "%s/en_3sg_gold.tsv", pk) < (int)sizeof p)
                (void)load_two_col(p, 3);
            if (snprintf(p, sizeof p, "%s/en_comparatives_gold.tsv", pk) < (int)sizeof p)
                (void)load_two_col(p, 4);
            if (snprintf(p, sizeof p, "%s/en_superlatives_gold.tsv", pk) < (int)sizeof p)
                (void)load_two_col(p, 5);
            if (snprintf(p, sizeof p, "%s/en_adverbs_gold.tsv", pk) < (int)sizeof p)
                (void)load_two_col(p, 6);
        }
        (void)load_two_col("artifacts/roe_daily_packs/en_plurals_gold.tsv", 0);
        (void)load_two_col("artifacts/roe_daily_packs/en_articles_gold.tsv", 1);
        (void)load_two_col("artifacts/roe_daily_packs/en_gerunds_gold.tsv", 2);
        (void)load_two_col("artifacts/roe_daily_packs/en_3sg_gold.tsv", 3);
        (void)load_two_col("artifacts/roe_daily_packs/en_comparatives_gold.tsv", 4);
        (void)load_two_col("artifacts/roe_daily_packs/en_superlatives_gold.tsv", 5);
        (void)load_two_col("artifacts/roe_daily_packs/en_adverbs_gold.tsv", 6);
    }
}

int cnet_te_load_tsv(const char *path) {
    FILE *f;
    char line[256];
    if (!path || !path[0]) return -1;
    cnet_te_init();
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *tab1, *tab2, *nl;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        tab1 = strchr(line, '\t');
        if (!tab1) continue;
        *tab1 = 0;
        tab1++;
        tab2 = strchr(tab1, '\t');
        if (tab2) {
            *tab2 = 0;
            tab2++;
        } else {
            tab2 = tab1;
        }
        if (!line[0]) continue;
        (void)cnet_te_upsert(line, tab1[0] ? tab1 : NULL, tab2[0] ? tab2 : NULL);
    }
    fclose(f);
    return 0;
}

static int starts(const char *s, const char *p) {
    size_t n;
    if (!s || !p) return 0;
    n = strlen(p);
    return strncmp(s, p, n) == 0;
}

static int parse_kind(const char *norm, CnetTeKind *kind, const char **rest) {
    static const struct {
        const char *pat;
        CnetTeKind kind;
    } g[] = {
        {"past participle of ", CNET_TE_PART},
        {"present participle of ", CNET_TE_GERUND},
        {"what is the past tense of ", CNET_TE_PAST},
        {"what is the past of ", CNET_TE_PAST},
        {"past tense of ", CNET_TE_PAST},
        {"past of ", CNET_TE_PAST},
        {"what is the plural of ", CNET_TE_PLURAL},
        {"plural of ", CNET_TE_PLURAL},
        {"third-person singular of ", CNET_TE_S3},
        {"third person of ", CNET_TE_S3},
        {"3sg of ", CNET_TE_S3},
        {"gerund of ", CNET_TE_GERUND},
        {"comparative of ", CNET_TE_COMP},
        {"superlative of ", CNET_TE_SUPER},
        {"adverb of ", CNET_TE_ADV},
        {"ly form of ", CNET_TE_ADV},
        {"un- form of ", CNET_TE_UN},
        {"un form of ", CNET_TE_UN},
        {"re- form of ", CNET_TE_RE},
        {"re form of ", CNET_TE_RE},
        {"ness of ", CNET_TE_NESS},
        {"able form of ", CNET_TE_ABLE},
        {"able of ", CNET_TE_ABLE},
        {"a or an ", CNET_TE_ART},
        {"an or a ", CNET_TE_ART},
        {NULL, CNET_TE_NONE},
    };
    int i;
    for (i = 0; g[i].pat; i++) {
        if (starts(norm, g[i].pat)) {
            *kind = g[i].kind;
            *rest = norm + strlen(g[i].pat);
            return 1;
        }
    }
    if (strcmp(norm, "a or an") == 0 || strcmp(norm, "an or a") == 0) {
        *kind = CNET_TE_ART;
        *rest = "";
        return 1;
    }
    return 0;
}

static int split_lemmas(const char *rest, char lemmas[][CNET_TE_LEMMA], int maxn) {
    char buf[256];
    char *p, *tok;
    int n = 0;
    scopy(buf, sizeof buf, rest);
    p = buf;
    while (p && *p && n < maxn) {
        while (*p == ' ') p++;
        tok = p;
        while (*p && !(p[0] == ' ' && p[1] == 'a' && p[2] == 'n' && p[3] == 'd' &&
                       p[4] == ' ') &&
               !(p[0] == ' ' && p[1] == 'o' && p[2] == 'r' && p[3] == ' ') &&
               *p != ',')
            p++;
        if (*p) {
            char cut = *p;
            *p = 0;
            if (cut == ',')
                p++;
            else if (cut == ' ' && p[1] == 'a')
                p += 5; /* " and " */
            else
                p += 4; /* " or " */
        }
        if (tok[0] && strcmp(tok, "and") != 0 && strcmp(tok, "or") != 0) {
            scopy(lemmas[n], CNET_TE_LEMMA, tok);
            squeeze_space(lemmas[n]);
            if (lemmas[n][0]) n++;
        }
        if (!*p) break;
    }
    return n;
}

static int te_vowel(char c) {
    return c == 'a' || c == 'e' || c == 'i' || c == 'o' || c == 'u';
}

static int te_has_vowel(const char *s) {
    const char *p;
    size_t n;
    if (!s) return 0;
    for (p = s; *p; p++)
        if (te_vowel(*p)) return 1;
    /* try/cry/dry: y is the vowel. xyzzy (len 5, no aeiou) still refuses. */
    n = strlen(s);
    if (n >= 3 && n <= 4 && s[n - 1] == 'y' && !te_vowel(s[n - 2]))
        return 1;
    return 0;
}

/* One-syllable-ish CVC → double final cons (stop→stopped). Skip w/x/y. */
static int te_cvc_double(const char *s) {
    size_t n;
    char a, b, c;
    if (!s) return 0;
    n = strlen(s);
    if (n < 3 || n > 4) return 0;
    c = s[n - 1];
    b = s[n - 2];
    a = s[n - 3];
    if (te_vowel(c) || c == 'w' || c == 'x' || c == 'y') return 0;
    if (!te_vowel(b) || te_vowel(a)) return 0;
    return 1;
}

/* Certified regular-past transducer. Exceptions live in the table and win.
 * Lemmas with no aeiou (xyzzy) refuse — not a verb we will -ed. */
static int te_regular_form(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || cap < 8) return -1;
    n = strlen(lemma);
    if (n < 2 || n + 4 >= cap || n + 4 >= CNET_TE_FORM) return -1;
    if (!te_has_vowel(lemma)) return -1;
    if (lemma[n - 1] == 'y' && n >= 2 && !te_vowel(lemma[n - 2])) {
        if (snprintf(out, cap, "%.*sied", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (lemma[n - 1] == 'e') {
        if (snprintf(out, cap, "%sd", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (te_cvc_double(lemma)) {
        if (snprintf(out, cap, "%s%ced", lemma, lemma[n - 1]) >= (int)cap)
            return -1;
        return 0;
    }
    if (snprintf(out, cap, "%sed", lemma) >= (int)cap) return -1;
    return 0;
}

/* Certified regular-plural transducer. Irregulars live in overlay and win. */
static int te_regular_plural(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || cap < 8) return -1;
    n = strlen(lemma);
    if (n < 1 || n + 4 >= cap || n + 4 >= CNET_TE_FORM) return -1;
    if (!te_has_vowel(lemma)) return -1;
    if (lemma[n - 1] == 's' || lemma[n - 1] == 'x' || lemma[n - 1] == 'z') {
        if (snprintf(out, cap, "%ses", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (n >= 2 && lemma[n - 2] == 'c' && lemma[n - 1] == 'h') {
        if (snprintf(out, cap, "%ses", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (n >= 2 && lemma[n - 2] == 's' && lemma[n - 1] == 'h') {
        if (snprintf(out, cap, "%ses", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (lemma[n - 1] == 'y' && n >= 2 && !te_vowel(lemma[n - 2])) {
        if (snprintf(out, cap, "%.*sies", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (snprintf(out, cap, "%ss", lemma) >= (int)cap) return -1;
    return 0;
}

/* Spelling vowel-letter. Sound exceptions (hour, university) are overlay. */
static int te_regular_art(const char *lemma, char *out, size_t cap) {
    if (!lemma || !lemma[0] || !out || cap < 4) return -1;
    if (!isalpha((unsigned char)lemma[0])) return -1;
    if (snprintf(out, cap, "%s", te_vowel(lemma[0]) ? "an" : "a") >= (int)cap)
        return -1;
    return 0;
}

/* Certified present-participle transducer. Overlay wins for true exceptions. */
static int te_regular_ing(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || cap < 8) return -1;
    n = strlen(lemma);
    if (n < 1 || n + 5 >= cap || n + 5 >= CNET_TE_FORM) return -1;
    if (!te_has_vowel(lemma)) return -1;
    if (n >= 2 && lemma[n - 2] == 'i' && lemma[n - 1] == 'e') {
        if (snprintf(out, cap, "%.*sying", (int)(n - 2), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (n >= 2 && lemma[n - 1] == 'e' &&
        (lemma[n - 2] == 'e' || lemma[n - 2] == 'o')) {
        if (snprintf(out, cap, "%sing", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (n == 2 && lemma[n - 1] == 'e') {
        if (snprintf(out, cap, "%sing", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (lemma[n - 1] == 'e') {
        if (snprintf(out, cap, "%.*sing", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (te_cvc_double(lemma)) {
        if (snprintf(out, cap, "%s%cing", lemma, lemma[n - 1]) >= (int)cap)
            return -1;
        return 0;
    }
    if (snprintf(out, cap, "%sing", lemma) >= (int)cap) return -1;
    return 0;
}

/* Certified 3sg transducer. have/has and be/is are overlay. */
static int te_regular_3sg(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || cap < 8) return -1;
    n = strlen(lemma);
    if (n < 1 || n + 4 >= cap || n + 4 >= CNET_TE_FORM) return -1;
    if (!te_has_vowel(lemma)) return -1;
    if (lemma[n - 1] == 's' || lemma[n - 1] == 'x' || lemma[n - 1] == 'z') {
        if (snprintf(out, cap, "%ses", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (n >= 2 && lemma[n - 2] == 'c' && lemma[n - 1] == 'h') {
        if (snprintf(out, cap, "%ses", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (n >= 2 && lemma[n - 2] == 's' && lemma[n - 1] == 'h') {
        if (snprintf(out, cap, "%ses", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (lemma[n - 1] == 'y' && n >= 2 && !te_vowel(lemma[n - 2])) {
        if (snprintf(out, cap, "%.*sies", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (lemma[n - 1] == 'o' && n >= 2 && !te_vowel(lemma[n - 2])) {
        if (snprintf(out, cap, "%ses", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (snprintf(out, cap, "%ss", lemma) >= (int)cap) return -1;
    return 0;
}

static int te_regular_er(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || cap < 8) return -1;
    n = strlen(lemma);
    if (n < 1 || n + 4 >= cap || n + 4 >= CNET_TE_FORM) return -1;
    if (!te_has_vowel(lemma)) return -1;
    if (lemma[n - 1] == 'y' && n >= 2 && !te_vowel(lemma[n - 2])) {
        if (snprintf(out, cap, "%.*sier", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (lemma[n - 1] == 'e') {
        if (snprintf(out, cap, "%sr", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (te_cvc_double(lemma)) {
        if (snprintf(out, cap, "%s%cer", lemma, lemma[n - 1]) >= (int)cap)
            return -1;
        return 0;
    }
    if (snprintf(out, cap, "%ser", lemma) >= (int)cap) return -1;
    return 0;
}

static int te_regular_est(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || cap < 8) return -1;
    n = strlen(lemma);
    if (n < 1 || n + 5 >= cap || n + 5 >= CNET_TE_FORM) return -1;
    if (!te_has_vowel(lemma)) return -1;
    if (lemma[n - 1] == 'y' && n >= 2 && !te_vowel(lemma[n - 2])) {
        if (snprintf(out, cap, "%.*siest", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (lemma[n - 1] == 'e') {
        if (snprintf(out, cap, "%sst", lemma) >= (int)cap) return -1;
        return 0;
    }
    if (te_cvc_double(lemma)) {
        if (snprintf(out, cap, "%s%cest", lemma, lemma[n - 1]) >= (int)cap)
            return -1;
        return 0;
    }
    if (snprintf(out, cap, "%sest", lemma) >= (int)cap) return -1;
    return 0;
}

static int te_regular_ly(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || cap < 8) return -1;
    n = strlen(lemma);
    if (n < 1 || n + 4 >= cap || n + 4 >= CNET_TE_FORM) return -1;
    if (!te_has_vowel(lemma)) return -1;
    if (lemma[n - 1] == 'y' && n >= 2 && !te_vowel(lemma[n - 2])) {
        if (snprintf(out, cap, "%.*sily", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (snprintf(out, cap, "%sly", lemma) >= (int)cap) return -1;
    return 0;
}

static int te_regular_un(const char *lemma, char *out, size_t cap) {
    if (!lemma || !out || !te_has_vowel(lemma)) return -1;
    if (snprintf(out, cap, "un%s", lemma) >= (int)cap) return -1;
    return 0;
}

static int te_regular_re(const char *lemma, char *out, size_t cap) {
    if (!lemma || !out || !te_has_vowel(lemma)) return -1;
    if (snprintf(out, cap, "re%s", lemma) >= (int)cap) return -1;
    return 0;
}

static int te_regular_ness(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || !te_has_vowel(lemma)) return -1;
    n = strlen(lemma);
    if (n >= 2 && lemma[n - 1] == 'y' && !te_vowel(lemma[n - 2])) {
        if (snprintf(out, cap, "%.*siness", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (snprintf(out, cap, "%sness", lemma) >= (int)cap) return -1;
    return 0;
}

static int te_regular_able(const char *lemma, char *out, size_t cap) {
    size_t n;
    if (!lemma || !out || !te_has_vowel(lemma)) return -1;
    n = strlen(lemma);
    if (n >= 2 && lemma[n - 1] == 'e') {
        if (snprintf(out, cap, "%.*sable", (int)(n - 1), lemma) >= (int)cap)
            return -1;
        return 0;
    }
    if (snprintf(out, cap, "%sable", lemma) >= (int)cap) return -1;
    return 0;
}

static const char *te_rule_tag(const CnetTeHop *h) {
    if (!h || !h->via_rule) return h && h->local ? " TABLE" : "";
    if (h->kind == CNET_TE_GERUND) return " RULE=ing";
    if (h->kind == CNET_TE_COMP) return " RULE=er";
    if (h->kind == CNET_TE_SUPER) return " RULE=est";
    if (h->kind == CNET_TE_ADV) return " RULE=ly";
    if (h->kind == CNET_TE_UN) return " RULE=un";
    if (h->kind == CNET_TE_RE) return " RULE=re";
    if (h->kind == CNET_TE_NESS) return " RULE=ness";
    if (h->kind == CNET_TE_ABLE) return " RULE=able";
    if (h->kind == CNET_TE_PLURAL || h->kind == CNET_TE_S3) return " RULE=s";
    if (h->kind == CNET_TE_ART) return " RULE=vowel-letter";
    return " RULE=ed";
}

static void fill_show(CnetTeHop *h) {
    const char *kname = "NONE";
    if (h->kind == CNET_TE_PAST) kname = "PAST";
    else if (h->kind == CNET_TE_PART) kname = "PART";
    else if (h->kind == CNET_TE_PLURAL) kname = "PLURAL";
    else if (h->kind == CNET_TE_ART) kname = "ART";
    else if (h->kind == CNET_TE_GERUND) kname = "GERUND";
    else if (h->kind == CNET_TE_S3) kname = "S3";
    else if (h->kind == CNET_TE_COMP) kname = "COMP";
    else if (h->kind == CNET_TE_SUPER) kname = "SUPER";
    else if (h->kind == CNET_TE_ADV) kname = "ADV";
    else if (h->kind == CNET_TE_UN) kname = "UN";
    else if (h->kind == CNET_TE_RE) kname = "RE";
    else if (h->kind == CNET_TE_NESS) kname = "NESS";
    else if (h->kind == CNET_TE_ABLE) kname = "ABLE";
    else if (h->kind == CNET_TE_ABSTAIN) kname = "ABSTAIN";
    snprintf(h->show, sizeof h->show,
             "%s lemma=%s hash=%016llx form=%s %s%s", kname, h->lemma,
             (unsigned long long)h->hash, h->form[0] ? h->form : "-",
             h->local ? "LOCAL" : (h->gap ? "GAP" : "MISS"),
             te_rule_tag(h));
}

int cnet_te_audit_ok(const CnetTeResult *r) {
    int i;
    if (!r || r->n_cand < 1) return 0;
    if (!r->show[0]) return 0;
    for (i = 0; i < r->n_cand; i++) {
        const CnetTeHop *h = &r->cand[i];
        if (!h->show[0] || !h->lemma[0]) return 0;
        if (!strstr(h->show, h->lemma)) return 0;
        if (h->kind == CNET_TE_ABSTAIN || h->gap) {
            if (!strstr(h->show, "ABSTAIN") && !strstr(h->show, "GAP"))
                return 0;
        } else if (h->local) {
            if (!h->form[0] || !strstr(h->show, h->form)) return 0;
            if (!strstr(h->show, "PAST") && !strstr(h->show, "PART") &&
                !strstr(h->show, "PLURAL") && !strstr(h->show, "ART") &&
                !strstr(h->show, "GERUND") && !strstr(h->show, "S3") &&
                !strstr(h->show, "COMP") && !strstr(h->show, "SUPER") &&
                !strstr(h->show, "ADV"))
                return 0;
        }
    }
    return 1;
}

int cnet_te_ask(const char *q, CnetTeResult *out) {
    char norm[256];
    const char *rest = NULL;
    CnetTeKind kind = CNET_TE_NONE;
    char lemmas[CNET_TE_MAX_CAND][CNET_TE_LEMMA];
    int n = 0, i, pos = 0;

    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!q || !q[0]) return -1;
    cnet_te_init();
    stolower_copy(norm, sizeof norm, q);
    squeeze_space(norm);
    if (!parse_kind(norm, &kind, &rest)) {
        /* no grammar → not our student; SHOW exists but grammar_hit=0 */
        out->grammar_hit = 0;
        out->n_cand = 1;
        out->cand[0].kind = CNET_TE_ABSTAIN;
        scopy(out->cand[0].lemma, sizeof out->cand[0].lemma, "unk");
        out->cand[0].hash = fnv1a(norm);
        out->cand[0].gap = 1;
        fill_show(&out->cand[0]);
        scopy(out->show, sizeof out->show, out->cand[0].show);
        out->any_gap = 1;
        return 0;
    }
    out->grammar_hit = 1;
    n = split_lemmas(rest, lemmas, CNET_TE_MAX_CAND);
    if (n < 1) {
        out->n_cand = 1;
        out->cand[0].kind = CNET_TE_ABSTAIN;
        scopy(out->cand[0].lemma, sizeof out->cand[0].lemma, "unk");
        out->cand[0].gap = 1;
        fill_show(&out->cand[0]);
        scopy(out->show, sizeof out->show, out->cand[0].show);
        out->any_gap = 1;
        return 0;
    }
    out->n_cand = n;
    out->all_local = 1;
    for (i = 0; i < n; i++) {
        CnetTeHop *h = &out->cand[i];
        h->kind = (int)kind;
        scopy(h->lemma, sizeof h->lemma, lemmas[i]);
        h->hash = fnv1a(h->lemma);
        if (kind == CNET_TE_PLURAL) {
            const CnetTeNoun *nn = find_noun(h->lemma);
            if (nn && nn->plural[0]) {
                scopy(h->form, sizeof h->form, nn->plural);
                h->local = 1;
                h->gap = 0;
            } else if (te_regular_plural(h->lemma, h->form, sizeof h->form) == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        } else if (kind == CNET_TE_GERUND) {
            const CnetTeNoun *ing = find_form_row(g_ings, g_ning, h->lemma);
            if (ing && ing->plural[0]) {
                scopy(h->form, sizeof h->form, ing->plural);
                h->local = 1;
                h->gap = 0;
            } else if (te_regular_ing(h->lemma, h->form, sizeof h->form) == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        } else if (kind == CNET_TE_S3) {
            const CnetTeNoun *s3 = find_form_row(g_s3, g_ns3, h->lemma);
            if (s3 && s3->plural[0]) {
                scopy(h->form, sizeof h->form, s3->plural);
                h->local = 1;
                h->gap = 0;
            } else if (te_regular_3sg(h->lemma, h->form, sizeof h->form) == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        } else if (kind == CNET_TE_COMP) {
            const CnetTeNoun *row = find_form_row(g_comp, g_ncomp, h->lemma);
            if (row && row->plural[0]) {
                scopy(h->form, sizeof h->form, row->plural);
                h->local = 1;
                h->gap = 0;
            } else if (te_regular_er(h->lemma, h->form, sizeof h->form) == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        } else if (kind == CNET_TE_SUPER) {
            const CnetTeNoun *row = find_form_row(g_super, g_nsuper, h->lemma);
            if (row && row->plural[0]) {
                scopy(h->form, sizeof h->form, row->plural);
                h->local = 1;
                h->gap = 0;
            } else if (te_regular_est(h->lemma, h->form, sizeof h->form) == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        } else if (kind == CNET_TE_ADV) {
            const CnetTeNoun *row = find_form_row(g_adv, g_nadv, h->lemma);
            if (row && row->plural[0]) {
                scopy(h->form, sizeof h->form, row->plural);
                h->local = 1;
                h->gap = 0;
            } else if (te_regular_ly(h->lemma, h->form, sizeof h->form) == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        } else if (kind == CNET_TE_UN || kind == CNET_TE_RE ||
                   kind == CNET_TE_NESS || kind == CNET_TE_ABLE) {
            int rc = -1;
            if (kind == CNET_TE_UN)
                rc = te_regular_un(h->lemma, h->form, sizeof h->form);
            else if (kind == CNET_TE_RE)
                rc = te_regular_re(h->lemma, h->form, sizeof h->form);
            else if (kind == CNET_TE_NESS)
                rc = te_regular_ness(h->lemma, h->form, sizeof h->form);
            else
                rc = te_regular_able(h->lemma, h->form, sizeof h->form);
            if (rc == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        } else if (kind == CNET_TE_ART) {
            const CnetTeArtRow *ar = find_art(h->lemma);
            if (ar && ar->art[0]) {
                scopy(h->form, sizeof h->form, ar->art);
                h->local = 1;
                h->gap = 0;
            } else if (te_regular_art(h->lemma, h->form, sizeof h->form) == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        } else {
            const CnetTeVerb *v = find_verb(h->lemma);
            if (v) {
                const char *form = (kind == CNET_TE_PART) ? v->part : v->past;
                if (form && form[0]) {
                    scopy(h->form, sizeof h->form, form);
                    h->local = 1;
                    h->gap = 0;
                } else {
                    h->kind = CNET_TE_ABSTAIN;
                    h->local = 0;
                    h->gap = 1;
                    out->all_local = 0;
                    out->any_gap = 1;
                }
            } else if (te_regular_form(h->lemma, h->form, sizeof h->form) == 0) {
                h->local = 1;
                h->gap = 0;
                h->via_rule = 1;
            } else {
                h->kind = CNET_TE_ABSTAIN;
                h->local = 0;
                h->gap = 1;
                out->all_local = 0;
                out->any_gap = 1;
            }
        }
        fill_show(h);
        {
            int w = snprintf(out->show + pos, sizeof out->show - (size_t)pos,
                             "%s%s", pos ? " | " : "", h->show);
            if (w > 0) pos += w;
        }
    }
    if (out->all_local && !out->any_gap && cnet_te_audit_ok(out))
        out->claimed_cert = 1;
    else
        out->claimed_cert = 0;
    return 0;
}

int cnet_te_format_answer(const CnetTeResult *r, char *out, size_t cap) {
    int i, pos = 0;
    if (!r || !out || cap < 8) return -1;
    out[0] = 0;
    if (!r->grammar_hit || r->n_cand < 1) return -1;
    for (i = 0; i < r->n_cand; i++) {
        const CnetTeHop *h = &r->cand[i];
        int w;
        if (pos >= (int)cap - 8) break;
        if (h->local && h->form[0]) {
            w = snprintf(out + pos, cap - (size_t)pos, "%s%s -> %s.",
                         pos ? " " : "", h->lemma, h->form);
            if (w > 0) pos += w;
        } else if (h->lemma[0] && strcmp(h->lemma, "unk") != 0) {
            w = snprintf(out + pos, cap - (size_t)pos,
                         "%s(nothing sealed here for \"%s\" - ask it separately)",
                         pos ? "  " : "", h->lemma);
            if (w > 0) pos += w;
        }
    }
    return out[0] ? 0 : -1;
}

int cnet_te_persist_floats(const char *path) {
    (void)path;
    return -1; /* CNU 0/1 law: continuous thought is not a capsule */
}

int cnet_te_persist_discrete(const char *path, char *digest_hex, size_t cap) {
    FILE *f;
    int i;
    uint64_t h = 14695981039346656037ULL;
    if (!path || !digest_hex || cap < 17) return -1;
    cnet_te_init();
    f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "# cnet_typed_en discrete 0/1 occupancy\n");
    for (i = 0; i < g_nverb; i++) {
        const CnetTeVerb *v = &g_verbs[i];
        if (!v->used) continue;
        fprintf(f, "%s\t%s\t%s\n", v->lemma, v->past, v->part);
        {
            const char *parts[3];
            int pi, ci;
            parts[0] = v->lemma;
            parts[1] = v->past;
            parts[2] = v->part;
            for (pi = 0; pi < 3; pi++) {
                for (ci = 0; parts[pi][ci]; ci++) {
                    h ^= (unsigned char)parts[pi][ci];
                    h *= 1099511628211ULL;
                }
                h ^= (unsigned char)(pi < 2 ? '\t' : '\n');
                h *= 1099511628211ULL;
            }
        }
    }
    fclose(f);
    snprintf(digest_hex, cap, "%016llx", (unsigned long long)h);
    return 0;
}

int cnet_te_upsert(const char *lemma, const char *past, const char *part) {
    CnetTeVerb *v;
    char lem[CNET_TE_LEMMA], pst[CNET_TE_FORM], prt[CNET_TE_FORM];
    if (!lemma || !lemma[0]) return -1;
    cnet_te_init();
    stolower_copy(lem, sizeof lem, lemma);
    squeeze_space(lem);
    if (!lem[0]) return -1;
    pst[0] = 0;
    prt[0] = 0;
    if (past && past[0]) {
        stolower_copy(pst, sizeof pst, past);
        squeeze_space(pst);
    }
    if (part && part[0]) {
        stolower_copy(prt, sizeof prt, part);
        squeeze_space(prt);
    }
    v = find_verb(lem);
    if (!v) return add_verb(lem, pst, prt);
    if (pst[0] && !v->past[0]) scopy(v->past, sizeof v->past, pst);
    if (prt[0] && !v->part[0]) scopy(v->part, sizeof v->part, prt);
    return 0;
}

static int parse_gold_form(const char *answer, char *form, size_t cap) {
    char a[256];
    const char *p, *arrow;
    size_t i = 0;
    if (!form || !cap) return -1;
    form[0] = 0;
    stolower_copy(a, sizeof a, answer ? answer : "");
    squeeze_space(a);
    if (!a[0]) return -1;
    if (!strncmp(a, "abstain", 7)) return -1;
    if (!strncmp(a, "i don't know", 12) || !strncmp(a, "i do not know", 13))
        return -1;
    arrow = strstr(a, "->");
    p = arrow ? arrow + 2 : a;
    while (*p == ' ') p++;
    while (*p && *p != '.' && *p != ' ' && i + 1 < cap) {
        if (!isalpha((unsigned char)*p)) return -1;
        form[i++] = *p++;
    }
    form[i] = 0;
    while (*p == '.' || *p == ' ') p++;
    if (*p || !form[0]) return -1;
    return 0;
}

static int gold_tsv_append(const char *path, const char *lemma, const char *past,
                           const char *part) {
    FILE *f;
    if (!path || !path[0] || !lemma || !lemma[0]) return -1;
    f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "%s\t%s\t%s\n", lemma, past ? past : "", part ? part : "");
    fclose(f);
    return 0;
}

static int gold_two_col_append(const char *path, const char *lemma,
                               const char *form) {
    FILE *f;
    if (!path || !path[0] || !lemma || !lemma[0] || !form || !form[0]) return -1;
    f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "%s\t%s\n", lemma, form);
    fclose(f);
    return 0;
}

int cnet_te_gold_apply(const char *query, const char *answer, const char *tsv_path) {
    CnetTeResult r;
    CnetTeVerb *v;
    char form[CNET_TE_FORM];
    const char *lemma;
    int kind;
    if (cnet_te_ask(query, &r) != 0 || !r.grammar_hit) return 0;
    if (r.n_cand != 1) return 0;
    lemma = r.cand[0].lemma;
    if (!lemma[0] || strcmp(lemma, "unk") == 0) return 0;
    if (parse_gold_form(answer, form, sizeof form) != 0) return -1;
    v = find_verb(lemma);
    {
        char norm[256];
        const char *rest = NULL;
        CnetTeKind k = CNET_TE_NONE;
        stolower_copy(norm, sizeof norm, query);
        squeeze_space(norm);
        if (!parse_kind(norm, &k, &rest)) return 0;
        kind = (int)k;
    }
    if (kind == CNET_TE_PLURAL) {
        CnetTeNoun *n = find_noun(lemma);
        if (n && n->plural[0]) return 0;
        if (r.cand[0].via_rule && r.cand[0].form[0] &&
            strcmp(r.cand[0].form, form) == 0)
            return 0;
        if (upsert_noun(lemma, form) != 0) return -1;
        if (gold_two_col_append(tsv_path, lemma, form) != 0) return -1;
        return 1;
    }
    if (kind == CNET_TE_ART) {
        CnetTeArtRow *a = find_art(lemma);
        if (a && a->art[0]) return 0;
        if (strcmp(form, "a") != 0 && strcmp(form, "an") != 0) return -1;
        if (r.cand[0].via_rule && r.cand[0].form[0] &&
            strcmp(r.cand[0].form, form) == 0)
            return 0;
        if (upsert_art(lemma, form) != 0) return -1;
        if (gold_two_col_append(tsv_path, lemma, form) != 0) return -1;
        return 1;
    }
    if (kind == CNET_TE_GERUND) {
        CnetTeNoun *ing = find_form_row(g_ings, g_ning, lemma);
        if (ing && ing->plural[0]) return 0;
        if (r.cand[0].via_rule && r.cand[0].form[0] &&
            strcmp(r.cand[0].form, form) == 0)
            return 0;
        if (upsert_form_row(g_ings, &g_ning, lemma, form) != 0) return -1;
        if (gold_two_col_append(tsv_path, lemma, form) != 0) return -1;
        return 1;
    }
    if (kind == CNET_TE_S3) {
        CnetTeNoun *s3 = find_form_row(g_s3, g_ns3, lemma);
        if (s3 && s3->plural[0]) return 0;
        if (r.cand[0].via_rule && r.cand[0].form[0] &&
            strcmp(r.cand[0].form, form) == 0)
            return 0;
        if (upsert_form_row(g_s3, &g_ns3, lemma, form) != 0) return -1;
        if (gold_two_col_append(tsv_path, lemma, form) != 0) return -1;
        return 1;
    }
    if (kind == CNET_TE_COMP) {
        CnetTeNoun *row = find_form_row(g_comp, g_ncomp, lemma);
        if (row && row->plural[0]) return 0;
        if (r.cand[0].via_rule && r.cand[0].form[0] &&
            strcmp(r.cand[0].form, form) == 0)
            return 0;
        if (upsert_form_row(g_comp, &g_ncomp, lemma, form) != 0) return -1;
        if (gold_two_col_append(tsv_path, lemma, form) != 0) return -1;
        return 1;
    }
    if (kind == CNET_TE_SUPER) {
        CnetTeNoun *row = find_form_row(g_super, g_nsuper, lemma);
        if (row && row->plural[0]) return 0;
        if (r.cand[0].via_rule && r.cand[0].form[0] &&
            strcmp(r.cand[0].form, form) == 0)
            return 0;
        if (upsert_form_row(g_super, &g_nsuper, lemma, form) != 0) return -1;
        if (gold_two_col_append(tsv_path, lemma, form) != 0) return -1;
        return 1;
    }
    if (kind == CNET_TE_ADV) {
        CnetTeNoun *row = find_form_row(g_adv, g_nadv, lemma);
        if (row && row->plural[0]) return 0;
        if (r.cand[0].via_rule && r.cand[0].form[0] &&
            strcmp(r.cand[0].form, form) == 0)
            return 0;
        if (upsert_form_row(g_adv, &g_nadv, lemma, form) != 0) return -1;
        if (gold_two_col_append(tsv_path, lemma, form) != 0) return -1;
        return 1;
    }
    if (kind == CNET_TE_PAST) {
        if (v && v->past[0]) return 0;
        if (cnet_te_upsert(lemma, form, NULL) != 0) return -1;
        v = find_verb(lemma);
        if (gold_tsv_append(tsv_path, lemma, v ? v->past : form,
                            v ? v->part : "") != 0)
            return -1;
        return 1;
    }
    if (kind == CNET_TE_PART) {
        if (v && v->part[0]) return 0;
        if (cnet_te_upsert(lemma, NULL, form) != 0) return -1;
        v = find_verb(lemma);
        if (gold_tsv_append(tsv_path, lemma, v ? v->past : "",
                            v ? v->part : form) != 0)
            return -1;
        return 1;
    }
    return 0;
}

int cnet_te_selftest(void) {
    CnetTeResult r;
    char digest[80];
    int fail = 0;
    memset(digest, 0, sizeof digest);

#define Tst(ok, msg)                                                           \
    do {                                                                       \
        int _ok = (ok) ? 1 : 0;                                                \
        printf("  %-56s %s\n", msg, _ok ? "PASS" : "FAIL");                    \
        if (!_ok) fail++;                                                      \
    } while (0)

    printf("=== typed english student (discrete, audit SHOW) ===\n");
    cnet_te_init();

    Tst(cnet_te_ask("past tense of go", &r) == 0, "past tense of go rc");
    Tst(r.n_cand == 1 && r.cand[0].kind == CNET_TE_PAST, "one PAST hop");
    Tst(strcmp(r.cand[0].lemma, "go") == 0, "lemma go");
    Tst(strcmp(r.cand[0].form, "went") == 0, "form went");
    Tst(r.cand[0].local == 1 && r.claimed_cert == 1, "LOCAL CERT hop");
    Tst(cnet_te_audit_ok(&r) == 1, "SHOW audit ok");
    Tst(strstr(r.cand[0].show, "PAST") && strstr(r.cand[0].show, "go") &&
            strstr(r.cand[0].show, "went"),
        "SHOW names PAST/go/went");

    Tst(cnet_te_ask("past tense of xyzzy", &r) == 0, "unknown lemma rc");
    Tst(r.claimed_cert == 0 && r.cand[0].kind == CNET_TE_ABSTAIN,
        "unknown → ABSTAIN not CERT");
    Tst(cnet_te_audit_ok(&r) == 1 && strstr(r.cand[0].show, "ABSTAIN") &&
            strstr(r.cand[0].show, "xyzzy"),
        "ABSTAIN still has SHOW (hostility visible)");

    Tst(cnet_te_ask("past tense of go and see", &r) == 0, "go and see rc");
    Tst(r.n_cand == 2, "two candidates");
    Tst(r.cand[0].local && r.cand[1].local && r.claimed_cert == 1 && !r.any_gap,
        "both LOCAL, no gap");
    Tst(strcmp(r.cand[0].form, "went") == 0 && strcmp(r.cand[1].form, "saw") == 0,
        "went + saw");
    Tst(strstr(r.show, "go") && strstr(r.show, "see"), "audit lists both lemmas");

    Tst(cnet_te_ask("past tense of go and xyzzy", &r) == 0, "go and xyzzy rc");
    Tst(r.n_cand == 2 && r.any_gap == 1 && r.claimed_cert == 0,
        "half-cover is a gap, not whole-answer CERT");
    Tst(r.cand[0].local == 1 && r.cand[1].local == 0, "go local, xyzzy gap");

    {
        CnetTeResult bad = r;
        bad.cand[0].show[0] = 0;
        Tst(cnet_te_audit_ok(&bad) == 0, "empty SHOW fails audit (hostility)");
        bad = r;
        bad.show[0] = 0;
        Tst(cnet_te_audit_ok(&bad) == 0, "empty chain SHOW fails audit");
    }

    Tst(cnet_te_persist_floats("/tmp/cnet_te_float.bin") != 0,
        "float persist refused");
    Tst(cnet_te_persist_discrete("/tmp/cnet_te_discrete.tsv", digest,
                                 sizeof digest) == 0,
        "discrete persist ok");
    Tst(digest[0] != 0, "discrete digest nonempty");

    Tst(cnet_te_ask("who are you", &r) == 0 && r.grammar_hit == 0,
        "open chat is not morphology grammar");
    Tst(cnet_te_ask("past tense of go", &r) == 0 && r.grammar_hit == 1,
        "past tense of go is grammar");
    {
        char spoken[256];
        Tst(cnet_te_format_answer(&r, spoken, sizeof spoken) == 0, "format go");
        Tst(strstr(spoken, "go -> went") != NULL, "spoken go -> went");
        Tst(strstr(spoken, "hash=") == NULL, "spoken has no hash");
    }
    Tst(cnet_te_ask("past tense of go and see", &r) == 0, "format go and see ask");
    {
        char spoken[256];
        Tst(cnet_te_format_answer(&r, spoken, sizeof spoken) == 0 &&
                strstr(spoken, "go -> went") && strstr(spoken, "see -> saw"),
            "spoken both lemmas");
    }
    Tst(cnet_te_ask("past tense of go and xyzzy", &r) == 0, "format gap ask");
    {
        char spoken[256];
        Tst(cnet_te_format_answer(&r, spoken, sizeof spoken) == 0 &&
                strstr(spoken, "go -> went") && strstr(spoken, "xyzzy"),
            "spoken names gap lemma");
        Tst(r.claimed_cert == 0, "gap still not whole CERT");
    }

    Tst(cnet_te_ask("past tense of walk", &r) == 0 && r.claimed_cert == 1 &&
            strcmp(r.cand[0].form, "walked") == 0 && r.cand[0].via_rule == 1,
        "regular walk -> walked RULE");
    Tst(strstr(r.cand[0].show, "RULE=ed") != NULL, "SHOW names RULE=ed");
    Tst(cnet_te_ask("past tense of like", &r) == 0 &&
            strcmp(r.cand[0].form, "liked") == 0,
        "regular like -> liked");
    Tst(cnet_te_ask("past tense of try", &r) == 0 &&
            strcmp(r.cand[0].form, "tried") == 0,
        "regular try -> tried");
    Tst(cnet_te_ask("past tense of stop", &r) == 0 &&
            strcmp(r.cand[0].form, "stopped") == 0,
        "regular CVC stop -> stopped");
    Tst(cnet_te_ask("past tense of visit", &r) == 0 &&
            strcmp(r.cand[0].form, "visited") == 0,
        "regular visit not visitted");
    Tst(cnet_te_ask("past tense of go", &r) == 0 &&
            strcmp(r.cand[0].form, "went") == 0 && r.cand[0].via_rule == 0,
        "table go still went not goed");
    Tst(cnet_te_ask("past tense of xyzzy", &r) == 0 && r.claimed_cert == 0 &&
            r.cand[0].gap == 1,
        "xyzzy still ABSTAIN (no vowel, not a regular verb)");
    Tst(cnet_te_ask("past tense of walk and xyzzy", &r) == 0 &&
            r.claimed_cert == 0 && r.cand[0].local && r.cand[1].gap,
        "regular+unknown half-cover CERT 0");

    {
        FILE *tf = fopen("/tmp/cnet_te_overlay.tsv", "w");
        if (tf) {
            fputs("fly\tflew\tflown\n", tf);
            fclose(tf);
        }
        Tst(cnet_te_load_tsv("/tmp/cnet_te_overlay.tsv") == 0, "load overlay tsv");
        Tst(cnet_te_ask("past tense of fly", &r) == 0 && r.claimed_cert == 1 &&
                strcmp(r.cand[0].form, "flew") == 0,
            "overlay fly -> flew LOCAL");
    }
    {
        FILE *tf = fopen("/tmp/cnet_te_hf_steal.tsv", "w");
        if (tf) {
            fputs("steal\tstole\tstolen\nfreeze\tfroze\tfrozen\n", tf);
            fclose(tf);
        }
        Tst(cnet_te_load_tsv("/tmp/cnet_te_hf_steal.tsv") == 0, "load hf-shaped tsv");
        Tst(cnet_te_ask("past tense of steal", &r) == 0 && r.claimed_cert == 1 &&
                strcmp(r.cand[0].form, "stole") == 0,
            "hf steal -> stole LOCAL");
        Tst(cnet_te_ask("past participle of freeze", &r) == 0 &&
                r.claimed_cert == 1 && strcmp(r.cand[0].form, "frozen") == 0,
            "hf freeze participle frozen LOCAL");
    }

    /* Gold last → table row, not FAQ. Swim is not a builtin. */
    Tst(cnet_te_ask("past tense of swim", &r) == 0 && r.grammar_hit == 1 &&
            r.cand[0].via_rule == 1,
        "swim without table uses regular (wrong until gold)");
    {
        const char *gp = "/tmp/cnet_te_gold_apply.tsv";
        (void)unlink(gp);
        Tst(cnet_te_gold_apply("who are you", "I am Marble.", gp) == 0,
            "non-morphology gold_apply is a no-op");
        Tst(cnet_te_gold_apply("past tense of go and see", "went", gp) == 0,
            "multi-lemma gold_apply is a no-op");
        Tst(cnet_te_gold_apply("past tense of swim", "ABSTAIN: no", gp) != 0,
            "ABSTAIN gold_apply refused");
        Tst(cnet_te_gold_apply("past tense of swim", "swam", gp) == 1,
            "gold_apply past swim -> swam");
        Tst(cnet_te_ask("past tense of swim", &r) == 0 && r.claimed_cert == 1 &&
                strcmp(r.cand[0].form, "swam") == 0 && cnet_te_audit_ok(&r),
            "after gold overlay swim is LOCAL CERT");
        Tst(cnet_te_ask("past participle of swim", &r) == 0 && r.claimed_cert == 0,
            "past-only gold does not fake participle");
        Tst(cnet_te_gold_apply("past participle of swim", "swum", gp) == 1,
            "gold_apply participle swum");
        Tst(cnet_te_ask("past participle of swim", &r) == 0 && r.claimed_cert == 1 &&
                strcmp(r.cand[0].form, "swum") == 0,
            "participle LOCAL after gold");
        Tst(cnet_te_gold_apply("past tense of go", "goed", gp) == 0,
            "do not overwrite builtin go/went");
        Tst(cnet_te_ask("past tense of go", &r) == 0 &&
                strcmp(r.cand[0].form, "went") == 0,
            "go still went");
        Tst(cnet_te_gold_apply("past tense of ring", "ring -> rang.", gp) == 1,
            "lemma -> form. gold_apply");
        Tst(cnet_te_ask("past tense of ring", &r) == 0 &&
                strcmp(r.cand[0].form, "rang") == 0 && r.claimed_cert == 1,
            "ring -> rang LOCAL");
        {
            FILE *gf = fopen(gp, "r");
            char buf[512];
            size_t n = 0;
            Tst(gf != NULL, "overlay tsv written");
            if (gf) {
                n = fread(buf, 1, sizeof buf - 1, gf);
                buf[n] = 0;
                fclose(gf);
                Tst(strstr(buf, "swim\tswam") != NULL, "tsv has swim swam");
                Tst(strstr(buf, "ring\trang") != NULL, "tsv has ring rang");
            }
        }
    }

    /* Plural operator — RULE=s, not FAQ rows. Irregulars are overlay. */
    Tst(cnet_te_ask("plural of cat", &r) == 0 && r.grammar_hit == 1 &&
            r.claimed_cert == 1 && strcmp(r.cand[0].form, "cats") == 0 &&
            r.cand[0].kind == CNET_TE_PLURAL && r.cand[0].via_rule == 1,
        "plural cat -> cats RULE=s");
    Tst(strstr(r.cand[0].show, "PLURAL") && strstr(r.cand[0].show, "RULE=s"),
        "SHOW names PLURAL RULE=s");
    Tst(cnet_te_ask("plural of box", &r) == 0 &&
            strcmp(r.cand[0].form, "boxes") == 0,
        "plural box -> boxes");
    Tst(cnet_te_ask("plural of city", &r) == 0 &&
            strcmp(r.cand[0].form, "cities") == 0,
        "plural city -> cities");
    Tst(cnet_te_ask("plural of day", &r) == 0 &&
            strcmp(r.cand[0].form, "days") == 0,
        "plural day -> days (vowel+y)");
    Tst(cnet_te_ask("plural of bus", &r) == 0 &&
            strcmp(r.cand[0].form, "buses") == 0,
        "plural bus -> buses");
    Tst(cnet_te_ask("plural of go", &r) == 0 &&
            strcmp(r.cand[0].form, "went") != 0 &&
            strcmp(r.cand[0].form, "gos") == 0,
        "plural does not steal verb past");
    Tst(cnet_te_ask("plural of xyzzy", &r) == 0 && r.claimed_cert == 0 &&
            r.cand[0].gap == 1,
        "plural xyzzy ABSTAIN (no vowel)");
    Tst(cnet_te_ask("plural of cat and xyzzy", &r) == 0 && r.claimed_cert == 0 &&
            r.cand[0].local && r.cand[1].gap,
        "plural half-cover CERT 0");
    Tst(cnet_te_ask("past tense of walk", &r) == 0 &&
            strcmp(r.cand[0].form, "walked") == 0,
        "past tense still RULE=ed after plural grammar");
    {
        const char *gp = "/tmp/cnet_te_plural_gold.tsv";
        (void)unlink(gp);
        Tst(cnet_te_ask("plural of mouse", &r) == 0 &&
                strcmp(r.cand[0].form, "mouses") == 0 && r.cand[0].via_rule == 1,
            "mouse without overlay is regular (wrong until gold)");
        Tst(cnet_te_gold_apply("plural of mouse", "mice", gp) == 1,
            "gold_apply plural mouse -> mice");
        Tst(cnet_te_ask("plural of mouse", &r) == 0 && r.claimed_cert == 1 &&
                strcmp(r.cand[0].form, "mice") == 0 && r.cand[0].via_rule == 0,
            "after gold mouse is TABLE not RULE=s");
        Tst(cnet_te_gold_apply("plural of cat", "cats", gp) == 0,
            "gold of a form the operator already produced is not a FAQ row");
    }

    /* Article operator — vowel-letter, overlay exceptions. Owns bare a-or-an. */
    Tst(cnet_te_ask("a or an cat", &r) == 0 && r.grammar_hit == 1 &&
            r.claimed_cert == 1 && strcmp(r.cand[0].form, "a") == 0 &&
            r.cand[0].kind == CNET_TE_ART && r.cand[0].via_rule == 1,
        "a or an cat -> a RULE=vowel-letter");
    Tst(strstr(r.cand[0].show, "ART") &&
            strstr(r.cand[0].show, "RULE=vowel-letter"),
        "SHOW names ART RULE=vowel-letter");
    Tst(cnet_te_ask("a or an apple", &r) == 0 &&
            strcmp(r.cand[0].form, "an") == 0,
        "a or an apple -> an");
    Tst(cnet_te_ask("a or an", &r) == 0 && r.grammar_hit == 1 &&
            r.claimed_cert == 0,
        "bare a or an is organ gap, not FAQ pack");
    Tst(cnet_te_ask("who are you", &r) == 0 && r.grammar_hit == 0,
        "identity still not morphology");
    {
        const char *gp = "/tmp/cnet_te_art_gold.tsv";
        (void)unlink(gp);
        Tst(cnet_te_ask("a or an hour", &r) == 0 &&
                strcmp(r.cand[0].form, "a") == 0 && r.cand[0].via_rule == 1,
            "hour without overlay is spelling (wrong until gold)");
        Tst(cnet_te_gold_apply("a or an hour", "an", gp) == 1,
            "gold_apply hour -> an");
        Tst(cnet_te_ask("a or an hour", &r) == 0 &&
                strcmp(r.cand[0].form, "an") == 0 && r.cand[0].via_rule == 0,
            "after gold hour is TABLE an");
        Tst(cnet_te_gold_apply("a or an university", "a", gp) == 1,
            "gold_apply university -> a");
        Tst(cnet_te_ask("a or an university", &r) == 0 &&
                strcmp(r.cand[0].form, "a") == 0,
            "university overlay a not an");
    }

    /* Present participle — RULE=ing, not FAQ. Overlay only for true exceptions. */
    Tst(cnet_te_ask("present participle of walk", &r) == 0 && r.grammar_hit == 1 &&
            r.claimed_cert == 1 && strcmp(r.cand[0].form, "walking") == 0 &&
            r.cand[0].kind == CNET_TE_GERUND && r.cand[0].via_rule == 1,
        "ing walk -> walking RULE=ing");
    Tst(strstr(r.cand[0].show, "GERUND") && strstr(r.cand[0].show, "RULE=ing"),
        "SHOW names GERUND RULE=ing");
    Tst(cnet_te_ask("present participle of make", &r) == 0 &&
            strcmp(r.cand[0].form, "making") == 0,
        "ing make -> making (drop e)");
    Tst(cnet_te_ask("present participle of see", &r) == 0 &&
            strcmp(r.cand[0].form, "seeing") == 0,
        "ing see -> seeing (keep ee)");
    Tst(cnet_te_ask("present participle of be", &r) == 0 &&
            strcmp(r.cand[0].form, "being") == 0,
        "ing be -> being (short Ce keeps e)");
    Tst(cnet_te_ask("present participle of die", &r) == 0 &&
            strcmp(r.cand[0].form, "dying") == 0,
        "ing die -> dying (ie->y)");
    Tst(cnet_te_ask("present participle of run", &r) == 0 &&
            strcmp(r.cand[0].form, "running") == 0,
        "ing run -> running CVC");
    Tst(cnet_te_ask("present participle of go", &r) == 0 &&
            strcmp(r.cand[0].form, "going") == 0 && r.cand[0].via_rule == 1,
        "ing go -> going (not went)");
    Tst(cnet_te_ask("present participle of xyzzy", &r) == 0 &&
            r.claimed_cert == 0 && r.cand[0].gap == 1,
        "ing xyzzy ABSTAIN");
    Tst(cnet_te_ask("gerund of walk", &r) == 0 &&
            strcmp(r.cand[0].form, "walking") == 0,
        "gerund of is the same operator");

    /* 3sg — RULE=s verb, overlay have/has be/is. Not FAQ walks. */
    Tst(cnet_te_ask("third person of walk", &r) == 0 && r.grammar_hit == 1 &&
            r.claimed_cert == 1 && strcmp(r.cand[0].form, "walks") == 0 &&
            r.cand[0].kind == CNET_TE_S3 && r.cand[0].via_rule == 1,
        "3sg walk -> walks RULE=s");
    Tst(strstr(r.cand[0].show, "S3") && strstr(r.cand[0].show, "RULE=s"),
        "SHOW names S3 RULE=s");
    Tst(cnet_te_ask("third person of try", &r) == 0 &&
            strcmp(r.cand[0].form, "tries") == 0,
        "3sg try -> tries");
    Tst(cnet_te_ask("third person of watch", &r) == 0 &&
            strcmp(r.cand[0].form, "watches") == 0,
        "3sg watch -> watches");
    Tst(cnet_te_ask("third person of go", &r) == 0 &&
            strcmp(r.cand[0].form, "goes") == 0 && r.cand[0].via_rule == 1,
        "3sg go -> goes (o+es operator, not went)");
    Tst(cnet_te_ask("third person of do", &r) == 0 &&
            strcmp(r.cand[0].form, "does") == 0,
        "3sg do -> does");
    Tst(cnet_te_ask("3sg of walk", &r) == 0 &&
            strcmp(r.cand[0].form, "walks") == 0,
        "3sg of alias");
    Tst(cnet_te_ask("third person of xyzzy", &r) == 0 && r.claimed_cert == 0,
        "3sg xyzzy ABSTAIN");
    Tst(cnet_te_ask("third person of have", &r) == 0 &&
            strcmp(r.cand[0].form, "has") == 0 && r.cand[0].via_rule == 0,
        "3sg have -> has TABLE overlay");
    Tst(cnet_te_ask("third person of be", &r) == 0 &&
            strcmp(r.cand[0].form, "is") == 0 && r.cand[0].via_rule == 0,
        "3sg be -> is TABLE overlay");
    Tst(cnet_te_gold_apply("third person of have", "has", "/tmp/cnet_te_s3_gold.tsv") == 0,
        "gold of overlay have/has is not a FAQ row");
    Tst(cnet_te_gold_apply("third person of walk", "walks", "/tmp/cnet_te_s3_gold.tsv") == 0,
        "gold of a form the operator already produced is not a FAQ row");

    /* Comparative / superlative — RULE=er / RULE=est. Overlay good/better. */
    Tst(cnet_te_ask("comparative of tall", &r) == 0 && r.grammar_hit == 1 &&
            r.claimed_cert == 1 && strcmp(r.cand[0].form, "taller") == 0 &&
            r.cand[0].kind == CNET_TE_COMP && r.cand[0].via_rule == 1,
        "comp tall -> taller RULE=er");
    Tst(strstr(r.cand[0].show, "COMP") && strstr(r.cand[0].show, "RULE=er"),
        "SHOW names COMP RULE=er");
    Tst(cnet_te_ask("comparative of happy", &r) == 0 &&
            strcmp(r.cand[0].form, "happier") == 0,
        "comp happy -> happier");
    Tst(cnet_te_ask("comparative of nice", &r) == 0 &&
            strcmp(r.cand[0].form, "nicer") == 0,
        "comp nice -> nicer");
    Tst(cnet_te_ask("comparative of big", &r) == 0 &&
            strcmp(r.cand[0].form, "bigger") == 0,
        "comp big -> bigger CVC");
    Tst(cnet_te_ask("comparative of good", &r) == 0 &&
            strcmp(r.cand[0].form, "better") == 0 && r.cand[0].via_rule == 0,
        "comp good -> better TABLE");
    Tst(cnet_te_ask("comparative of xyzzy", &r) == 0 && r.claimed_cert == 0,
        "comp xyzzy ABSTAIN");
    Tst(cnet_te_ask("superlative of tall", &r) == 0 &&
            strcmp(r.cand[0].form, "tallest") == 0 && r.cand[0].via_rule == 1,
        "super tall -> tallest RULE=est");
    Tst(cnet_te_ask("superlative of happy", &r) == 0 &&
            strcmp(r.cand[0].form, "happiest") == 0,
        "super happy -> happiest");
    Tst(cnet_te_ask("superlative of good", &r) == 0 &&
            strcmp(r.cand[0].form, "best") == 0 && r.cand[0].via_rule == 0,
        "super good -> best TABLE");
    Tst(cnet_te_gold_apply("comparative of tall", "taller",
                          "/tmp/cnet_te_comp_gold.tsv") == 0,
        "gold of taller the operator already produced is not FAQ");

    /* Adverb RULE=ly. Overlay true/truly good/well. */
    Tst(cnet_te_ask("adverb of quick", &r) == 0 && r.grammar_hit == 1 &&
            r.claimed_cert == 1 && strcmp(r.cand[0].form, "quickly") == 0 &&
            r.cand[0].kind == CNET_TE_ADV && r.cand[0].via_rule == 1,
        "adv quick -> quickly RULE=ly");
    Tst(strstr(r.cand[0].show, "ADV") && strstr(r.cand[0].show, "RULE=ly"),
        "SHOW names ADV RULE=ly");
    Tst(cnet_te_ask("adverb of happy", &r) == 0 &&
            strcmp(r.cand[0].form, "happily") == 0,
        "adv happy -> happily");
    Tst(cnet_te_ask("ly form of quick", &r) == 0 &&
            strcmp(r.cand[0].form, "quickly") == 0,
        "ly form of alias");
    Tst(cnet_te_ask("adverb of true", &r) == 0 &&
            strcmp(r.cand[0].form, "truly") == 0 && r.cand[0].via_rule == 0,
        "adv true -> truly TABLE");
    Tst(cnet_te_ask("adverb of good", &r) == 0 &&
            strcmp(r.cand[0].form, "well") == 0,
        "adv good -> well TABLE");
    Tst(cnet_te_ask("adverb of xyzzy", &r) == 0 && r.claimed_cert == 0,
        "adv xyzzy ABSTAIN");
    Tst(cnet_te_ask("un form of lock", &r) == 0 && r.grammar_hit == 1 &&
            strcmp(r.cand[0].form, "unlock") == 0 && r.cand[0].via_rule == 1,
        "un lock -> unlock RULE=un");
    Tst(cnet_te_ask("re form of play", &r) == 0 &&
            strcmp(r.cand[0].form, "replay") == 0,
        "re play -> replay RULE=re");
    Tst(cnet_te_ask("ness of happy", &r) == 0 &&
            strcmp(r.cand[0].form, "happiness") == 0,
        "ness happy -> happiness RULE=ness");
    Tst(cnet_te_ask("able of read", &r) == 0 &&
            strcmp(r.cand[0].form, "readable") == 0,
        "able read -> readable RULE=able");
    Tst(cnet_te_ask("un form of xyzzy", &r) == 0 && r.claimed_cert == 0,
        "un xyzzy ABSTAIN");

    /* 27B residual harvest: propose irregulars, never CERT, never overlay. */
    {
        char form[CNET_TE_FORM];
        Tst(cnet_te_parse_teacher_form("swam", form, sizeof form) == 0 &&
                strcmp(form, "swam") == 0,
            "teacher form swam");
        Tst(cnet_te_parse_teacher_form("swam.", form, sizeof form) == 0 &&
                strcmp(form, "swam") == 0,
            "teacher form swam.");
        Tst(cnet_te_parse_teacher_form("I think swam is right", form, sizeof form) !=
                0,
            "sentence draft refused");
        Tst(cnet_te_parse_teacher_form("ABSTAIN: no", form, sizeof form) != 0,
            "ABSTAIN draft refused");
        Tst(cnet_te_teacher_consider("go", "went") == 0,
            "TABLE go is not a teacher propose");
        Tst(cnet_te_teacher_consider("walk", "walked") == 0,
            "regular walk/walked is not a FAQ row");
        Tst(cnet_te_teacher_consider("blim", "blam") == 1,
            "regular blimmed vs teacher blam → propose");
        Tst(cnet_te_teacher_consider("blim", "blim") == 0,
            "teacher repeating the lemma is not a past form");
        Tst(cnet_te_teacher_consider("xyzzy", "xyzzyed") == 0,
            "gap lemma is not harvested");
        {
            const char *lp = "/tmp/cnet_te_teacher_lemmas.txt";
            const char *pp = "/tmp/cnet_te_teacher_propose.tsv";
            FILE *lf;
            char buf[256];
            (void)unlink(pp);
            lf = fopen(lp, "w");
            if (lf) {
                fputs("blim\nwalk\ngo\n", lf);
                fclose(lf);
            }
            Tst(cnet_te_teacher_tick(lp, pp, NULL) == 0,
                "tick without ask writes nothing");
            {
                FILE *pf = fopen(pp, "r");
                Tst(pf == NULL, "no propose file without teacher");
                if (pf) fclose(pf);
            }
            Tst(cnet_te_ask("past tense of blim", &r) == 0 &&
                    strcmp(r.cand[0].form, "blimmed") == 0 && r.cand[0].via_rule,
                "blim still RULE=ed after tick (no overlay)");
            (void)buf;
        }
    }

#undef Tst
    printf("\nfailures=%d\n", fail);
    if (fail) {
        printf("TYPED_EN_FAIL\n");
        return 1;
    }
    printf("TYPED_EN_PASS\n");
    return 0;
}

int cnet_te_parse_teacher_form(const char *draft, char *form, size_t cap) {
    return parse_gold_form(draft, form, cap);
}

int cnet_te_teacher_consider(const char *lemma, const char *teacher_form) {
    char q[96];
    CnetTeResult r;
    if (!lemma || !lemma[0] || !teacher_form || !teacher_form[0]) return -1;
    if (snprintf(q, sizeof q, "past tense of %s", lemma) >= (int)sizeof q)
        return -1;
    if (cnet_te_ask(q, &r) != 0 || !r.grammar_hit || r.n_cand < 1) return 0;
    if (r.cand[0].gap || r.cand[0].kind == CNET_TE_ABSTAIN) return 0;
    if (!r.cand[0].via_rule) return 0; /* TABLE already owns it */
    if (strcmp(teacher_form, lemma) == 0) return 0; /* not a past form */
    if (strcmp(r.cand[0].form, teacher_form) == 0) return 0;
    return 1;
}

static int propose_has(const char *path, const char *lemma) {
    FILE *f;
    char line[256];
    if (!path || !lemma) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (tab) *tab = 0;
        if (line[0] && strcmp(line, lemma) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int cnet_te_teacher_tick(const char *lemmas_path, const char *propose_path,
                         CnetTeAskFn ask) {
    FILE *lf;
    char line[128];
    int n = 0;
    if (!lemmas_path || !propose_path) return -1;
    if (!ask) return 0;
    lf = fopen(lemmas_path, "r");
    if (!lf) return -1;
    while (fgets(line, sizeof line, lf)) {
        char *nl, form[CNET_TE_FORM], draft[256], q[160];
        CnetTeResult r;
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        if (propose_has(propose_path, line)) continue;
        if (snprintf(q, sizeof q, "past tense of %s", line) >= (int)sizeof q)
            continue;
        if (cnet_te_ask(q, &r) != 0 || !r.grammar_hit || !r.cand[0].via_rule)
            continue;
        if (snprintf(q, sizeof q,
                     "past tense of %s. Reply with one English word only.",
                     line) >= (int)sizeof q)
            continue;
        draft[0] = 0;
        if (ask(q, draft, sizeof draft) != 0 || !draft[0]) continue;
        if (cnet_te_parse_teacher_form(draft, form, sizeof form) != 0) continue;
        if (cnet_te_teacher_consider(line, form) != 1) continue;
        if (gold_tsv_append(propose_path, line, form, "") != 0) continue;
        n++;
    }
    fclose(lf);
    return n;
}
