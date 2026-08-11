/* Domain route table — CERT-first, fail-closed, no malloc on match. */
#include "../include/cnet_domain_route.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- compiled-in defaults (static binary table) ------------------------ */
static const CnetDomainRule k_static_rules[] = {
    /* CERT — product path */
    {"who are you", CNET_ROUTE_CERT, "pack_soul_marble", "", 0, 1},
    {"never self-cert", CNET_ROUTE_CERT, "pack_roe_self", "", 0, 1},
    {"self-cert", CNET_ROUTE_CERT, "pack_roe_self", "", 0, 1},
    {"format-truncation", CNET_ROUTE_CERT, "pack_coding_cnet_c", "", 0, 1},
    {"use tools not guess", CNET_ROUTE_CERT, "pack_toolcall_hermes", "", 0, 1},
    {"git push", CNET_ROUTE_CERT, "pack_toolcall_hermes", "", 0, 1},
    {"roe second brain", CNET_ROUTE_CERT, "pack_roe_self", "", 0, 1},
    {"outside coverage", CNET_ROUTE_CERT, "pack_roe_self", "", 0, 1},
    {"microsplit goal", CNET_ROUTE_CERT, "pack_goal_split", "", 0, 1},
    {"have miss", CNET_ROUTE_CERT, "pack_goal_split", "", 0, 1},
    /* MTK — residual specialty only when no CERT rule hit (examples) */
    {"bitnet ternary specialty", CNET_ROUTE_MTK, "", "skills/mtk/bitnet_demo.tskill", 0, 1},
    {"mtk residual adapter", CNET_ROUTE_MTK, "", "skills/mtk/residual_adapter.tskill", 0, 1},
    /* BASE GGUF — explicit open residual without cartridge */
    {"open residual chat", CNET_ROUTE_BASE_GGUF, "", "", 0, 1},
    {"base gguf host", CNET_ROUTE_BASE_GGUF, "", "", 0, 1},
};

static const int k_n_static =
    (int)(sizeof k_static_rules / sizeof k_static_rules[0]);

const char *cnet_route_kind_name(CnetRouteKind k) {
    switch (k) {
    case CNET_ROUTE_CERT: return "CERT";
    case CNET_ROUTE_MTK: return "MTK";
    case CNET_ROUTE_BASE_GGUF: return "BASE_GGUF";
    case CNET_ROUTE_ABSTAIN: return "ABSTAIN";
    default: return "NONE";
    }
}

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

static void tolower_copy(char *d, size_t cap, const char *s) {
    size_t i;
    if (!d || !cap) return;
    for (i = 0; s && s[i] && i + 1 < cap; i++)
        d[i] = (char)tolower((unsigned char)s[i]);
    d[i < cap ? i : cap - 1] = 0;
}

static int is_word_char(unsigned char c) {
    return c == '_' || isalnum(c);
}

/* Policy C: word-boundary / token-ish match, no heap. */
static int word_boundary_match_ci(const char *query, const char *pat) {
    char h[512], n[CNET_DR_PAT];
    size_t qlen, plen, i;
    if (!query || !pat || !pat[0]) return 0;
    tolower_copy(h, sizeof h, query);
    tolower_copy(n, sizeof n, pat);
    qlen = strlen(h);
    plen = strlen(n);
    if (plen == 0 || plen > qlen) return 0;
    for (i = 0; i + plen <= qlen; i++) {
        if (memcmp(h + i, n, plen) != 0) continue;
        {
            int left_ok = (i == 0) || !is_word_char((unsigned char)h[i - 1]);
            int right_ok =
                (i + plen == qlen) || !is_word_char((unsigned char)h[i + plen]);
            if (left_ok && right_ok) return 1;
        }
    }
    return 0;
}

static int rule_matches(const CnetDomainRule *r, const char *query) {
    int plen;
    if (!r || !query || !r->pattern[0]) return 0;
    plen = (int)strlen(r->pattern);
    {
        int floor = r->min_pat_len > 0 ? r->min_pat_len : CNET_DR_DEFAULT_MIN_PAT;
        if (plen < floor) return 0; /* Policy B */
    }
    return word_boundary_match_ci(query, r->pattern);
}

void cnet_domain_route_init(CnetDomainRouter *R) {
    int i;
    if (!R) return;
    memset(R, 0, sizeof *R);
    R->n_static = k_n_static;
    if (R->n_static > CNET_DR_MAX_RULES) R->n_static = CNET_DR_MAX_RULES;
    for (i = 0; i < R->n_static; i++) {
        R->rules[i] = k_static_rules[i];
        R->rules[i].active = 1;
        if (R->rules[i].min_pat_len <= 0)
            R->rules[i].min_pat_len = CNET_DR_DEFAULT_MIN_PAT;
    }
    R->n_rules = R->n_static;
    scopy(R->source, sizeof R->source, "static");
}

static CnetRouteKind parse_kind(const char *s) {
    char t[32];
    int i;
    if (!s) return CNET_ROUTE_NONE;
    for (i = 0; s[i] && i + 1 < (int)sizeof t; i++)
        t[i] = (char)tolower((unsigned char)s[i]);
    t[i] = 0;
    if (strcmp(t, "cert") == 0 || strcmp(t, "pack") == 0) return CNET_ROUTE_CERT;
    if (strcmp(t, "mtk") == 0 || strcmp(t, "tskill") == 0) return CNET_ROUTE_MTK;
    if (strcmp(t, "base") == 0 || strcmp(t, "gguf") == 0 ||
        strcmp(t, "base_gguf") == 0)
        return CNET_ROUTE_BASE_GGUF;
    if (strcmp(t, "abstain") == 0) return CNET_ROUTE_ABSTAIN;
    return CNET_ROUTE_NONE;
}

/* Line formats (no JSON dependency):
 *   kind | pattern | pack_or_path
 *   CERT\twho are you\tpack_soul_marble
 *   MTK\tbitnet specialty\tskills/mtk/x.tskill
 *   BASE\topen residual\t-
 *   # comment
 */
int cnet_domain_route_load_file(CnetDomainRouter *R, const char *path) {
    FILE *f;
    char line[512];
    int added = 0;
    if (!R || !path || !path[0]) return -1;
    if (R->n_rules <= 0) cnet_domain_route_init(R);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f) && R->n_rules < CNET_DR_MAX_RULES) {
        char *p = line, *k = NULL, *pat = NULL, *rest = NULL;
        CnetRouteKind kind;
        CnetDomainRule *r;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#' || *p == '\n' || *p == '\r') continue;
        /* split on tab or | */
        k = p;
        while (*p && *p != '\t' && *p != '|') p++;
        if (*p) {
            *p++ = 0;
        } else
            continue;
        while (*p == ' ' || *p == '\t') p++;
        pat = p;
        while (*p && *p != '\t' && *p != '|') p++;
        if (*p) {
            *p++ = 0;
        } else
            *p = 0;
        while (*p == ' ' || *p == '\t') p++;
        rest = p;
        while (*rest && (*rest == '\n' || *rest == '\r')) rest++;
        {
            char *e = rest + strlen(rest);
            while (e > rest && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '))
                *--e = 0;
        }
        kind = parse_kind(k);
        if (kind == CNET_ROUTE_NONE || !pat[0]) continue;
        r = &R->rules[R->n_rules];
        memset(r, 0, sizeof *r);
        r->kind = kind;
        r->active = 1;
        r->min_pat_len = CNET_DR_DEFAULT_MIN_PAT;
        scopy(r->pattern, sizeof r->pattern, pat);
        if (kind == CNET_ROUTE_MTK)
            scopy(r->mtk_path, sizeof r->mtk_path, rest);
        else if (kind == CNET_ROUTE_CERT)
            scopy(r->pack_or_skill, sizeof r->pack_or_skill, rest);
        R->n_rules++;
        added++;
    }
    fclose(f);
    if (added > 0) {
        R->loaded_file = 1;
        scopy(R->source, sizeof R->source, path);
    }
    return 0;
}

int cnet_domain_route_n_rules(const CnetDomainRouter *R) {
    return R ? R->n_rules : 0;
}

static void fill_decision(CnetDomainDecision *out, const CnetDomainRule *r,
                          int idx, const char *query, const char *reason) {
    int qlen, plen;
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!r) {
        out->kind = CNET_ROUTE_ABSTAIN;
        out->kind_name = "ABSTAIN";
        out->reason = reason ? reason : "no_domain_match";
        out->rule_index = -1;
        out->conf_x1000 = 0;
        return;
    }
    out->kind = r->kind;
    out->kind_name = cnet_route_kind_name(r->kind);
    out->rule_index = idx;
    out->reason = reason ? reason : "matched";
    scopy(out->pattern, sizeof out->pattern, r->pattern);
    scopy(out->pack_or_skill, sizeof out->pack_or_skill, r->pack_or_skill);
    scopy(out->mtk_path, sizeof out->mtk_path, r->mtk_path);
    plen = (int)strlen(r->pattern);
    qlen = query ? (int)strlen(query) : 0;
    if (qlen < 1) qlen = 1;
    out->pat_len = plen;
    out->conf_x1000 = (plen * 1000) / qlen;
    if (out->conf_x1000 > 1000) out->conf_x1000 = 1000;
}

void cnet_domain_route_resolve(const CnetDomainRouter *R, const char *query,
                               CnetDomainDecision *out) {
    static const CnetRouteKind order[] = {
        CNET_ROUTE_CERT, CNET_ROUTE_MTK, CNET_ROUTE_BASE_GGUF};
    int t, i;
    int best_i = -1, best_len = -1;
    const CnetDomainRule *best = NULL;

    if (!out) return;
    if (!R || !query || !query[0]) {
        fill_decision(out, NULL, -1, query, "empty_or_null");
        return;
    }

    /* Tiered deterministic scan — CERT before MTK before BASE. */
    for (t = 0; t < 3; t++) {
        CnetRouteKind want = order[t];
        best = NULL;
        best_i = -1;
        best_len = -1;
        for (i = 0; i < R->n_rules; i++) {
            const CnetDomainRule *r = &R->rules[i];
            int plen;
            if (!r->active || r->kind != want) continue;
            if (!r->pattern[0]) continue;
            if (!rule_matches(r, query)) continue;
            plen = (int)strlen(r->pattern);
            /* longest pattern wins within tier */
            if (plen > best_len) {
                best_len = plen;
                best = r;
                best_i = i;
            }
        }
        if (best) {
            fill_decision(out, best, best_i, query,
                          t == 0 ? "cert_first"
                          : t == 1 ? "mtk_residual"
                                   : "base_gguf");
            return;
        }
    }
    /* Fail-closed: no MTK, no flush */
    fill_decision(out, NULL, -1, query, "fail_closed_abstain");
}

int cnet_domain_route_selftest(void) {
    CnetDomainRouter R;
    CnetDomainDecision d;
    int fail = 0;
    char tmp[] = "/tmp/cnet_domain_routes_XXXXXX";
    int fd;

#define T(ok, m)                                                             \
    do {                                                                     \
        printf("  %-56s %s\n", m, (ok) ? "PASS" : "FAIL");                   \
        if (!(ok)) fail++;                                                   \
    } while (0)

    printf("=== domain route (CERT-first) ===\n");
    cnet_domain_route_init(&R);
    T(R.n_static >= 8, "static rules loaded");
    T(cnet_domain_route_n_rules(&R) == R.n_static, "n_rules == static");

    cnet_domain_route_resolve(&R, "who are you today", &d);
    T(d.kind == CNET_ROUTE_CERT, "who are you → CERT");
    T(strcmp(d.pack_or_skill, "pack_soul_marble") == 0, "soul pack id");
    T(strcmp(d.reason, "cert_first") == 0, "reason cert_first");

    cnet_domain_route_resolve(&R, "please never self-cert from llm", &d);
    T(d.kind == CNET_ROUTE_CERT, "self-cert phrase → CERT not MTK");

    cnet_domain_route_resolve(&R, "bitnet ternary specialty run", &d);
    T(d.kind == CNET_ROUTE_MTK, "mtk specialty → MTK");
    T(d.mtk_path[0] != 0, "mtk path set");

    cnet_domain_route_resolve(&R, "open residual chat please", &d);
    T(d.kind == CNET_ROUTE_BASE_GGUF, "explicit base → BASE_GGUF");

    cnet_domain_route_resolve(&R, "completely unknown domain xyzzy", &d);
    T(d.kind == CNET_ROUTE_ABSTAIN, "unknown → ABSTAIN fail-closed");
    T(d.rule_index < 0, "abstain has no rule");

    /* CERT wins even if query also mentions mtk-ish words later in table */
    cnet_domain_route_resolve(&R, "format-truncation and mtk residual adapter",
                              &d);
    T(d.kind == CNET_ROUTE_CERT, "CERT tier before MTK when both match");

    /* Policy B+C: short embedded substring must not false-positive */
    cnet_domain_route_resolve(
        &R, "the word format is buried inside unformattedtext blob", &d);
    T(d.kind != CNET_ROUTE_CERT ||
          strstr(d.pattern, "format-truncation") == NULL,
      "no CERT on bare 'format' inside longer token");
    /* word-boundary: pattern as whole tokens still matches */
    cnet_domain_route_resolve(&R, "please run format-truncation fix now", &d);
    T(d.kind == CNET_ROUTE_CERT, "word-boundary CERT still hits full pattern");

    /* Dynamic overlay file */
    fd = mkstemp(tmp);
    if (fd >= 0) {
        FILE *f = fdopen(fd, "w");
        if (f) {
            fputs("# test overlay\n", f);
            fputs("CERT\tunique_domain_alpha\tpack_personal\n", f);
            fputs("MTK\tunique_mtk_beta\t/tmp/beta.tskill\n", f);
            fclose(f);
            T(cnet_domain_route_load_file(&R, tmp) == 0, "load overlay");
            cnet_domain_route_resolve(&R, "unique_domain_alpha query", &d);
            T(d.kind == CNET_ROUTE_CERT &&
                  strcmp(d.pack_or_skill, "pack_personal") == 0,
              "overlay CERT");
            cnet_domain_route_resolve(&R, "unique_mtk_beta now", &d);
            T(d.kind == CNET_ROUTE_MTK, "overlay MTK");
        }
        unlink(tmp);
    } else {
        T(0, "mkstemp overlay");
    }

    /* No malloc stress: many resolves */
    {
        int i;
        for (i = 0; i < 1000; i++)
            cnet_domain_route_resolve(&R, "who are you", &d);
        T(d.kind == CNET_ROUTE_CERT, "1000 resolves stable");
    }

#undef T
    printf("\nfailures=%d\n", fail);
    if (fail) {
        printf("DOMAIN_ROUTE_FAIL\n");
        return 1;
    }
    printf("DOMAIN_ROUTE_PASS\n");
    return 0;
}
