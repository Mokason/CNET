#include "cnet_ood_skill.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

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

#define CNET_OOD_NUMERAL_MAX 256
#define CNET_OOD_NUMERAL_W 32

static struct {
    char w[CNET_OOD_NUMERAL_W];
    unsigned long n;
    size_t len;
} g_numerals[CNET_OOD_NUMERAL_MAX];
static int g_n_numerals;

void cnet_ood_numerals_clear(void) { g_n_numerals = 0; }

static int numeral_upsert(const char *word, unsigned long n) {
    size_t i, k, len;
    char low[CNET_OOD_NUMERAL_W];
    if (word == NULL || word[0] == '\0') return -1;
    len = 0;
    for (i = 0; word[i]; ++i) {
        if (!isalpha((unsigned char)word[i])) return -1;
        if (len + 1 >= sizeof low) return -1;
        low[len++] = (char)tolower((unsigned char)word[i]);
    }
    low[len] = '\0';
    for (k = 0; k < (size_t)g_n_numerals; ++k) {
        if (g_numerals[k].len == len && memcmp(g_numerals[k].w, low, len) == 0) {
            g_numerals[k].n = n;
            return 0;
        }
    }
    if (g_n_numerals >= CNET_OOD_NUMERAL_MAX) return -1;
    memcpy(g_numerals[g_n_numerals].w, low, len + 1);
    g_numerals[g_n_numerals].n = n;
    g_numerals[g_n_numerals].len = len;
    g_n_numerals++;
    return 1;
}

int cnet_ood_load_numerals(const char *path) {
    FILE *f;
    char line[128];
    int n = 0;
    if (path == NULL || path[0] == '\0') return -1;
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        char *tab, *end = NULL;
        unsigned long v;
        size_t L = strlen(line);
        while (L > 0 && (line[L - 1] == '\n' || line[L - 1] == '\r'))
            line[--L] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;
        tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        v = strtoul(tab + 1, &end, 10);
        if (end == tab + 1) continue;
        if (numeral_upsert(line, v) >= 0) n++;
    }
    fclose(f);
    return n;
}

int cnet_ood_gold_numeral(const char *query, const char *answer,
                         const char *tsv_path) {
    const char *q, *a;
    char word[CNET_OOD_NUMERAL_W];
    char *end = NULL;
    unsigned long v;
    size_t n = 0;
    FILE *f;
    if (!query || !answer) return 0;
    q = query;
    while (*q && isspace((unsigned char)*q)) q++;
    if (!strncmp(q, "numeral ", 8)) q += 8;
    while (*q && isspace((unsigned char)*q)) q++;
    while (*q && isalpha((unsigned char)*q)) {
        if (n + 1 >= sizeof word) return 0;
        word[n++] = *q++;
    }
    word[n] = '\0';
    while (*q && isspace((unsigned char)*q)) q++;
    if (!n || *q) return 0;
    a = answer;
    while (*a && isspace((unsigned char)*a)) a++;
    if (!isdigit((unsigned char)*a)) return 0;
    v = strtoul(a, &end, 10);
    if (end == a) return 0;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end) return 0;
    if (numeral_upsert(word, v) < 0) return -1;
    if (tsv_path && tsv_path[0]) {
        f = fopen(tsv_path, "a");
        if (!f) return -1;
        fprintf(f, "%s\t%lu\n", word, v);
        fclose(f);
    }
    return 1;
}

static int parse_numword(const char *p, unsigned long *v, size_t *consumed) {
    int i;
    size_t k;
    if (p == NULL || v == NULL || consumed == NULL) return 0;
    for (i = 0; i < g_n_numerals; ++i) {
        int match = 1;
        for (k = 0; k < g_numerals[i].len; ++k) {
            if (tolower((unsigned char)p[k]) != (unsigned char)g_numerals[i].w[k]) {
                match = 0;
                break;
            }
        }
        if (!match) continue;
        if (p[g_numerals[i].len] != '\0' &&
            isalnum((unsigned char)p[g_numerals[i].len]))
            continue;
        *v = g_numerals[i].n;
        *consumed = g_numerals[i].len;
        return 1;
    }
    return 0;
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
        if (isalpha((unsigned char)*p)) {
            unsigned long wv;
            size_t cons = 0;
            if (p > turn && isalnum((unsigned char)p[-1])) {
                ++p;
                continue;
            }
            if (parse_numword(p, &wv, &cons)) {
                if (n >= 2) return 1;
                nums[n++] = wv;
                p += cons;
                continue;
            }
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
            if (*p == '/') {
                found = CNET_OOD_OP_DIV;
                ++p;
                continue;
            }
            if (*p == '%') {
                found = CNET_OOD_OP_MOD;
                ++p;
                continue;
            }
            if (*p == '<' && p[1] == '<') {
                found = CNET_OOD_OP_SHL;
                p += 2;
                continue;
            }
            if (*p == '>' && p[1] == '>') {
                found = CNET_OOD_OP_SHR;
                p += 2;
                continue;
            }
            if (*p == '&') {
                found = CNET_OOD_OP_AND;
                ++p;
                continue;
            }
            if (*p == '|') {
                found = CNET_OOD_OP_OR;
                ++p;
                continue;
            }
            if (*p == '^') {
                found = CNET_OOD_OP_XOR;
                ++p;
                continue;
            }
            if (*p == '<' ) {
                found = CNET_OOD_OP_LT;
                ++p;
                continue;
            }
            if (*p == '>') {
                found = CNET_OOD_OP_GT;
                ++p;
                continue;
            }
            if (*p == '=' && p[1] == '=') {
                found = CNET_OOD_OP_EQ;
                p += 2;
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
        else if (has_word_ci(turn, "divided") || has_word_ci(turn, "divide"))
            found = CNET_OOD_OP_DIV;
        else if (has_word_ci(turn, "mod") || has_word_ci(turn, "modulo") ||
                 has_word_ci(turn, "remainder"))
            found = CNET_OOD_OP_MOD;
        else if (has_word_ci(turn, "min") || has_word_ci(turn, "minimum"))
            found = CNET_OOD_OP_MIN;
        else if (has_word_ci(turn, "max") || has_word_ci(turn, "maximum"))
            found = CNET_OOD_OP_MAX;
        else if (has_word_ci(turn, "less"))
            found = CNET_OOD_OP_LT;
        else if (has_word_ci(turn, "greater"))
            found = CNET_OOD_OP_GT;
        else if (has_word_ci(turn, "equals") || has_word_ci(turn, "equal"))
            found = CNET_OOD_OP_EQ;
        else if (has_word_ci(turn, "xor"))
            found = CNET_OOD_OP_XOR;
        else if (has_word_ci(turn, "and"))
            found = CNET_OOD_OP_AND;
        else if (has_word_ci(turn, "shl"))
            found = CNET_OOD_OP_SHL;
        else if (has_word_ci(turn, "shr"))
            found = CNET_OOD_OP_SHR;
        else if (has_word_ci(turn, "or"))
            found = CNET_OOD_OP_OR;
        else
            return 1;
    }
    *a = nums[0];
    *b = nums[1];
    *op = found;
    return 0;
}

static int word_is_ci(const char *p, const char *w, size_t *consumed) {
    size_t n, k;
    if (p == NULL || w == NULL) return 0;
    n = strlen(w);
    for (k = 0; k < n; ++k) {
        if (p[k] == '\0') return 0;
        if (tolower((unsigned char)p[k]) != (unsigned char)w[k]) return 0;
    }
    if (p[n] != '\0' && isalnum((unsigned char)p[n])) return 0;
    if (consumed) *consumed = n;
    return 1;
}

typedef struct {
    const char *token;
    int op;
} OodOperatorToken;

static const OodOperatorToken symbol_operators[] = {
    {"<<", CNET_OOD_OP_SHL}, {">>", CNET_OOD_OP_SHR},
    {"==", CNET_OOD_OP_EQ},  {"+", CNET_OOD_OP_ADD},
    {"*", CNET_OOD_OP_MUL},  {"/", CNET_OOD_OP_DIV},
    {"%", CNET_OOD_OP_MOD},  {"<", CNET_OOD_OP_LT},
    {">", CNET_OOD_OP_GT},   {"&", CNET_OOD_OP_AND},
    {"|", CNET_OOD_OP_OR},   {"^", CNET_OOD_OP_XOR},
};

static const OodOperatorToken word_operators[] = {
    {"plus", CNET_OOD_OP_ADD},       {"add", CNET_OOD_OP_ADD},
    {"sum", CNET_OOD_OP_ADD},        {"minus", CNET_OOD_OP_SUB},
    {"times", CNET_OOD_OP_MUL},      {"multiplied", CNET_OOD_OP_MUL},
    {"divided", CNET_OOD_OP_DIV},    {"divide", CNET_OOD_OP_DIV},
    {"modulo", CNET_OOD_OP_MOD},     {"remainder", CNET_OOD_OP_MOD},
    {"mod", CNET_OOD_OP_MOD},        {"minimum", CNET_OOD_OP_MIN},
    {"min", CNET_OOD_OP_MIN},        {"maximum", CNET_OOD_OP_MAX},
    {"max", CNET_OOD_OP_MAX},        {"less", CNET_OOD_OP_LT},
    {"greater", CNET_OOD_OP_GT},     {"equals", CNET_OOD_OP_EQ},
    {"equal", CNET_OOD_OP_EQ},       {"xor", CNET_OOD_OP_XOR},
    {"and", CNET_OOD_OP_AND},        {"or", CNET_OOD_OP_OR},
    {"shl", CNET_OOD_OP_SHL},        {"shr", CNET_OOD_OP_SHR},
};

static int op_at(const char *p, size_t *consumed) {
    size_t i;
    if (p == NULL) return 0;
    for (i = 0; i < sizeof symbol_operators / sizeof symbol_operators[0]; i++) {
        size_t length = strlen(symbol_operators[i].token);
        if (strncmp(p, symbol_operators[i].token, length) == 0) {
            if (consumed) *consumed = length;
            return symbol_operators[i].op;
        }
    }
    if (*p == '-' &&
        (p[1] == '\0' || p[1] == ' ' || isdigit((unsigned char)p[1]))) {
        if (consumed) *consumed = 1;
        return CNET_OOD_OP_SUB;
    }
    for (i = 0; i < sizeof word_operators / sizeof word_operators[0]; i++) {
        size_t length = 0;
        if (word_is_ci(p, word_operators[i].token, &length)) {
            if (consumed) *consumed = length;
            return word_operators[i].op;
        }
    }
    return 0;
}

int cnet_ood_arith_shaped(const char *turn, int *op) {
    const char *p;
    int n = 0, found = 0;
    if (op) *op = 0;
    if (turn == NULL) return 0;
    for (p = turn; *p != '\0';) {
        size_t cons = 0;
        int o;
        char tok[CNET_OOD_NUMERAL_W];
        size_t tlen = 0;
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '*' && *p != '-' &&
            *p != '/' && *p != '%' && *p != '<' && *p != '>' && *p != '=') {
            ++p;
            continue;
        }
        o = op_at(p, &cons);
        if (o) {
            if (n == 1 && found == 0) found = o;
            p += cons ? cons : 1;
            continue;
        }
        if (isdigit((unsigned char)*p)) {
            if (p > turn && isalnum((unsigned char)p[-1])) {
                ++p;
                continue;
            }
            while (isdigit((unsigned char)*p)) ++p;
            if (n >= 2) return 0;
            n++;
            continue;
        }
        if (!isalpha((unsigned char)*p)) {
            ++p;
            continue;
        }
        if (p > turn && isalnum((unsigned char)p[-1])) {
            ++p;
            continue;
        }
        while (p[tlen] != '\0' && isalnum((unsigned char)p[tlen]) &&
               tlen + 1 < sizeof tok) {
            tok[tlen] = (char)tolower((unsigned char)p[tlen]);
            tlen++;
        }
        tok[tlen] = '\0';
        p += tlen;
        if (tlen == 0) {
            ++p;
            continue;
        }
        if (is_stop(tok)) continue;
        if (n >= 2) return 0;
        n++;
    }
    if (n != 2) return 0;
    if (found == 0) {
        if (has_word_ci(turn, "plus") || has_word_ci(turn, "add") ||
            has_word_ci(turn, "sum"))
            found = CNET_OOD_OP_ADD;
        else if (has_word_ci(turn, "minus"))
            found = CNET_OOD_OP_SUB;
        else if (has_word_ci(turn, "times") || has_word_ci(turn, "multiplied"))
            found = CNET_OOD_OP_MUL;
        else if (has_word_ci(turn, "divided") || has_word_ci(turn, "divide"))
            found = CNET_OOD_OP_DIV;
        else if (has_word_ci(turn, "mod") || has_word_ci(turn, "modulo") ||
                 has_word_ci(turn, "remainder"))
            found = CNET_OOD_OP_MOD;
        else if (has_word_ci(turn, "min") || has_word_ci(turn, "minimum"))
            found = CNET_OOD_OP_MIN;
        else if (has_word_ci(turn, "max") || has_word_ci(turn, "maximum"))
            found = CNET_OOD_OP_MAX;
        else if (has_word_ci(turn, "less"))
            found = CNET_OOD_OP_LT;
        else if (has_word_ci(turn, "greater"))
            found = CNET_OOD_OP_GT;
        else if (has_word_ci(turn, "equals") || has_word_ci(turn, "equal"))
            found = CNET_OOD_OP_EQ;
        else
            return 0;
    }
    if (op) *op = found;
    return 1;
}

void cnet_ood_numerals_reload(void) {
    char p[1024];
    const char *pk = getenv("CNET_PACKS_ROOT");
    const char *min = getenv("CNET_MINIMAL_ROOT");
    (void)cnet_ood_load_numerals("config/en_numerals.tsv");
    if (pk && pk[0]) {
        int n = snprintf(p, sizeof p, "%s/en_numerals.tsv", pk);
        if (n > 0 && (size_t)n < sizeof p) (void)cnet_ood_load_numerals(p);
    }
    if (min && min[0]) {
        int n = snprintf(p, sizeof p, "%s/config/en_numerals.tsv", min);
        if (n > 0 && (size_t)n < sizeof p) (void)cnet_ood_load_numerals(p);
    }
}

int cnet_ood_gap_unknown(const char *turn, char *word, size_t cap) {
    const char *p;
    int dummy = 0;
    if (word && cap) word[0] = '\0';
    if (turn == NULL || word == NULL || cap == 0) return -1;
    if (!cnet_ood_arith_shaped(turn, &dummy)) return 1;
    for (p = turn; *p != '\0';) {
        size_t cons = 0, tlen = 0;
        unsigned long wv = 0;
        char tok[CNET_OOD_NUMERAL_W];
        int o;
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '*' && *p != '-' &&
            *p != '/' && *p != '%' && *p != '<' && *p != '>' && *p != '=') {
            ++p;
            continue;
        }
        o = op_at(p, &cons);
        if (o) {
            p += cons ? cons : 1;
            continue;
        }
        if (isdigit((unsigned char)*p)) {
            if (p > turn && isalnum((unsigned char)p[-1])) {
                ++p;
                continue;
            }
            while (isdigit((unsigned char)*p)) ++p;
            continue;
        }
        if (!isalpha((unsigned char)*p)) {
            ++p;
            continue;
        }
        if (p > turn && isalnum((unsigned char)p[-1])) {
            ++p;
            continue;
        }
        if (parse_numword(p, &wv, &cons)) {
            p += cons;
            continue;
        }
        while (p[tlen] != '\0' && isalnum((unsigned char)p[tlen]) &&
               tlen + 1 < sizeof tok) {
            tok[tlen] = (char)tolower((unsigned char)p[tlen]);
            tlen++;
        }
        tok[tlen] = '\0';
        p += tlen ? tlen : 1;
        if (tlen == 0 || is_stop(tok)) continue;
        if (tlen + 1 > cap) return -1;
        memcpy(word, tok, tlen + 1);
        return 0;
    }
    return 1;
}

static int json_query_field(const char *line, char *out, size_t cap) {
    const char *p = strstr(line, "\"query\"");
    size_t o = 0;
    if (!p) p = strstr(line, "\"q\"");
    if (!p || !out || !cap) return -1;
    p = strchr(p, ':');
    if (!p) return -1;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    while (*p && *p != '"' && o + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[o++] = *p++;
    }
    out[o] = '\0';
    return o > 0 ? 0 : 1;
}

static int propose_has_word(const char *path, const char *word) {
    FILE *f;
    char line[128];
    size_t n;
    if (!path || !word || !word[0]) return 0;
    f = fopen(path, "r");
    if (!f) return 0;
    n = strlen(word);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, word, n) &&
            (line[n] == '\t' || line[n] == '\n' || line[n] == '\r' ||
             line[n] == '\0')) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int cnet_ood_numeral_tick(const char *miss_log, const char *propose_path,
                         const char *overlay_path) {
    FILE *f;
    char line[4096], q[256], unk[CNET_OOD_NUMERAL_W];
    int nnew = 0;
    if (!miss_log || !miss_log[0] || !propose_path || !propose_path[0]) return -1;
    cnet_ood_numerals_reload();
    if (overlay_path && overlay_path[0]) (void)cnet_ood_load_numerals(overlay_path);
    f = fopen(miss_log, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        FILE *pf;
        if (strstr(line, "\"shortcircuit\":true")) continue;
        if (json_query_field(line, q, sizeof q) != 0) continue;
        if (cnet_ood_gap_unknown(q, unk, sizeof unk) != 0) continue;
        if (propose_has_word(propose_path, unk)) continue;
        pf = fopen(propose_path, "a");
        if (!pf) {
            fclose(f);
            return -1;
        }
        fprintf(pf, "%s\t\n", unk);
        fclose(pf);
        nnew++;
    }
    fclose(f);
    return nnew;
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

static int commit_exact_text(CnetSkillLaneResult *out, const char *skill,
                             const char *value) {
    if (out == NULL || skill == NULL || value == NULL) return -1;
    memset(out, 0, sizeof *out);
    copy_text(out->skill, sizeof out->skill, skill);
    copy_text(out->value, sizeof out->value, value);
    copy_text(out->spoken, sizeof out->spoken, value);
    out->bound = 1;
    out->kind = CNET_SKILL_LANE_EXACT;
    out->claimed_cert = 1;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    return 0;
}

static int commit_compute(CnetSkillLaneResult *out, const char *skill,
                          unsigned long value) {
    char formatted[32];
    snprintf(formatted, sizeof formatted, "%lu", value);
    return commit_exact_text(out, skill, formatted);
}

static int commit_abstain(CnetSkillLaneResult *out, const char *skill) {
    if (out == NULL || skill == NULL) return -1;
    memset(out, 0, sizeof *out);
    copy_text(out->skill, sizeof out->skill, skill);
    copy_text(out->value, sizeof out->value, "ABSTAIN");
    copy_text(out->spoken, sizeof out->spoken,
              "ABSTAIN: no sealed numeral. Not CERT.");
    copy_text(out->refusal, sizeof out->refusal, "numeral_gap");
    out->bound = 0;
    out->kind = CNET_SKILL_LANE_ABSTAIN;
    out->claimed_cert = 0;
    out->residual_calls = 0;
    out->teacher_calls = 0;
    return 0;
}

static void trim_whole(const char *in, char *out, size_t cap) {
    size_t n;
    if (!out || !cap) return;
    out[0] = '\0';
    if (!in) return;
    while (*in == ' ' || *in == '\t') in++;
    copy_text(out, cap, in);
    n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '?' || out[n - 1] == '!' ||
                     out[n - 1] == '.' || out[n - 1] == '\n'))
        out[--n] = '\0';
}

static int whole_ci(const char *turn, const char *want) {
    char t[256];
    size_t i;
    trim_whole(turn, t, sizeof t);
    if (!want) return 0;
    for (i = 0; want[i]; i++) {
        if (tolower((unsigned char)t[i]) != tolower((unsigned char)want[i]))
            return 0;
    }
    return t[i] == '\0';
}

static int apply_binop(int op, unsigned long a, unsigned long b, unsigned long *out) {
    if (!out) return -1;
    switch (op) {
        case CNET_OOD_OP_ADD:
            if (a > 4294967295UL - b) return -1;
            *out = a + b;
            return 0;
        case CNET_OOD_OP_SUB:
            if (b > a) return -1;
            *out = a - b;
            return 0;
        case CNET_OOD_OP_MUL:
            if (a != 0ul && b > 4294967295UL / a) return -1;
            *out = a * b;
            return 0;
        case CNET_OOD_OP_DIV:
            if (b == 0ul) return 1;
            *out = a / b;
            return 0;
        case CNET_OOD_OP_MOD:
            if (b == 0ul) return 1;
            *out = a % b;
            return 0;
        case CNET_OOD_OP_LT:
            *out = a < b ? 1ul : 0ul;
            return 0;
        case CNET_OOD_OP_GT:
            *out = a > b ? 1ul : 0ul;
            return 0;
        case CNET_OOD_OP_EQ:
            *out = a == b ? 1ul : 0ul;
            return 0;
        case CNET_OOD_OP_MIN:
            *out = a < b ? a : b;
            return 0;
        case CNET_OOD_OP_MAX:
            *out = a > b ? a : b;
            return 0;
        case CNET_OOD_OP_AND:
            *out = a & b;
            return 0;
        case CNET_OOD_OP_OR:
            *out = a | b;
            return 0;
        case CNET_OOD_OP_XOR:
            *out = a ^ b;
            return 0;
        case CNET_OOD_OP_SHL:
            if (b > 31ul) return 1;
            *out = (a << b) & 4294967295UL;
            return 0;
        case CNET_OOD_OP_SHR:
            if (b > 31ul) return 1;
            *out = a >> b;
            return 0;
        default:
            return -1;
    }
}

static int parse_ymd(const char *s, struct tm *tm) {
    int y = 0, m = 0, d = 0;
    if (!s || !tm) return 1;
    if (sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return 1;
    if (y < 1970 || m < 1 || m > 12 || d < 1 || d > 31) return 1;
    memset(tm, 0, sizeof *tm);
    tm->tm_year = y - 1900;
    tm->tm_mon = m - 1;
    tm->tm_mday = d;
    tm->tm_isdst = -1;
    if (mktime(tm) == (time_t)-1) return 1;
    return 0;
}

static int try_date(const char *turn, CnetSkillLaneResult *out) {
    char buf[80];
    struct tm tm;
    time_t t;
    const char *p;
    if (!turn) return 1;
    if (whole_ci(turn, "what day is it") || whole_ci(turn, "day of week") ||
        whole_ci(turn, "what's the date") || whole_ci(turn, "what date is it") ||
        whole_ci(turn, "whats the date")) {
        t = time(NULL);
        if (!localtime_r(&t, &tm)) return 1;
        strftime(buf, sizeof buf, "%A %Y-%m-%d", &tm);
        return commit_exact_text(out, CNET_OOD_DATE, buf);
    }
    if (strncmp(turn, "day of week ", 12) == 0 && parse_ymd(turn + 12, &tm) == 0) {
        strftime(buf, sizeof buf, "%A", &tm);
        return commit_exact_text(out, CNET_OOD_DATE, buf);
    }
    p = strstr(turn, "days between ");
    if (p) {
        struct tm a, b;
        time_t ta, tb;
        long diff;
        p += 13;
        while (*p == ' ') p++;
        if (parse_ymd(p, &a) != 0) return 1;
        while (*p && *p != ' ') p++;
        while (*p == ' ') p++;
        if (parse_ymd(p, &b) != 0) return 1;
        ta = mktime(&a);
        tb = mktime(&b);
        if (ta == (time_t)-1 || tb == (time_t)-1) return 1;
        diff = (long)((tb - ta) / 86400);
        if (diff < 0) diff = -diff;
        return commit_compute(out, CNET_OOD_DATE, (unsigned long)diff);
    }
    p = strstr(turn, "add ");
    if (p && strstr(turn, "day")) {
        unsigned long n = 0;
        struct tm base;
        char *end = NULL;
        p += 4;
        n = strtoul(p, &end, 10);
        p = strstr(turn, "to ");
        if (!(p && parse_ymd(p + 3, &base) == 0)) {
            time_t now = time(NULL);
            if (!localtime_r(&now, &base)) return 1;
        }
        base.tm_mday += (int)n;
        t = mktime(&base);
        if (t == (time_t)-1) return 1;
        if (!localtime_r(&t, &base)) return 1;
        strftime(buf, sizeof buf, "%Y-%m-%d", &base);
        return commit_exact_text(out, CNET_OOD_DATE, buf);
    }
    return 1;
}

static int try_clock(const char *turn, CnetSkillLaneResult *out) {
    time_t now;
    struct tm tm;
    char buf[64];
    if (!whole_ci(turn, "now") && !whole_ci(turn, "what time is it") &&
        !whole_ci(turn, "what's the time") && !whole_ci(turn, "whats the time") &&
        !whole_ci(turn, "what is the time") && !whole_ci(turn, "current time") &&
        !whole_ci(turn, "time now"))
        return 1;
    now = time(NULL);
    if (!localtime_r(&now, &tm)) return 1;
    if (!strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M%z", &tm) || !buf[0]) return 1;
    return commit_exact_text(out, CNET_OOD_CLOCK, buf);
}

static int try_host(const char *turn, CnetSkillLaneResult *out) {
    char buf[128];
    buf[0] = '\0';
    if (whole_ci(turn, "load average") || whole_ci(turn, "host load") ||
        whole_ci(turn, "what's the load") || whole_ci(turn, "whats the load")) {
        FILE *f = fopen("/proc/loadavg", "r");
        double a = 0, b = 0, c = 0;
        if (!f || fscanf(f, "%lf %lf %lf", &a, &b, &c) < 1) {
            if (f) fclose(f);
            return 1;
        }
        fclose(f);
        snprintf(buf, sizeof buf, "%.2f %.2f %.2f", a, b, c);
        return commit_exact_text(out, CNET_OOD_HOST_LOAD, buf);
    }
    if (whole_ci(turn, "uptime") || whole_ci(turn, "host uptime") ||
        whole_ci(turn, "what's the uptime") || whole_ci(turn, "whats the uptime")) {
        FILE *f = fopen("/proc/uptime", "r");
        double sec = 0;
        unsigned long s, h, m;
        if (!f || fscanf(f, "%lf", &sec) != 1) {
            if (f) fclose(f);
            return 1;
        }
        fclose(f);
        s = (unsigned long)sec;
        h = s / 3600ul;
        m = (s % 3600ul) / 60ul;
        snprintf(buf, sizeof buf, "%luh %lum", h, m);
        return commit_exact_text(out, CNET_OOD_HOST_UP, buf);
    }
    if (whole_ci(turn, "disk free") || whole_ci(turn, "disk space") ||
        whole_ci(turn, "how much disk")) {
        struct statvfs st;
        unsigned long long avail;
        if (statvfs("/", &st) != 0) return 1;
        avail = ((unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize) /
                (1024ull * 1024ull);
        snprintf(buf, sizeof buf, "%llu MiB free", avail);
        return commit_exact_text(out, CNET_OOD_HOST_DISK, buf);
    }
    if (whole_ci(turn, "mem free") || whole_ci(turn, "memory") ||
        whole_ci(turn, "host mem") || whole_ci(turn, "how much ram")) {
        FILE *f = fopen("/proc/meminfo", "r");
        char line[128];
        unsigned long kb = 0;
        if (!f) return 1;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemAvailable: %lu", &kb) == 1) break;
        }
        fclose(f);
        if (!kb) return 1;
        snprintf(buf, sizeof buf, "%lu MiB avail", kb / 1024ul);
        return commit_exact_text(out, CNET_OOD_HOST_MEM, buf);
    }
    if (whole_ci(turn, "gpu") || whole_ci(turn, "host gpu") ||
        whole_ci(turn, "gpu status")) {
        FILE *f = fopen("/sys/class/drm/card0/device/vendor", "r");
        char id[32];
        if (!f) return commit_abstain(out, CNET_OOD_HOST_GPU);
        if (!fgets(id, sizeof id, f)) {
            fclose(f);
            return commit_abstain(out, CNET_OOD_HOST_GPU);
        }
        fclose(f);
        {
            char *nl = strchr(id, '\n');
            if (nl) *nl = 0;
        }
        snprintf(buf, sizeof buf, "drm0 vendor %s", id);
        return commit_exact_text(out, CNET_OOD_HOST_GPU, buf);
    }
    if (whole_ci(turn, "temp") || whole_ci(turn, "temperature") ||
        whole_ci(turn, "host temp") || whole_ci(turn, "cpu temp")) {
        FILE *f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
        long milli = 0;
        if (!f || fscanf(f, "%ld", &milli) != 1) {
            if (f) fclose(f);
            return commit_abstain(out, CNET_OOD_HOST_TEMP);
        }
        fclose(f);
        snprintf(buf, sizeof buf, "%.1f C", milli / 1000.0);
        return commit_exact_text(out, CNET_OOD_HOST_TEMP, buf);
    }
    return 1;
}

static int try_clamp(const char *turn, CnetSkillLaneResult *out) {
    const char *p;
    unsigned long nums[3];
    int n = 0;
    unsigned long x, lo, hi, r;
    if (!turn || !has_word_ci(turn, "clamp")) return 1;
    for (p = turn; *p && n < 3;) {
        if (isdigit((unsigned char)*p)) {
            char *end = NULL;
            nums[n++] = strtoul(p, &end, 10);
            p = end ? end : p + 1;
            continue;
        }
        p++;
    }
    if (n != 3) return 1;
    x = nums[0];
    lo = nums[1];
    hi = nums[2];
    if (lo > hi) return commit_abstain(out, CNET_OOD_CLAMP);
    r = x < lo ? lo : (x > hi ? hi : x);
    return commit_compute(out, CNET_OOD_CLAMP, r);
}

static int try_compose(const char *turn, CnetSkillLaneResult *out) {
    const char *p;
    char *end;
    int op1 = 0, op2 = 0;
    unsigned long a = 0, b = 0, c = 0, mid, fin;
    size_t n;
    int rc;
    int named;
    if (!turn) return 1;
    p = turn;
    while (*p == ' ') p++;
    named = word_is_ci(p, "compose", &n);
    if (named) {
        p += n;
        while (*p == ' ') p++;
        op1 = op_at(p, &n);
        if (!op1) return 1;
        p += n;
        while (*p == ' ') p++;
        if (!isdigit((unsigned char)*p)) return 1;
        a = strtoul(p, &end, 10);
        p = end;
        while (*p == ' ') p++;
        if (!isdigit((unsigned char)*p)) return 1;
        b = strtoul(p, &end, 10);
        p = end;
    } else {
        /* 3 plus 5 then min 4 — no word "compose" */
        if (!has_word_ci(turn, "then")) return 1;
        if (!isdigit((unsigned char)*p)) return 1;
        a = strtoul(p, &end, 10);
        p = end;
        while (*p == ' ') p++;
        op1 = op_at(p, &n);
        if (!op1) return 1;
        p += n;
        while (*p == ' ') p++;
        if (!isdigit((unsigned char)*p)) return 1;
        b = strtoul(p, &end, 10);
        p = end;
    }
    while (*p == ' ') p++;
    if (word_is_ci(p, "then", &n)) {
        p += n;
        while (*p == ' ') p++;
    }
    op2 = op_at(p, &n);
    if (!op2) return 1;
    p += n;
    while (*p == ' ') p++;
    if (!isdigit((unsigned char)*p)) return 1;
    c = strtoul(p, &end, 10);
    (void)end;
    rc = apply_binop(op1, a, b, &mid);
    if (rc != 0)
        return rc == 1 ? commit_abstain(out, CNET_OOD_COMPOSE) : 1;
    rc = apply_binop(op2, mid, c, &fin);
    if (rc != 0)
        return rc == 1 ? commit_abstain(out, CNET_OOD_COMPOSE) : 1;
    p = end;
    while (*p == ' ') p++;
    if (word_is_ci(p, "then", &n)) {
        int op3;
        unsigned long d, fin2;
        p += n;
        while (*p == ' ') p++;
        op3 = op_at(p, &n);
        if (!op3) return commit_compute(out, CNET_OOD_COMPOSE, fin);
        p += n;
        while (*p == ' ') p++;
        if (!isdigit((unsigned char)*p))
            return commit_compute(out, CNET_OOD_COMPOSE, fin);
        d = strtoul(p, &end, 10);
        rc = apply_binop(op3, fin, d, &fin2);
        if (rc != 0)
            return rc == 1 ? commit_abstain(out, CNET_OOD_COMPOSE) : 1;
        fin = fin2;
    }
    return commit_compute(out, CNET_OOD_COMPOSE, fin);
}

static unsigned crc8_atm_byte(unsigned byte) {
    unsigned crc = byte & 255u;
    int bit;
    for (bit = 0; bit < 8; ++bit)
        crc = (crc & 0x80u) ? ((crc << 1) ^ 0x07u) & 255u : (crc << 1) & 255u;
    return crc;
}

static int try_minutes(const char *turn, CnetSkillLaneResult *out) {
    const char *p;
    char *end;
    unsigned long m;
    if (!turn || !has_word_ci(turn, "minutes")) return 1;
    if (!has_word_ci(turn, "seconds") && !has_word_ci(turn, "second")) return 1;
    p = turn;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return 1;
    m = strtoul(p, &end, 10);
    if (m > 1000000UL) return commit_abstain(out, CNET_OOD_MINUTES);
    return commit_compute(out, CNET_OOD_MINUTES, m * 60UL);
}

static int try_crc8(const char *turn, CnetSkillLaneResult *out) {
    const char *p;
    char *end;
    unsigned long x;
    if (!turn) return 1;
    if (!has_word_ci(turn, "crc8") && !has_word_ci(turn, "crc8_atm")) return 1;
    p = turn;
    while (*p && !isdigit((unsigned char)*p)) p++;
    if (!isdigit((unsigned char)*p)) return 1;
    x = strtoul(p, &end, 10);
    if (x > 255UL) return commit_abstain(out, CNET_OOD_CRC8);
    return commit_compute(out, CNET_OOD_CRC8, (unsigned long)crc8_atm_byte((unsigned)x));
}

int cnet_ood_try_add(const char *turn, CnetSkillLaneResult *out) {
    unsigned long a, b, result;
    int op;
    const char *skill;
    if (out == NULL) return -1;
    memset(out, 0, sizeof *out);
    if (try_clock(turn, out) == 0) return 0;
    if (try_date(turn, out) == 0) return 0;
    if (try_host(turn, out) == 0) return 0;
    if (try_clamp(turn, out) == 0) return 0;
    if (try_minutes(turn, out) == 0) return 0;
    if (try_crc8(turn, out) == 0) return 0;
    if (try_compose(turn, out) == 0) return 0;
    if (cnet_ood_parse_arith(turn, &a, &b, &op) != 0) {
        int sop = 0;
        if (!cnet_ood_arith_shaped(turn, &sop)) return 1;
        skill = (sop == CNET_OOD_OP_SUB)   ? CNET_OOD_SUB
                : (sop == CNET_OOD_OP_MUL) ? CNET_OOD_MUL
                : (sop == CNET_OOD_OP_DIV) ? CNET_OOD_DIV
                : (sop == CNET_OOD_OP_MOD) ? CNET_OOD_MOD
                : (sop == CNET_OOD_OP_MIN) ? CNET_OOD_MIN
                : (sop == CNET_OOD_OP_MAX) ? CNET_OOD_MAX
                : (sop == CNET_OOD_OP_LT || sop == CNET_OOD_OP_GT ||
                   sop == CNET_OOD_OP_EQ)
                    ? CNET_OOD_CMP
                    : CNET_OOD_ADD;
        return commit_abstain(out, skill);
    }
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
    } else if (op == CNET_OOD_OP_DIV) {
        skill = CNET_OOD_DIV;
        if (b == 0ul) return commit_abstain(out, skill);
        result = a / b;
    } else if (op == CNET_OOD_OP_MOD) {
        skill = CNET_OOD_MOD;
        if (b == 0ul) return commit_abstain(out, skill);
        result = a % b;
    } else if (op == CNET_OOD_OP_LT) {
        result = a < b ? 1ul : 0ul;
        skill = CNET_OOD_CMP;
    } else if (op == CNET_OOD_OP_GT) {
        result = a > b ? 1ul : 0ul;
        skill = CNET_OOD_CMP;
    } else if (op == CNET_OOD_OP_EQ) {
        result = a == b ? 1ul : 0ul;
        skill = CNET_OOD_CMP;
    } else if (op == CNET_OOD_OP_MIN) {
        result = a < b ? a : b;
        skill = CNET_OOD_MIN;
    } else if (op == CNET_OOD_OP_MAX) {
        result = a > b ? a : b;
        skill = CNET_OOD_MAX;
    } else if (op == CNET_OOD_OP_AND) {
        result = a & b;
        skill = CNET_OOD_AND;
    } else if (op == CNET_OOD_OP_OR) {
        result = a | b;
        skill = CNET_OOD_OR;
    } else if (op == CNET_OOD_OP_XOR) {
        result = a ^ b;
        skill = CNET_OOD_XOR;
    } else if (op == CNET_OOD_OP_SHL) {
        if (b > 31ul) return commit_abstain(out, CNET_OOD_SHL);
        result = (a << b) & 4294967295UL;
        skill = CNET_OOD_SHL;
    } else if (op == CNET_OOD_OP_SHR) {
        if (b > 31ul) return commit_abstain(out, CNET_OOD_SHR);
        result = a >> b;
        skill = CNET_OOD_SHR;
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
