/* ROE evolve tick — unattended gardener, pure C.
 *
 * Learns / mints capsules WITHOUT a human pressing accept every turn, but only
 * when a fail-closed policy says so:
 *
 *   gold_file     artifacts/roe_daily_packs/gold/<sha1_16>.txt matches answer.
 *                 Gold is external verify, so it may skip the reviewer.
 *   multi_stable  same (norm_q, answer) seen >= ROE_EVOLVE_STABLE_N (default 3).
 *                 Always needs an independent reviewer APPROVE.
 *
 * Never promotes a single novel LLM answer, an ABSTAIN, or a blocklisted query:
 * that would be self-CERT. Persona packs (pack_soul_*) are never auto-written.
 *
 * This is the C reimplementation of the retired tools/roe_evolve_tick.py.
 * CNET product path is C only — the reviewer is an external *binary*
 * (ROE_REVIEWER_BIN), never a Python import. With the reviewer gate on and no
 * reviewer binary present, non-gold candidates are skipped reviewer_unavailable
 * (fail closed), never promoted.
 *
 * Usage:
 *   bin/roe_evolve_tick [--dry-run] [--seed-demo] [--no-reviewer] [--reviewer]
 *                       [--teacher] [--stable-n N] [--max-promotes N]
 *
 * Env:
 *   ROE_EVOLVE_STABLE_N=3        ROE_EVOLVE_MAX_PROMOTES=20
 *   ROE_EVOLVE_TEACHER=0         ROE_EVOLVE_REVIEWER=1
 *   ROE_REVIEWER_BIN             reviewer binary: <bin> <query> <answer>,
 *                                stdout must carry APPROVE or REJECT
 *   CNET_PACKS_ROOT  CNET_MINIMAL_ROOT  CNET_FRONT_DOOR_BIN  CNET_BLOCKLIST
 *   ROE_CHARTER_BIN              optional promote-budget hook: <bin> promote <n>
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define PATHMAX   1024
#define QMAX      1024
#define AMAX      4096
/* Gold/skill identity (SHA-1 + norm_q + packs root) is shared with
 * tools/roe_gold_put.c via one header, so the two can never drift apart. */
#include "cnet_roe_gold_id.h"
#include "cnet_json_internal.h"
#define NORMMAX ROE_NORMMAX
#define PATMAX    64    /* pattern truncates to 48 chars */
#define ANSKEEP   500   /* skill/catalog answer truncation, matches spec */

/* --------------------------------------------------------------- utilities */
static void die(const char *msg) {
    fprintf(stderr, "roe_evolve_tick: %s\n", msg);
    exit(2);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *q, size_t n) {
    void *p = realloc(q, n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s, n);
    return p;
}

/* snprintf that refuses to silently truncate a path */
static int spath(char *dst, size_t cap, const char *fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(dst, cap, fmt, ap);
    va_end(ap);
    return (n < 0 || (size_t)n >= cap) ? -1 : 0;
}

static int is_file(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static int is_dir(const char *p) {
    struct stat st;
    return p && p[0] && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_exec(const char *p) {
    return is_file(p) && access(p, X_OK) == 0;
}

/* mkdir -p */
static int mkdir_p(const char *path) {
    char tmp[PATHMAX];
    char *p;
    if (spath(tmp, sizeof tmp, "%s", path) != 0) return -1;
    for (p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0755) != 0 && !is_dir(tmp)) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && !is_dir(tmp)) return -1;
    return 0;
}

static const char *env_or(const char *k, const char *dflt) {
    const char *v = getenv(k);
    return (v && v[0]) ? v : dflt;
}

static int env_int(const char *k, int dflt) {
    const char *v = getenv(k);
    char *end;
    long n;
    if (!v || !v[0]) return dflt;
    n = strtol(v, &end, 10);
    if (end == v || n < 0 || n > 1000000) return dflt;
    return (int)n;
}

/* -1 unset, 0 false, 1 true */
static int env_tri(const char *k) {
    const char *v = getenv(k);
    if (!v || !v[0]) return -1;
    if (!strcasecmp(v, "1") || !strcasecmp(v, "true") || !strcasecmp(v, "yes")) return 1;
    if (!strcasecmp(v, "0") || !strcasecmp(v, "false") || !strcasecmp(v, "no")) return 0;
    return -1;
}

static void utc_now(char out[32]) {
    time_t t = time(NULL);
    struct tm g;
    gmtime_r(&t, &g);
    strftime(out, 32, "%Y-%m-%dT%H:%M:%SZ", &g);
}


/* Build the substring trigger for a promoted capsule.
 *
 * A skill fires only when its pattern is a case-insensitive SUBSTRING of the
 * incoming query (src/roe/cnet_roe_asi.c:125, contains_ci). So the pattern must
 * be a contiguous run of the normalised query — not a filtered join of tokens.
 *
 * The old rule joined "meaningful" tokens with single spaces after dropping
 * stopwords and any 1-character token, which silently produced patterns that
 * could not match their own query:
 *   "using C not JAVA"      -> "using not java"        (C dropped; no match)
 *   "we use C for CNET core"-> "we use for cnet core"  (C dropped; no match)
 *   "the quick brown fox and the lazy dog"
 *                           -> "quick brown fox lazy dog"  (interior "and the"
 *                                                            dropped; no match)
 *
 * Now: skip *leading* stopwords only, then take up to 6 consecutive tokens and
 * return that span verbatim. Dropping leading noise keeps the original intent
 * ("what is X" triggers on X) while the span guarantees the substring property.
 */
static void pattern_for(const char *q, char out[PATMAX]) {
    static const char *stop[] = {"the", "a", "an", "to", "of", "and", "or",
                                 "is", "are", "how", "what", "please", NULL};
    char nq[NORMMAX];
    size_t tstart[24], tend[24];
    int ntok = 0, first = 0, last, k;
    size_t p = 0, from, to, len;

    roe_norm_q(q, nq);
    while (nq[p] && ntok < 24) {
        size_t s;
        if (!isalnum((unsigned char)nq[p])) { p++; continue; }
        s = p++;
        while (nq[p] && (isalnum((unsigned char)nq[p]) || nq[p] == '-')) p++;
        tstart[ntok] = s;
        tend[ntok] = p;
        ntok++;
    }
    if (ntok == 0) {
        snprintf(out, PATMAX, "%.40s", nq[0] ? nq : "auto");
        return;
    }

    /* Skip leading stopwords; if every token is one, keep the whole run. */
    for (first = 0; first < ntok; first++) {
        int isstop = 0;
        char t[NORMMAX];
        size_t tl = tend[first] - tstart[first];
        if (tl >= NORMMAX) tl = NORMMAX - 1;
        memcpy(t, nq + tstart[first], tl);
        t[tl] = 0;
        for (k = 0; stop[k]; k++)
            if (!strcmp(t, stop[k])) { isstop = 1; break; }
        if (!isstop) break;
    }
    if (first >= ntok) first = 0;

    /* Up to 6 consecutive tokens, trimmed to 48 chars on a token boundary. */
    last = (first + 5 < ntok - 1) ? first + 5 : ntok - 1;
    from = tstart[first];
    while (last > first && tend[last] - from > 48) last--;
    to = tend[last];
    len = to - from;
    if (len > 48) len = 48;            /* single oversized token: hard cut */
    memcpy(out, nq + from, len);
    out[len] = 0;
}

/* lower + collapse + truncate 400, then equal-or-substring either way */
static int answers_match(const char *a, const char *b) {
    char ca[512], cb[512];
    roe_norm_trunc(a, ca, sizeof ca, 400);
    roe_norm_trunc(b, cb, sizeof cb, 400);
    if (!strcmp(ca, cb)) return 1;
    if (ca[0] && strstr(cb, ca)) return 1;
    if (cb[0] && strstr(ca, cb)) return 1;
    return 0;
}

static void strip_tag_prefix(char *s) {
    static const char *pfx[] = {"[llm-live] ", "[llm-untrusted] ", "[lookup] ", NULL};
    int i;
    for (i = 0; pfx[i]; i++) {
        size_t n = strlen(pfx[i]);
        if (!strncmp(s, pfx[i], n)) { memmove(s, s + n, strlen(s + n) + 1); return; }
    }
}


/* ------------------------------------------------------------ JSONL reader */
/* Flat-object field reader that honours string state, so a key name appearing
 * inside a value cannot be mistaken for the key. Decodes JSON escapes. */
static int json_field(const char *line, const char *key, char *out, size_t cap) {
    JsonCursor c = {(const unsigned char *)line};
    int found = 0;
    if (!line || !out || !cap) return -1;
    out[0] = 0;
    json_ws(&c);
    if (*c.p++ != '{') return -1;
    json_ws(&c);
    if (*c.p == '}') return -1;
    for (;;) {
        char name[128];
        if (json_string(&c, name, sizeof name)) return -1;
        json_ws(&c);
        if (*c.p++ != ':') return -1;
        json_ws(&c);
        if (!strcmp(name, key)) {
            if (found++ || json_string(&c, out, cap)) return -1;
        } else if (json_value(&c, 1)) return -1;
        json_ws(&c);
        if (*c.p == '}') { c.p++; break; }
        if (*c.p++ != ',') return -1;
        json_ws(&c);
    }
    json_ws(&c);
    return !*c.p && found && out[0] ? 0 : -1;
}

static void json_escape(const char *in, char *out, size_t cap) {
    size_t o = 0;
    size_t i;
    for (i = 0; in && in[i] && o + 7 < cap; i++) {
        unsigned char c = (unsigned char)in[i];
        switch (c) {
        case '"':  out[o++] = '\\'; out[o++] = '"';  break;
        case '\\': out[o++] = '\\'; out[o++] = '\\'; break;
        case '\n': out[o++] = '\\'; out[o++] = 'n';  break;
        case '\r': out[o++] = '\\'; out[o++] = 'r';  break;
        case '\t': out[o++] = '\\'; out[o++] = 't';  break;
        default:
            if (c < 0x20) { snprintf(out + o, 7, "\\u%04x", c); o += 6; }
            else out[o++] = (char)c;
        }
    }
    out[o] = 0;
}

/* shell single-quote a value for popen */
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

/* ------------------------------------------------------------ deploy paths */
typedef struct {
    char root[PATHMAX];        /* repo root                       */
    char packs[PATHMAX];       /* roe_daily_packs                 */
    char miss[PATHMAX];
    char gold_dir[PATHMAX];
    char gold_jsonl[PATHMAX];
    char personal[PATHMAX];
    char report[PATHMAX];
    char state[PATHMAX];
    char blocklist[PATHMAX];
    char front_door[PATHMAX];
} Paths;

/* argv[0] is bin/roe_evolve_tick, so the repo root is its grandparent; the
 * CWD is authoritative when it already looks like the tree (make/systemd
 * both set WorkingDirectory). */
static void resolve_root(char *out, size_t cap, const char *argv0) {
    char buf[PATHMAX];
    char *slash;
    const char *env = getenv("CNET_ROOT");
    if (env && env[0] && is_dir(env)) { spath(out, cap, "%s", env); return; }
    if (is_dir("tools") && is_dir("include")) { spath(out, cap, "%s", "."); return; }
    if (argv0 && spath(buf, sizeof buf, "%s", argv0) == 0) {
        slash = strrchr(buf, '/');
        if (slash) {
            *slash = 0;                       /* .../bin */
            slash = strrchr(buf, '/');
            if (slash) { *slash = 0; spath(out, cap, "%s", buf); return; }
        }
    }
    spath(out, cap, "%s", ".");
}

static void resolve_paths(Paths *P, const char *argv0) {
    const char *minroot = env_or("CNET_MINIMAL_ROOT", "");
    const char *packs_env = env_or("CNET_PACKS_ROOT", "");
    char cand[PATHMAX];

    memset(P, 0, sizeof *P);
    resolve_root(P->root, sizeof P->root, argv0);

    if (packs_env[0]) {
        spath(P->packs, sizeof P->packs, "%s", packs_env);
    } else if (minroot[0] &&
               spath(cand, sizeof cand, "%s/data/roe_daily_packs", minroot) == 0 &&
               is_dir(cand)) {
        spath(P->packs, sizeof P->packs, "%s", cand);
    } else {
        spath(P->packs, sizeof P->packs, "%s/artifacts/roe_daily_packs", P->root);
    }

    spath(P->miss,       sizeof P->miss,       "%s/miss_log.jsonl",   P->packs);
    spath(P->gold_dir,   sizeof P->gold_dir,   "%s/gold",             P->packs);
    spath(P->gold_jsonl, sizeof P->gold_jsonl, "%s/gold.jsonl",       P->packs);
    spath(P->personal,   sizeof P->personal,   "%s/pack_personal",    P->packs);
    spath(P->report,     sizeof P->report,     "%s/EVOLVE_TICK.json", P->packs);
    spath(P->state,      sizeof P->state,      "%s/evolve_state.json", P->packs);

    if (env_or("CNET_BLOCKLIST", "")[0]) {
        spath(P->blocklist, sizeof P->blocklist, "%s", getenv("CNET_BLOCKLIST"));
    } else if (minroot[0] &&
               spath(cand, sizeof cand, "%s/config/promote_blocklist.txt", minroot) == 0 &&
               is_file(cand)) {
        spath(P->blocklist, sizeof P->blocklist, "%s", cand);
    } else {
        spath(P->blocklist, sizeof P->blocklist, "%s/config/promote_blocklist.txt", P->root);
    }

    if (env_or("CNET_FRONT_DOOR_BIN", "")[0]) {
        spath(P->front_door, sizeof P->front_door, "%s", getenv("CNET_FRONT_DOOR_BIN"));
    } else if (minroot[0] &&
               spath(cand, sizeof cand, "%s/bin/roe_front_door", minroot) == 0 &&
               is_file(cand)) {
        spath(P->front_door, sizeof P->front_door, "%s", cand);
    } else {
        spath(P->front_door, sizeof P->front_door, "%s/bin/roe_front_door", P->root);
    }
}

/* ---------------------------------------------------------------- blocklist */
typedef struct { char kind[24]; char val[256]; } Rule;
typedef struct { Rule *v; int n, cap; } Rules;

static void rules_load(Rules *R, const char *path) {
    FILE *f;
    char line[512];
    R->v = NULL; R->n = R->cap = 0;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *bar, *k, *v, *e;
        line[strcspn(line, "\r\n")] = 0;
        k = line;
        while (*k && isspace((unsigned char)*k)) k++;
        if (!*k || *k == '#') continue;
        bar = strchr(k, '|');
        if (!bar) continue;
        *bar = 0;
        v = bar + 1;
        for (e = k + strlen(k); e > k && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        while (*v && isspace((unsigned char)*v)) v++;
        for (e = v + strlen(v); e > v && isspace((unsigned char)e[-1]); e--) e[-1] = 0;
        if (!*k || !*v) continue;
        if (R->n == R->cap) {
            R->cap = R->cap ? R->cap * 2 : 32;
            R->v = (Rule *)xrealloc(R->v, (size_t)R->cap * sizeof *R->v);
        }
        {
            size_t i;
            snprintf(R->v[R->n].kind, sizeof R->v[R->n].kind, "%s", k);
            for (i = 0; R->v[R->n].kind[i]; i++)
                R->v[R->n].kind[i] = (char)tolower((unsigned char)R->v[R->n].kind[i]);
            snprintf(R->v[R->n].val, sizeof R->v[R->n].val, "%s", v);
        }
        R->n++;
    }
    fclose(f);
}

static void lower_copy(const char *in, char *out, size_t cap) {
    size_t i;
    for (i = 0; in && in[i] && i + 1 < cap; i++)
        out[i] = (char)tolower((unsigned char)in[i]);
    out[i] = 0;
}

/* Returns 0 when allowed, or fills reason and returns 1. Fail-closed:
 * ABSTAIN / "no local skill ... ask user" are blocked even with no rules file.
 * NOTE: "regex|" rules are honoured as literal substrings on the normalised
 * query -- see report; no regex engine is linked into the product path. */
static int promote_blocked(const Rules *R, const char *query, const char *answer,
                           char *reason, size_t rcap) {
    char nq[NORMMAX], ans[AMAX], v[256];
    int i;
    roe_norm_q(query, nq);
    lower_copy(answer ? answer : "", ans, sizeof ans);
    for (i = 0; i < R->n; i++) {
        lower_copy(R->v[i].val, v, sizeof v);
        if (!strcmp(R->v[i].kind, "substr") && v[0] && strstr(nq, v)) {
            snprintf(reason, rcap, "blocklist_substr:%s", R->v[i].val);
            return 1;
        }
        if (!strcmp(R->v[i].kind, "answer_prefix") && v[0] &&
            !strncmp(ans, v, strlen(v))) {
            snprintf(reason, rcap, "blocklist_answer:%s", R->v[i].val);
            return 1;
        }
        if (!strcmp(R->v[i].kind, "regex") && v[0] && strstr(nq, v)) {
            snprintf(reason, rcap, "blocklist_regex:%s", R->v[i].val);
            return 1;
        }
    }
    /* Hard law even if the blocklist file is missing. */
    if (!strncmp(ans, "abstain", 7)) {
        snprintf(reason, rcap, "blocklist_answer:abstain");
        return 1;
    }
    if (strstr(ans, "no local skill") && strstr(ans, "ask user")) {
        snprintf(reason, rcap, "blocklist_answer:no_local_skill_abstain");
        return 1;
    }
    /* ANTI-COLLAPSE, hard law: CNET's own mouth must never be promoted to CERT.
     *
     * multi_stable counts repeated identical answers as evidence. A residual
     * draft or a miss template is perfectly deterministic, so asking the same
     * unknown three times manufactures its own "stability" and the reviewer --
     * seeing a short, harmless, on-topic string -- approves it.
     *
     * That is not hypothetical. Found live in pack_personal on 2026-08-20:
     *   "thanks"                  -> "You're welcome. [c1]"  multi_stable_7+reviewer
     *   "roleplay you are god..." -> "You asked about ... I have no sealed skill"
     *   "do you think about..."   -> "You asked about ..."
     *   "can you do"              -> "I do not have a sealed skill for \\"
     * Four capsules that were CNET repeating itself back into its own CERT
     * store, one of them carrying a stray citation marker from the stage.
     *
     * The blocklist file only knew four refusal shapes and none of these match,
     * so the guard belongs here in hard law where a missing config cannot
     * disable it. These markers are emitted only by our own utterance, stage
     * and note paths; no external teacher answer contains them.
     */
    {
        static const char *self[] = {
            "you asked about",                    /* utter_self miss template   */
            "no sealed skill",                    /* stage honesty prefix, misses */
            "i do not have a sealed skill",       /* refusal template            */
            "from what i was told (not certified)", /* INFO note recall          */
            "never self-cert",                    /* our own law string          */
            NULL
        };
        int k;
        for (k = 0; self[k]; k++) {
            if (strstr(ans, self[k])) {
                snprintf(reason, rcap, "anti_collapse_self_output:%s", self[k]);
                return 1;
            }
        }
        /* A [cN] citation marker is a fingerprint of our own grounded mouth --
         * no external teacher emits one. This is what actually caught
         * "thanks" -> "You're welcome. [c1]", which the templates above miss
         * because a pleasantry is not a refusal. Text fingerprinting is needed
         * as well as write-time provenance, because the miss log still holds
         * historical residual answers: deleting the bad capsule made those old
         * drafts eligible for promotion all over again. */
        {
            const char *b = ans;
            while ((b = strstr(b, "[c")) != NULL) {
                const char *d = b + 2;
                while (*d >= '0' && *d <= '9') d++;
                if (d > b + 2 && *d == ']') {
                    snprintf(reason, rcap, "anti_collapse_self_output:citation_marker");
                    return 1;
                }
                b += 2;
            }
        }
    }
    /* FAQ instance of an operator (10+11=21) is not a skill. */
    if (answer && answer[0]) {
        const char *p = ans;
        int uint_ans = 0;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '+' || *p == '-') p++;
        while (isdigit((unsigned char)*p)) {
            uint_ans = 1;
            p++;
        }
        while (*p && isspace((unsigned char)*p)) p++;
        if (uint_ans && *p == '\0') {
            if (strstr(nq, " plus") || strstr(nq, "plus ") ||
                strstr(nq, " minus") || strstr(nq, "minus ") ||
                strstr(nq, " times") || strstr(nq, "times ") ||
                strstr(nq, " multiplied") || strstr(nq, " divided") ||
                strstr(nq, "divided ") || strstr(nq, " modulo") ||
                strstr(nq, " mod ")) {
                snprintf(reason, rcap, "operator_not_faq");
                return 1;
            }
            {
                size_t i;
                for (i = 0; nq[i]; i++) {
                    const char *s;
                    if (!isdigit((unsigned char)nq[i])) continue;
                    s = nq + i;
                    while (isdigit((unsigned char)*s)) s++;
                    while (*s == ' ') s++;
                    if (*s == '+' || *s == '*' || *s == '-' || *s == '/' ||
                        *s == '%') {
                        s++;
                        while (*s == ' ') s++;
                        if (isdigit((unsigned char)*s)) {
                            snprintf(reason, rcap, "operator_not_faq");
                            return 1;
                        }
                    }
                }
            }
        }
    }
    return 0;
}

/* ----------------------------------------------------------- miss clusters */
typedef struct { char *ans; int count; } AnsCount;

typedef struct {
    char norm[NORMMAX];
    char query[QMAX];      /* last-seen original casing, like the spec */
    AnsCount *ans;
    int nans, acap;
    int n;                 /* rows in cluster */
    int first;             /* first row index, for stable ordering */
} Cluster;

typedef struct { Cluster *v; int n, cap; } Clusters;

static Cluster *cluster_get(Clusters *C, const char *nq, int idx) {
    int i;
    for (i = 0; i < C->n; i++)
        if (!strcmp(C->v[i].norm, nq)) return &C->v[i];
    if (C->n == C->cap) {
        C->cap = C->cap ? C->cap * 2 : 64;
        C->v = (Cluster *)xrealloc(C->v, (size_t)C->cap * sizeof *C->v);
    }
    memset(&C->v[C->n], 0, sizeof C->v[C->n]);
    snprintf(C->v[C->n].norm, NORMMAX, "%s", nq);
    C->v[C->n].first = idx;
    return &C->v[C->n++];
}

static void cluster_add_answer(Cluster *c, const char *a) {
    int i;
    if (!a || !a[0]) return;
    for (i = 0; i < c->nans; i++)
        if (!strcmp(c->ans[i].ans, a)) { c->ans[i].count++; return; }
    if (c->nans == c->acap) {
        c->acap = c->acap ? c->acap * 2 : 8;
        c->ans = (AnsCount *)xrealloc(c->ans, (size_t)c->acap * sizeof *c->ans);
    }
    c->ans[c->nans].ans = xstrdup(a);
    c->ans[c->nans].count = 1;
    c->nans++;
}

/* most frequent answer in the cluster */
static const char *pick_stable(const Cluster *c, int *count_out) {
    int i, best = -1;
    *count_out = 0;
    for (i = 0; i < c->nans; i++)
        if (best < 0 || c->ans[i].count > c->ans[best].count) best = i;
    if (best < 0) return NULL;
    *count_out = c->ans[best].count;
    return c->ans[best].ans;
}

static int cluster_cmp(const void *a, const void *b) {
    const Cluster *x = (const Cluster *)a, *y = (const Cluster *)b;
    if (x->n != y->n) return y->n - x->n;      /* descending row count */
    return x->first - y->first;                /* stable by first sighting */
}

/* Is this miss row CNET's own output rather than external evidence?
 *
 * The query is real organic demand either way, so the row still counts toward
 * the cluster. What must never count is the ANSWER: multi_stable votes on
 * repeated identical answers, and our residual is deterministic, so asking the
 * same unknown three times manufactures its own stability.
 *
 * Prefers the explicit write-time tag; the `via`/`source` checks cover rows
 * already in the log from before the tag existed. The text fingerprints in
 * promote_blocked() remain as a last line of defence for anything that slips
 * through both. */
/* Positive provenance allowlist. Unknown, duplicate, malformed and own-output
 * records remain demand only. A label is not authentication: these logs and
 * verified-tool records must be written by trusted local ingestion code. */
static int miss_row_self_authored(const char *line) {
    JsonCursor c = {(const unsigned char *)line};
    char source[64] = "", via[96] = "";
    char keys[64][128];
    size_t nkeys = 0;
    int verified = 0;
    if (!line) return 1;
    json_ws(&c);
    if (*c.p++ != '{') return 1;
    json_ws(&c);
    if (*c.p == '}') return 1;
    for (;;) {
        char key[128];
        if (nkeys == 64 || json_string(&c, key, sizeof key)) return 1;
        for (size_t i = 0; i < nkeys; i++)
            if (!strcmp(keys[i], key)) return 1;
        snprintf(keys[nkeys++], sizeof keys[0], "%s", key);
        json_ws(&c);
        if (*c.p++ != ':') return 1;
        json_ws(&c);
        if (!strcmp(key, "source")) {
            if (json_string(&c, source, sizeof source)) return 1;
        } else if (!strcmp(key, "via")) {
            if (json_string(&c, via, sizeof via)) return 1;
        } else if (!strcmp(key, "self_authored") || !strcmp(key, "claimed_cert")) {
            if (!strncmp((const char *)c.p, "false", 5)) c.p += 5;
            else if (*c.p == '0') c.p++;
            else return 1;
        } else if (!strcmp(key, "verified")) {
            if (!strncmp((const char *)c.p, "true", 4)) { verified = 1; c.p += 4; }
            else if (!strncmp((const char *)c.p, "false", 5)) c.p += 5;
            else return 1;
        } else if (json_value(&c, 1)) return 1;
        json_ws(&c);
        if (*c.p == '}') { c.p++; break; }
        if (*c.p++ != ',') return 1;
        json_ws(&c);
    }
    json_ws(&c);
    if (*c.p || !strcmp(via, "core_open_chat") ||
        !strcmp(via, "stage_residual") || !strcmp(via, "kb_recall")) return 1;
    return strcmp(source, "LLM") && strcmp(source, "EXTERNAL_TEACHER") &&
           strcmp(source, "USER_CORRECTION") &&
           !(verified && !strcmp(source, "TOOL"));
}

static long load_misses(const char *path, Clusters *C) {
    FILE *f;
    char *line = NULL;
    size_t cap = 0;
    long rows = 0;
    long self_rows = 0;
    int idx = 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (getline(&line, &cap, f) > 0) {
        char q[QMAX], a[AMAX], nq[NORMMAX];
        Cluster *c;
        if (json_field(line, "query", q, sizeof q) != 0 &&
            json_field(line, "q", q, sizeof q) != 0) continue;
        rows++;
        roe_norm_q(q, nq);
        if (!nq[0]) continue;
        c = cluster_get(C, nq, idx++);
        snprintf(c->query, sizeof c->query, "%s", q);
        c->n++;
        /* Cluster the demand, but never vote with our own answer. */
        if (miss_row_self_authored(line)) { self_rows++; continue; }
        if (json_field(line, "answer", a, sizeof a) == 0 ||
            json_field(line, "teacher_answer", a, sizeof a) == 0 ||
            json_field(line, "snippet", a, sizeof a) == 0) {
            char *t = a;
            size_t e;
            strip_tag_prefix(t);
            while (*t && isspace((unsigned char)*t)) t++;
            for (e = strlen(t); e > 0 && isspace((unsigned char)t[e - 1]); e--) t[e - 1] = 0;
            cluster_add_answer(c, t);
        }
    }
    free(line);
    fclose(f);
    if (self_rows)
        fprintf(stderr, "[roe_evolve_tick] anti-collapse: %ld self-authored miss "
                        "rows counted as demand but excluded from answer votes\n",
                self_rows);
    return rows;
}

/* -------------------------------------------------------------------- gold */
static int load_gold(const Paths *P, const char *q, char *out, size_t cap) {
    char h[17], path[PATHMAX];
    FILE *f;
    size_t n, e;
    roe_q_hash16(q, h);
    if (spath(path, sizeof path, "%s/%s.txt", P->gold_dir, h) == 0 &&
        (f = fopen(path, "r")) != NULL) {
        n = fread(out, 1, cap - 1, f);
        fclose(f);
        out[n] = 0;
        for (e = strlen(out); e > 0 && isspace((unsigned char)out[e - 1]); e--) out[e - 1] = 0;
        if (out[0]) return 0;
    }
    /* gold.jsonl fallback */
    if ((f = fopen(P->gold_jsonl, "r")) != NULL) {
        char *line = NULL;
        size_t lc = 0;
        char nq[NORMMAX], rq[QMAX], rnq[NORMMAX];
        int hit = 0;
        roe_norm_q(q, nq);
        while (getline(&line, &lc, f) > 0) {
            if (json_field(line, "query", rq, sizeof rq) != 0) continue;
            roe_norm_q(rq, rnq);
            if (strcmp(rnq, nq)) continue;
            if (json_field(line, "answer", out, cap) == 0 && out[0]) { hit = 1; break; }
        }
        free(line);
        fclose(f);
        if (hit) return 0;
    }
    out[0] = 0;
    return -1;
}

/* ---------------------------------------------------- front door / reviewer */
/* Run a command, capture up to cap-1 bytes of stdout+stderr. */
static int run_capture(const char *cmd, char *out, size_t cap) {
    FILE *p = popen(cmd, "r");
    size_t n;
    int rc;
    out[0] = 0;
    if (!p) return -1;
    n = fread(out, 1, cap - 1, p);
    out[n] = 0;
    rc = pclose(p);
    return (rc == -1) ? -1 : rc;
}

static int already_local(const Paths *P, const char *q) {
    char qq[QMAX * 4], cmd[PATHMAX * 2 + QMAX * 4], out[8192];
    if (!is_exec(P->front_door)) return 0;
    if (sh_quote(q, qq, sizeof qq) != 0) return 0;
    if (spath(cmd, sizeof cmd,
              "ROE_LIVE=0 ROE_LLM=0 '%s' ask %s --root '%s' 2>&1",
              P->front_door, qq, P->packs) != 0) return 0;
    if (run_capture(cmd, out, sizeof out) < 0) return 0;
    return strstr(out, "source=LOCAL") != NULL || strstr(out, "src=LOCAL") != NULL;
}

/* Optional live teacher fill. Still untrusted — policy still decides. */
static int teacher_fill(const Paths *P, const char *q, char *out, size_t cap) {
    char qq[QMAX * 4], cmd[PATHMAX * 2 + QMAX * 4], buf[16384], *a;
    if (!is_exec(P->front_door)) return -1;
    if (sh_quote(q, qq, sizeof qq) != 0) return -1;
    if (spath(cmd, sizeof cmd,
              "ROE_LIVE=%s ROE_LLM=%s ROE_LOOKUP=%s ROE_LLM_THINK=%s '%s' ask %s --root '%s' 2>/dev/null",
              env_or("ROE_LIVE", "1"), env_or("ROE_LLM", "1"),
              env_or("ROE_LOOKUP", "0"), env_or("ROE_LLM_THINK", "0"),
              P->front_door, qq, P->packs) != 0) return -1;
    if (run_capture(cmd, buf, sizeof buf) < 0) return -1;
    for (a = buf; a; ) {
        char *nl;
        if (!strncmp(a, "A:", 2)) {
            char *s = a + 2;
            size_t e;
            while (*s == ' ' || *s == '\t') s++;
            nl = strchr(s, '\n');
            if (nl) *nl = 0;
            snprintf(out, cap, "%s", s);
            for (e = strlen(out); e > 0 && isspace((unsigned char)out[e - 1]); e--) out[e - 1] = 0;
            return out[0] ? 0 : -1;
        }
        nl = strchr(a, '\n');
        a = nl ? nl + 1 : NULL;
    }
    return -1;
}

/* Reviewer is a separate ROLE and a separate BINARY — never the teacher, and
 * never a Python import. Contract: `$ROE_REVIEWER_BIN <query> <answer>` prints
 * APPROVE or REJECT on stdout. Anything else (missing binary, non-zero exit,
 * unparseable output) is treated as NOT approved: fail closed. */
typedef struct { int approved; char verdict[16]; char detail[160]; } Review;

static int reviewer_available(void) {
    const char *bin = env_or("ROE_REVIEWER_BIN", "");
    return bin[0] && is_exec(bin);
}

static void reviewer_run(const char *q, const char *ans, Review *rv) {
    const char *bin = env_or("ROE_REVIEWER_BIN", "");
    char qq[QMAX * 4], aa[AMAX * 4], cmd[PATHMAX + QMAX * 4 + AMAX * 4], out[8192];
    char *up;
    int rc;
    memset(rv, 0, sizeof *rv);
    snprintf(rv->verdict, sizeof rv->verdict, "UNAVAILABLE");
    if (!bin[0] || !is_exec(bin)) return;
    if (sh_quote(q, qq, sizeof qq) != 0 || sh_quote(ans, aa, sizeof aa) != 0) return;
    if (spath(cmd, sizeof cmd, "'%s' %s %s 2>&1", bin, qq, aa) != 0) return;
    rc = run_capture(cmd, out, sizeof out);
    snprintf(rv->detail, sizeof rv->detail, "%.*s", (int)(sizeof rv->detail - 1), out);
    for (up = rv->detail; *up; up++) if (*up == '\n' || *up == '\r') *up = ' ';
    if (rc != 0) { snprintf(rv->verdict, sizeof rv->verdict, "ERROR"); return; }
    /* REJECT wins if both appear — fail closed. */
    if (strstr(out, "REJECT")) { snprintf(rv->verdict, sizeof rv->verdict, "REJECT"); return; }
    if (strstr(out, "APPROVE")) {
        snprintf(rv->verdict, sizeof rv->verdict, "APPROVE");
        rv->approved = 1;
        return;
    }
    snprintf(rv->verdict, sizeof rv->verdict, "UNPARSED");
}

/* -------------------------------------------------------- pack_personal IO */
static void write_text(const char *path, const char *body) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(body, f);
    fclose(f);
}

static void ensure_personal_pack(const Paths *P) {
    char p[PATHMAX];
    mkdir_p(P->personal);
    if (spath(p, sizeof p, "%s/catalog.jsonl", P->personal) == 0 && !is_file(p))
        write_text(p, "");
    if (spath(p, sizeof p, "%s/PACK.abi", P->personal) == 0 && !is_file(p))
        write_text(p,
                   "ROE_DAILY_PACK\n"
                   "abi_version 1\n"
                   "id pack_personal\n"
                   "title Unattended promote garden (verified only)\n"
                   "cat personal\n"
                   "kind garden\n"
                   "never_self_cert 1\n"
                   "second_brain 0\n"
                   "seal_path forbidden\n"
                   "load_policy always_on_optional\n");
    if (spath(p, sizeof p, "%s/MANIFEST.txt", P->personal) == 0)
        write_text(p,
                   "pack_personal: auto-promoted skills after gold/multi_stable only.\n"
                   "never_self_cert:1  human button not required per turn.\n");
}

static int catalog_has(const char *cat_path, const char *sid) {
    FILE *f = fopen(cat_path, "r");
    char *line = NULL;
    size_t cap = 0;
    int hit = 0;
    char id[64];
    if (!f) return 0;
    while (getline(&line, &cap, f) > 0)
        if (json_field(line, "id", id, sizeof id) == 0 && !strcmp(id, sid)) { hit = 1; break; }
    free(line);
    fclose(f);
    return hit;
}


static int promote_via_front_door(const Paths *P, const char *q, const char *gold, int dry) {
    char qq[QMAX * 4], gg[AMAX * 4], ff[PATHMAX * 4], pp[PATHMAX * 4];
    char cmd[PATHMAX * 8 + QMAX * 4 + AMAX * 4 + 128], out[8192];
    int rc;
    if (dry) return 1;
    if (!is_exec(P->front_door)) return 0;
    if (sh_quote(q, qq, sizeof qq) != 0 || sh_quote(gold, gg, sizeof gg) != 0 ||
        sh_quote(P->front_door, ff, sizeof ff) || sh_quote(P->packs, pp, sizeof pp)) return 0;
    if (spath(cmd, sizeof cmd,
              "%s ask %s --accept --gold %s --promote-pack pack_personal --root %s 2>&1",
              ff, qq, gg, pp) != 0) return 0;
    rc = run_capture(cmd, out, sizeof out);
    return rc == 0 && strstr(out, "promote=yes pack=pack_personal ") != NULL;
}

static void append_jsonl(const char *path, const char *body) {
    FILE *f = fopen(path, "a");
    if (!f) return;
    fprintf(f, "%s\n", body);
    fclose(f);
}

/* ------------------------------------------------------------------- state */
typedef struct { char **v; int n, cap; } Ids;

static int ids_has(const Ids *I, const char *s) {
    int i;
    for (i = 0; i < I->n; i++) if (!strcmp(I->v[i], s)) return 1;
    return 0;
}

static void ids_add(Ids *I, const char *s) {
    if (ids_has(I, s)) return;
    if (I->n == I->cap) {
        I->cap = I->cap ? I->cap * 2 : 32;
        I->v = (char **)xrealloc(I->v, (size_t)I->cap * sizeof *I->v);
    }
    I->v[I->n++] = xstrdup(s);
}

static void ids_del(Ids *I, const char *s) {
    int i;
    for (i = 0; i < I->n; i++)
        if (!strcmp(I->v[i], s)) {
            free(I->v[i]);
            I->v[i] = I->v[--I->n];
            return;
        }
}

static int str_cmp(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* evolve_state.json is written by us; a flat "promoted_ids":[ "..", ".." ]. */
static void state_load(const char *path, Ids *I, long *ticks) {
    FILE *f = fopen(path, "r");
    char *buf;
    long sz;
    const char *p;
    *ticks = 0;
    if (!f) return;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > (long)(8 * 1024 * 1024)) { fclose(f); return; }
    buf = (char *)xmalloc((size_t)sz + 1);
    sz = (long)fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    p = strstr(buf, "\"promoted_ids\"");
    if (p && (p = strchr(p, '[')) != NULL) {
        p++;
        while (*p && *p != ']') {
            if (*p == '"') {
                const char *s = ++p;
                char id[64];
                size_t n;
                while (*p && *p != '"') p++;
                n = (size_t)(p - s);
                if (n && n < sizeof id) { memcpy(id, s, n); id[n] = 0; ids_add(I, id); }
                if (*p) p++;
            } else {
                p++;
            }
        }
    }
    p = strstr(buf, "\"ticks\"");
    if (p && (p = strchr(p, ':')) != NULL) *ticks = strtol(p + 1, NULL, 10);
    free(buf);
}

static void state_save(const char *path, Ids *I, long ticks, const char *ts) {
    FILE *f = fopen(path, "w");
    int i;
    if (!f) return;
    if (I->n > 1) qsort(I->v, (size_t)I->n, sizeof *I->v, str_cmp);
    fprintf(f, "{\n  \"promoted_ids\": [");
    for (i = 0; i < I->n; i++) fprintf(f, "%s\n    \"%s\"", i ? "," : "", I->v[i]);
    fprintf(f, "%s],\n  \"last_tick\": \"%s\",\n  \"ticks\": %ld\n}\n",
            I->n ? "\n  " : "", ts, ticks);
    fclose(f);
}

/* ------------------------------------------------------------------ report */
typedef struct {
    char query[QMAX];
    char skill_id[64];
    char pattern[PATMAX];
    char reason[128];
    char preview[160];
    char verdict[16];
    int front_door;
    int has_review;
} Promoted;

typedef struct {
    char query[QMAX];
    char reason[192];
    int  stable_cnt, need, has_gold, detailed;
} Skipped;

typedef struct {
    Promoted *p; int np, pcap;
    Skipped  *s; int ns, scap;
} Report;

static Promoted *report_promote(Report *R) {
    if (R->np == R->pcap) {
        R->pcap = R->pcap ? R->pcap * 2 : 16;
        R->p = (Promoted *)xrealloc(R->p, (size_t)R->pcap * sizeof *R->p);
    }
    memset(&R->p[R->np], 0, sizeof R->p[R->np]);
    return &R->p[R->np++];
}

static void report_skip(Report *R, const char *q, const char *reason) {
    Skipped *s;
    if (R->ns == R->scap) {
        R->scap = R->scap ? R->scap * 2 : 64;
        R->s = (Skipped *)xrealloc(R->s, (size_t)R->scap * sizeof *R->s);
    }
    s = &R->s[R->ns++];
    memset(s, 0, sizeof *s);
    snprintf(s->query, sizeof s->query, "%s", q);
    snprintf(s->reason, sizeof s->reason, "%s", reason);
}

static void write_report(const Paths *P, const Report *R, const char *ts, long miss_rows,
                         int nclusters, int stable_n, int use_reviewer, int dry) {
    FILE *f = fopen(P->report, "w");
    char e1[QMAX * 2], e2[512], e3[512];
    int i;
    if (!f) return;
    fprintf(f, "{\n  \"ts\": \"%s\",\n", ts);
    fprintf(f, "  \"miss_rows\": %ld,\n", miss_rows);
    fprintf(f, "  \"clusters\": %d,\n", nclusters);
    fprintf(f, "  \"stable_n\": %d,\n", stable_n);
    fprintf(f, "  \"promoted\": [");
    for (i = 0; i < R->np; i++) {
        const Promoted *p = &R->p[i];
        json_escape(p->query, e1, sizeof e1);
        json_escape(p->reason, e2, sizeof e2);
        json_escape(p->preview, e3, sizeof e3);
        fprintf(f, "%s\n    {\"query\": \"%s\", \"skill_id\": \"%s\", \"pattern\": \"%s\", "
                   "\"reason\": \"%s\", \"front_door\": %s, \"answer_preview\": \"%s\"",
                i ? "," : "", e1, p->skill_id, p->pattern, e2,
                p->front_door ? "true" : "false", e3);
        if (p->has_review) fprintf(f, ", \"reviewer\": {\"verdict\": \"%s\"}", p->verdict);
        fprintf(f, "}");
    }
    fprintf(f, "%s],\n", R->np ? "\n  " : "");
    fprintf(f, "  \"skipped\": [");
    for (i = 0; i < R->ns; i++) {
        const Skipped *s = &R->s[i];
        json_escape(s->query, e1, sizeof e1);
        json_escape(s->reason, e2, sizeof e2);
        fprintf(f, "%s\n    {\"query\": \"%s\", \"reason\": \"%s\"", i ? "," : "", e1, e2);
        if (s->detailed)
            fprintf(f, ", \"stable_cnt\": %d, \"need\": %d, \"has_gold\": %s",
                    s->stable_cnt, s->need, s->has_gold ? "true" : "false");
        fprintf(f, "}");
    }
    fprintf(f, "%s],\n", R->ns ? "\n  " : "");
    fprintf(f, "  \"reviewer_enabled\": %s,\n", use_reviewer ? "true" : "false");
    fprintf(f, "  \"dry_run\": %s,\n", dry ? "true" : "false");
    fprintf(f, "  \"policy\": [\n    \"gold_file\",\n    \"multi_stable>=%d\"%s\n  ],\n",
            stable_n, use_reviewer ? ",\n    \"reviewer_approve_non_gold\"" : "");
    fprintf(f, "  \"human_accept_required\": false,\n");
    fprintf(f, "  \"roles\": {\n    \"teacher\": \"propose\",\n"
               "    \"reviewer\": \"gate\",\n    \"human\": \"optional_gold_batch\"\n  },\n");
    fprintf(f, "  \"engine\": \"roe_evolve_tick_c\",\n");
    fprintf(f, "  \"note\": \"teacher!=reviewer; gold_file skips reviewer; "
               "multi_stable always requires an independent reviewer\"\n}\n");
    fclose(f);
}

/* Optional promote-budget hook. C binary only — the old Python charter import
 * is gone. Absent hook = no extra restriction; --max-promotes still bounds. */
static int charter_allows(int tick_promotes) {
    const char *bin = env_or("ROE_CHARTER_BIN", "");
    char cmd[PATHMAX + 64], out[2048];
    if (!bin[0] || !is_exec(bin)) return 1;
    if (spath(cmd, sizeof cmd, "'%s' promote %d 2>&1", bin, tick_promotes) != 0) return 1;
    if (run_capture(cmd, out, sizeof out) != 0) return 0;
    return strstr(out, "deny") == NULL && strstr(out, "DENY") == NULL;
}

static void seed_demo(const Paths *P) {
    const char *dq = "roe evolve tick demo query alpha";
    const char *da = "Evolve tick demo answer: unattended promote via gold_file policy.";
    char ts[32], row[1024], h[17], gp[PATHMAX];
    int i;
    utc_now(ts);
    mkdir_p(P->gold_dir);
    snprintf(row, sizeof row,
             "{\"ts\":\"%s\",\"query\":\"%s\",\"answer\":\"%s\",\"source\":\"LLM\"}", ts, dq, da);
    for (i = 0; i < 3; i++) append_jsonl(P->miss, row);
    roe_q_hash16(dq, h);
    if (spath(gp, sizeof gp, "%s/%s.txt", P->gold_dir, h) == 0) {
        FILE *f = fopen(gp, "w");
        if (f) { fprintf(f, "%s\n", da); fclose(f); }
    }
}

/* --selftest replaces the retired
 * `python3 -c "from tools.roe_evolve_tick import is_promote_blocked"` gate line.
 * Pure C, no interpreter in the product path. */
static int selftest(const char *argv0) {
    Paths P;
    Rules rules;
    char reason[192], hex[41], nq[NORMMAX], pat[PATMAX], sid[32];
    int fails = 0, checks = 0;

#define CHK(cond, name)                                                        \
    do {                                                                       \
        checks++;                                                              \
        printf("  %-52s %s\n", (name), (cond) ? "PASS" : "FAIL");              \
        if (!(cond)) fails++;                                                  \
    } while (0)

    resolve_paths(&P, argv0);
    rules_load(&rules, P.blocklist);
    printf("=== roe_evolve_tick selftest ===\n");

    /* SHA-1 must match hashlib/sha1sum or every gold/<hash>.txt breaks. */
    roe_sha1_hex("abc", hex);
    CHK(!strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1 rfc3174 vector");
    roe_sha1_hex("", hex);
    CHK(!strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "sha1 empty vector");

    roe_norm_q("  The   QUICK  Brown  ", nq);
    CHK(!strcmp(nq, "the quick brown"), "norm_q lower+collapse+trim");

    roe_skill_id_for("roe evolve tick demo query alpha", sid);
    CHK(!strcmp(sid, "auto_60e86e983af32d6e"), "skill id stable across the port");

    /* The trigger is a substring match (contains_ci), so every minted
     * pattern must be a substring of its own normalised query. This is the
     * general guard: a filtered-join pattern silently fails to match. */
    {
        static const char *pq[] = {
            "using C not JAVA",
            "we use C for CNET core",
            "What is the Tailscale mesh?",
            "the quick brown fox and the lazy dog",
            "what is a cnet certified unit",
            "CNET ASI meaning",
            "a", "the of and", "x", "",
            NULL};
        int pi, allsub = 1;
        for (pi = 0; pq[pi]; pi++) {
            char pn[NORMMAX], pp[PATMAX];
            roe_norm_q(pq[pi], pn);
            pattern_for(pq[pi], pp);
            if (!pn[0]) continue;
            if (!pp[0] || !strstr(pn, pp)) {
                printf("    pattern not a substring: q=[%s] nq=[%s] pat=[%s]\n",
                       pq[pi], pn, pp);
                allsub = 0;
            }
        }
        CHK(allsub, "pattern is always a substring of norm_q");
    }
    pattern_for("using C not JAVA", pat);
    CHK(!strcmp(pat, "using c not java"), "single-char token kept in pattern");
    pattern_for("we use C for CNET core", pat);
    CHK(!strcmp(pat, "we use c for cnet core"), "C kept in product phrase");
    pattern_for("What is the Tailscale mesh?", pat);
    CHK(strstr(pat, "tailscale") != NULL, "pattern keeps meaningful tokens");

    /* Hard law: refusal text never becomes CERT, blocklist file or not. */
    CHK(promote_blocked(&rules, "ok", "ABSTAIN: no", reason, sizeof reason),
        "abstain answer blocked");
    CHK(promote_blocked(&rules, "ok", "abstain: no local skill", reason, sizeof reason),
        "abstain lowercase blocked");
    CHK(promote_blocked(&rules, "anything",
                        "No local skill for this; ask user.", reason, sizeof reason),
        "no_local_skill answer blocked");

    /* Blocklist file rules. */
    CHK(rules.n > 0, "blocklist rules loaded");
    CHK(promote_blocked(&rules, "zz mystic ooze 99", "x", reason, sizeof reason),
        "probe substring blocked");

    /* A clean pair must still be allowed, or the gate proves nothing. */
    CHK(!promote_blocked(&rules, "what is a certified unit",
                         "A certified unit is a verified capsule.", reason, sizeof reason),
        "clean query+answer allowed");

    /* Anti-collapse: our own mouth must never be promoted. multi_stable counts
     * repeated identical answers, and a residual draft is deterministic, so
     * asking the same unknown three times manufactures its own evidence. */
    CHK(promote_blocked(&rules, "anything",
                        "You asked about foo. I have no sealed skill for that yet.",
                        reason, sizeof reason),
        "self output: utter_self miss template blocked");
    CHK(promote_blocked(&rules, "anything", "I do not have a sealed skill for that",
                        reason, sizeof reason),
        "self output: refusal template blocked");
    CHK(promote_blocked(&rules, "thanks", "You're welcome. [c1]", reason, sizeof reason),
        "self output: citation marker blocked (pleasantry, not a refusal)");
    CHK(promote_blocked(&rules, "anything",
                        "From what I was told (not certified): [1] something",
                        reason, sizeof reason),
        "self output: INFO note recall blocked");
    CHK(!promote_blocked(&rules, "what is a lease",
                         "A CORE lease reserves the unit before TABLE and CERTIFY.",
                         reason, sizeof reason),
        "a real teacher answer is still allowed");
    CHK(promote_blocked(&rules, "10+11", "21", reason, sizeof reason),
        "arith instance FAQ blocked");
    CHK(promote_blocked(&rules, "twelve plus five", "17", reason, sizeof reason),
        "number-word sum FAQ blocked");
    CHK(!promote_blocked(&rules, "twelve", "12", reason, sizeof reason),
        "numeral overlay row still allowed");

    /* Reviewer must fail closed when no reviewer binary is reachable. */
    unsetenv("ROE_REVIEWER_BIN");
    CHK(!reviewer_available(), "reviewer unavailable without ROE_REVIEWER_BIN");
    {
        Review rv;
        setenv("ROE_REVIEWER_BIN", "/nonexistent/reviewer", 1);
        reviewer_run("q", "a", &rv);
        CHK(!rv.approved, "missing reviewer never approves");
        unsetenv("ROE_REVIEWER_BIN");
    }

    printf("\nchecks=%d failures=%d\n", checks, fails);
    if (fails) { printf("ROE_EVOLVE_SELFTEST_FAIL\n"); return 1; }
    printf("EVOLVE_BLOCKLIST_OK\n");
    printf("ROE_EVOLVE_SELFTEST_PASS\n");
    return 0;
#undef CHK
}

static void usage(void) {
    printf("usage: roe_evolve_tick [--dry-run] [--seed-demo] [--reviewer|--no-reviewer]\n"
           "                       [--teacher] [--stable-n N] [--max-promotes N]\n"
           "                       [--gold-only] [--selftest]\n");
}

int main(int argc, char **argv) {
    Paths P;
    Rules rules;
    Clusters C;
    Report R;
    Ids promoted_ids;
    char ts[32];
    long miss_rows, ticks = 0;
    int dry = 0, demo = 0, teacher, want_reviewer = -1, use_reviewer, gold_only = 0;
    int stable_n, max_promotes, nprom = 0, i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--dry-run"))          dry = 1;
        else if (!strcmp(argv[i], "--seed-demo"))   demo = 1;
        else if (!strcmp(argv[i], "--no-reviewer")) want_reviewer = 0;
        else if (!strcmp(argv[i], "--reviewer"))    want_reviewer = 1;
        else if (!strcmp(argv[i], "--teacher"))     setenv("ROE_EVOLVE_TEACHER", "1", 1);
        else if (!strcmp(argv[i], "--gold-only"))   gold_only = 1;
        else if (!strcmp(argv[i], "--stable-n") && i + 1 < argc)
            setenv("ROE_EVOLVE_STABLE_N", argv[++i], 1);
        else if (!strcmp(argv[i], "--max-promotes") && i + 1 < argc)
            setenv("ROE_EVOLVE_MAX_PROMOTES", argv[++i], 1);
        else if (!strcmp(argv[i], "--pattern") && i + 1 < argc) {
            /* inspect the trigger a query would mint (ops / verification) */
            char pq[PATMAX];
            pattern_for(argv[++i], pq);
            printf("%s\n", pq);
            return 0;
        }
        else if (!strcmp(argv[i], "--selftest"))    return selftest(argv[0]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(); return 0; }
        else { fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(); return 2; }
    }

    stable_n     = env_int("ROE_EVOLVE_STABLE_N", 3);
    max_promotes = env_int("ROE_EVOLVE_MAX_PROMOTES", 20);
    teacher      = env_tri("ROE_EVOLVE_TEACHER") == 1;
    if (stable_n < 3) stable_n = 3;   /* repetition proposes, never certifies */

    resolve_paths(&P, argv[0]);

    /* Reviewer default: explicit flag > env > presence of the reviewer config. */
    if (want_reviewer >= 0) {
        use_reviewer = want_reviewer;
    } else {
        int t = env_tri("ROE_EVOLVE_REVIEWER");
        if (t >= 0) {
            use_reviewer = t;
        } else {
            char cfg[PATHMAX];
            use_reviewer = (spath(cfg, sizeof cfg,
                                  "%s/config/roe-reviewer-ollama-cloud.env", P.root) == 0 &&
                            is_file(cfg));
        }
    }

    memset(&C, 0, sizeof C);
    memset(&R, 0, sizeof R);
    memset(&promoted_ids, 0, sizeof promoted_ids);
    rules_load(&rules, P.blocklist);

    mkdir_p(P.packs);
    mkdir_p(P.gold_dir);
    ensure_personal_pack(&P);
    state_load(P.state, &promoted_ids, &ticks);

    if (demo) seed_demo(&P);

    miss_rows = load_misses(P.miss, &C);
    if (C.n > 1) qsort(C.v, (size_t)C.n, sizeof *C.v, cluster_cmp);
    utc_now(ts);

    for (i = 0; i < C.n; i++) {
        Cluster *c = &C.v[i];
        const char *q = c->query[0] ? c->query : c->norm;
        char sid[32], reason[128], gold[AMAX], tfill[AMAX], cat[PATHMAX];
        char pat[PATMAX], final[AMAX], line[AMAX * 2 + QMAX * 2 + 512];
        const char *ans;
        int cnt = 0, have_gold, has_ans;
        Review rv;
        Promoted *pr;

        if (nprom >= max_promotes) break;
        roe_skill_id_for(q, sid);

        /* Blocklist first: probe junk / ABSTAIN never CERT, even if a stale
         * state id says it was promoted before. */
        if (promote_blocked(&rules, q, NULL, reason, sizeof reason)) {
            report_skip(&R, q, reason);
            ids_del(&promoted_ids, sid);
            continue;
        }
        if (spath(cat, sizeof cat, "%s/catalog.jsonl", P.personal) != 0) continue;
        if (ids_has(&promoted_ids, sid) || catalog_has(cat, sid)) {
            report_skip(&R, q, "already_promoted");
            continue;
        }
        if (already_local(&P, q)) {
            report_skip(&R, q, "already_local");
            continue;
        }

        have_gold = (load_gold(&P, q, gold, sizeof gold) == 0);
        ans = pick_stable(c, &cnt);
        has_ans = ans && ans[0];

        if (gold_only && !have_gold) {
            report_skip(&R, q, "gold_only");
            continue;
        }

        if (teacher && !has_ans && !have_gold &&
            teacher_fill(&P, q, tfill, sizeof tfill) == 0) {
            char eq[QMAX * 2], ea[AMAX * 2];
            json_escape(q, eq, sizeof eq);
            json_escape(tfill, ea, sizeof ea);
            snprintf(line, sizeof line,
                     "{\"ts\":\"%s\",\"query\":\"%s\",\"answer\":\"%s\","
                     "\"source\":\"LLM\",\"via\":\"evolve_teacher\"}", ts, eq, ea);
            append_jsonl(P.miss, line);
            ans = tfill;
            has_ans = 1;
            cnt = 1;
        }

        if (have_gold) {
            /* Gold is external verify and always wins over teacher text. */
            snprintf(reason, sizeof reason, "%s",
                     (has_ans && !answers_match(ans, gold)) ? "gold_file_overrides_teacher"
                                                            : "gold_file");
            snprintf(final, sizeof final, "%s", gold);
        } else if (has_ans && cnt >= stable_n) {
            snprintf(final, sizeof final, "%s", ans);
            snprintf(reason, sizeof reason, "multi_stable_%d", cnt);
        } else {
            Skipped *s;
            report_skip(&R, q, "need_gold_or_stable");
            s = &R.s[R.ns - 1];
            s->detailed = 1; s->stable_cnt = cnt; s->need = stable_n; s->has_gold = have_gold;
            continue;
        }

        /* Promote-path blocklist: re-check now that the answer is known, so an
         * ABSTAIN body is refused even when the query itself is clean. */
        {
            char br[192];
            if (promote_blocked(&rules, q, final, br, sizeof br)) {
                report_skip(&R, q, br);
                continue;
            }
        }

        /* Never auto-write persona/soul answers. */
        if (strstr(c->norm, "soul") && strstr(c->norm, "who are you")) {
            report_skip(&R, q, "persona_guard");
            continue;
        }
        if (!charter_allows(nprom)) {
            report_skip(&R, q, "charter_promote_budget");
            continue;
        }

        /* Separate REVIEWER role. gold_file skips it (already external-verified);
         * everything else fails closed when no reviewer binary is reachable. */
        memset(&rv, 0, sizeof rv);
        if (strncmp(reason, "gold_file", 9) != 0) {
            if (!use_reviewer || !reviewer_available()) {
                report_skip(&R, q, "reviewer_unavailable");
                continue;
            }
            reviewer_run(q, final, &rv);
            {
                char eq[QMAX * 2], ed[512], rl[PATHMAX];
                json_escape(q, eq, sizeof eq);
                json_escape(rv.detail, ed, sizeof ed);
                if (spath(rl, sizeof rl, "%s/review_log.jsonl", P.packs) == 0) {
                    snprintf(line, sizeof line,
                             "{\"ts\":\"%s\",\"query\":\"%s\",\"verdict\":\"%s\","
                             "\"approved\":%s,\"reason\":\"%.160s\",\"role\":\"reviewer\"}",
                             ts, eq, rv.verdict, rv.approved ? "true" : "false", ed);
                    append_jsonl(rl, line);
                }
            }
            if (!rv.approved) {
                char br[192];
                snprintf(br, sizeof br, "reviewer_reject:%s", rv.verdict);
                report_skip(&R, q, br);
                continue;
            }
            {
                char merged[128];
                snprintf(merged, sizeof merged, "%.100s+reviewer", reason);
                snprintf(reason, sizeof reason, "%s", merged);
            }
        }

        pattern_for(q, pat);
        if (!promote_via_front_door(&P, q, final, dry)) {
            report_skip(&R, q, "front_door_admission_failed");
            continue;
        }
        pr = report_promote(&R);
        pr->front_door = 1;
        nprom++;
        ids_add(&promoted_ids, sid);
        snprintf(pr->query, sizeof pr->query, "%s", q);
        snprintf(pr->skill_id, sizeof pr->skill_id, "%s", sid);
        snprintf(pr->pattern, sizeof pr->pattern, "%s", pat);
        snprintf(pr->reason, sizeof pr->reason, "%s", reason);
        snprintf(pr->preview, sizeof pr->preview, "%.120s", final);
        if (use_reviewer && rv.verdict[0]) {
            pr->has_review = 1;
            snprintf(pr->verdict, sizeof pr->verdict, "%s", rv.verdict);
        }
        {
            char eq[QMAX * 2], pl[PATHMAX];
            json_escape(q, eq, sizeof eq);
            if (spath(pl, sizeof pl, "%s/evolve_promotes.jsonl", P.packs) == 0) {
                snprintf(line, sizeof line,
                         "{\"ts\":\"%s\",\"query\":\"%s\",\"skill_id\":\"%s\","
                         "\"reason\":\"%s\",\"dry_run\":%s,\"reviewer\":\"%s\"}",
                         ts, eq, sid, reason, dry ? "true" : "false",
                         pr->has_review ? pr->verdict : "");
                append_jsonl(pl, line);
            }
        }
    }

    if (!dry) state_save(P.state, &promoted_ids, ticks + 1, ts);
    write_report(&P, &R, ts, miss_rows, C.n, stable_n, use_reviewer, dry);

    printf("evolve_tick packs=%s\n", P.packs);
    printf("  miss_rows=%ld clusters=%d\n", miss_rows, C.n);
    printf("  promoted=%d skipped=%d\n", R.np, R.ns);
    printf("  reviewer_enabled=%s\n", use_reviewer ? "True" : "False");
    for (i = 0; i < R.np && i < 10; i++)
        printf("  + %s reason=%s q=%.50s\n", R.p[i].skill_id, R.p[i].reason, R.p[i].query);
    printf("  report -> %s\n", P.report);
    if (!dry && is_exec("bin/cnet_ood_numeral_tick")) {
        char cmd[PATHMAX * 4], mout[2048], mm[PATHMAX * 2], pr[PATHMAX * 2],
            ov[PATHMAX * 2], prop[PATHMAX], ovp[PATHMAX];
        mout[0] = 0;
        if (spath(prop, sizeof prop, "%s/en_numerals_propose.tsv", P.packs) == 0 &&
            spath(ovp, sizeof ovp, "%s/en_numerals.tsv", P.packs) == 0 &&
            sh_quote(P.miss, mm, sizeof mm) == 0 &&
            sh_quote(prop, pr, sizeof pr) == 0 &&
            sh_quote(ovp, ov, sizeof ov) == 0 &&
            snprintf(cmd, sizeof cmd,
                     "bin/cnet_ood_numeral_tick --miss %s --propose %s --overlay %s",
                     mm, pr, ov) < (int)sizeof cmd) {
            (void)run_capture(cmd, mout, sizeof mout);
            if (strstr(mout, "numeral_tick"))
                printf("  %s", mout);
        }
    }
    if (!dry && is_exec("bin/cnet_te_teacher_tick") &&
        getenv("CNET_HELD_MODEL_ENDPOINT") &&
        getenv("CNET_HELD_MODEL_ENDPOINT")[0]) {
        char cmd[PATHMAX * 4], mout[2048], pr[PATHMAX * 2], prop[PATHMAX];
        mout[0] = 0;
        if (spath(prop, sizeof prop, "%s/en_irregular_propose.tsv", P.packs) ==
                0 &&
            sh_quote(prop, pr, sizeof pr) == 0 &&
            snprintf(cmd, sizeof cmd,
                     "bin/cnet_te_teacher_tick --lemmas config/en_teacher_lemmas.txt "
                     "--propose %s",
                     pr) < (int)sizeof cmd) {
            (void)run_capture(cmd, mout, sizeof mout);
            if (strstr(mout, "te_teacher_tick"))
                printf("  %s", mout);
        }
    }
    printf("ROE_EVOLVE_TICK_PASS\n");
    if (R.np == 0 && !demo)
        printf("  (idle - no eligible promotes; drop gold or repeat misses)\n");
    for (i = 0; i < C.n; i++) {
        for (int j = 0; j < C.v[i].nans; j++) free(C.v[i].ans[j].ans);
        free(C.v[i].ans);
    }
    for (i = 0; i < promoted_ids.n; i++) free(promoted_ids.v[i]);
    free(promoted_ids.v); free(C.v); free(rules.v); free(R.p); free(R.s);
    return 0;
}
