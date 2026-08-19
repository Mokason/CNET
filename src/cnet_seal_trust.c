#include "../include/cnet_seal_trust.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Same Wilson score-interval lower bound as coverage_accuracy_lower_bound
 * (contract/coverage.c). Inlined so the seal_trust gate does not pull the
 * full BTN/specialist link set. Threshold-refusal family: Conformal Risk
 * Control (Angelopoulos, Bates et al.) — we use binomial Wilson LCB, not
 * full conformal nonconformity scores. */
static double seal_wilson_lcb(size_t passed, size_t n, double z) {
    double phat, denom, centre, half, lo;
    if (n == 0) return 0.0;
    if (passed > n) passed = n;
    phat = (double)passed / (double)n;
    denom = 1.0 + (z * z) / (double)n;
    centre = phat + (z * z) / (2.0 * (double)n);
    half = z * sqrt((phat * (1.0 - phat) + (z * z) / (4.0 * (double)n)) /
                    (double)n);
    lo = (centre - half) / denom;
    if (lo < 0.0) lo = 0.0;
    if (lo > 1.0) lo = 1.0;
    return lo;
}

static int domain_ok(const char *d) {
    size_t i;
    if (!d || !d[0] || strlen(d) >= CNET_SEAL_TRUST_DOMAIN_MAX) return 0;
    for (i = 0; d[i]; i++) {
        unsigned char c = (unsigned char)d[i];
        if (!(c == '_' || c == '-' || c == '.' ||
              (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9')))
            return 0;
    }
    return 1;
}

void cnet_seal_trust_config_defaults(CnetSealTrustConfig *c) {
    if (!c) return;
    c->min_lcb = 0.95;
    c->wilson_z = 1.96;
    c->explore_rate = 0.05;
    c->explore_warmup_rate = 0.20; /* faster maturation while cold */
    c->explore_warmup_until_n = 40;
    c->min_live_outcomes = 10; /* prior alone cannot clear ALLOW */
    c->enabled = 1;
}

size_t cnet_seal_trust_min_n_all_correct(double min_lcb, double z) {
    size_t n;
    if (min_lcb <= 0.0) return 0;
    if (min_lcb >= 1.0) return (size_t)-1;
    if (z <= 0.0) z = 1.96;
    for (n = 1; n < 100000; n++) {
        if (seal_wilson_lcb(n, n, z) + 1e-15 >= min_lcb) return n;
    }
    return (size_t)-1;
}

int cnet_seal_trust_open(CnetSealTrust *st, const char *path,
                         const CnetSealTrustConfig *cfg) {
    if (!st) return -1;
    memset(st, 0, sizeof *st);
    if (cfg)
        st->cfg = *cfg;
    else
        cnet_seal_trust_config_defaults(&st->cfg);
    if (st->cfg.min_lcb < 0.0) st->cfg.min_lcb = 0.0;
    if (st->cfg.min_lcb > 1.0) st->cfg.min_lcb = 1.0;
    if (st->cfg.wilson_z <= 0.0) st->cfg.wilson_z = 1.96;
    if (st->cfg.explore_rate < 0.0) st->cfg.explore_rate = 0.0;
    if (st->cfg.explore_rate > 1.0) st->cfg.explore_rate = 1.0;
    if (st->cfg.explore_warmup_rate < 0.0) st->cfg.explore_warmup_rate = 0.0;
    if (st->cfg.explore_warmup_rate > 1.0) st->cfg.explore_warmup_rate = 1.0;
    if (path && path[0]) {
        snprintf(st->path, sizeof st->path, "%s", path);
        if (cnet_seal_trust_load(st, path) != 0) {
            /* missing file is fine — start empty */
            st->n_rows = 0;
        }
    }
    st->loaded = 1;
    return 0;
}

void cnet_seal_trust_set_journal(CnetSealTrust *st, const char *path) {
    if (!st) return;
    if (path && path[0])
        snprintf(st->journal_path, sizeof st->journal_path, "%s", path);
    else
        st->journal_path[0] = 0;
}

void cnet_seal_trust_close(CnetSealTrust *st) {
    if (!st) return;
    if (st->path[0]) (void)cnet_seal_trust_save(st, st->path);
    memset(st, 0, sizeof *st);
}

static CnetSealDomainRow *find_mut(CnetSealTrust *st, const char *domain,
                                   int create) {
    size_t i;
    if (!st || !domain_ok(domain)) return NULL;
    for (i = 0; i < st->n_rows; i++) {
        if (strcmp(st->rows[i].domain, domain) == 0) return &st->rows[i];
    }
    if (!create || st->n_rows >= CNET_SEAL_TRUST_ROWS_MAX) return NULL;
    {
        CnetSealDomainRow *r = &st->rows[st->n_rows++];
        memset(r, 0, sizeof *r);
        snprintf(r->domain, sizeof r->domain, "%s", domain);
        return r;
    }
}

const CnetSealDomainRow *cnet_seal_trust_find(const CnetSealTrust *st,
                                              const char *domain) {
    size_t i;
    if (!st || !domain_ok(domain)) return NULL;
    for (i = 0; i < st->n_rows; i++) {
        if (strcmp(st->rows[i].domain, domain) == 0) return &st->rows[i];
    }
    return NULL;
}

double cnet_seal_trust_lcb(const CnetSealTrust *st, const char *domain) {
    const CnetSealDomainRow *r;
    if (!st || !domain_ok(domain)) return 0.0;
    r = cnet_seal_trust_find(st, domain);
    if (!r || r->seals == 0) return 0.0;
    return seal_wilson_lcb((size_t)r->correct, (size_t)r->seals, st->cfg.wilson_z);
}

static void journal_event(const CnetSealTrust *st, const char *domain,
                          const char *event, const char *kind, int correct) {
    FILE *f;
    time_t now;
    if (!st || !st->journal_path[0] || !domain || !event || !kind) return;
    f = fopen(st->journal_path, "a");
    if (!f) return;
    now = time(NULL);
    if (strcmp(event, "outcome") == 0)
        fprintf(f,
                "{\"ts\":%lld,\"domain\":\"%s\",\"event\":\"%s\",\"kind\":\"%s\","
                "\"correct\":%d}\n",
                (long long)now, domain, event, kind, correct ? 1 : 0);
    else
        fprintf(f,
                "{\"ts\":%lld,\"domain\":\"%s\",\"event\":\"%s\",\"kind\":\"%s\"}\n",
                (long long)now, domain, event, kind);
    fclose(f);
}

/* Deterministic explore at `rate`: every K-th refuse where K≈1/rate. */
static int should_explore_rate(CnetSealTrust *st, const char *domain,
                               double rate) {
    uint64_t k, seq;
    if (!st) return 0;
    if (rate <= 0.0) return 0;
    if (rate >= 1.0) return 1;
    k = (uint64_t)(1.0 / rate + 0.5);
    if (k < 2) k = 2;
    st->decide_seq++;
    seq = st->decide_seq;
    {
        const unsigned char *p = (const unsigned char *)domain;
        uint64_t h = 1469598103934665603ull;
        while (*p) {
            h ^= *p++;
            h *= 1099511628211ull;
        }
        seq ^= h;
    }
    return (seq % k) == 0;
}

static double effective_explore_rate(const CnetSealTrust *st,
                                     const char *domain) {
    const CnetSealDomainRow *r;
    if (!st) return 0.0;
    r = cnet_seal_trust_find(st, domain);
    if (r && r->seals < st->cfg.explore_warmup_until_n &&
        st->cfg.explore_warmup_rate > st->cfg.explore_rate)
        return st->cfg.explore_warmup_rate;
    return st->cfg.explore_rate;
}

CnetSealTrustDecision cnet_seal_trust_decide(CnetSealTrust *st,
                                             const char *domain) {
    double lcb;
    CnetSealDomainRow *r;
    if (!st || !st->loaded) return CNET_SEAL_TRUST_ERROR;
    if (!st->cfg.enabled) return CNET_SEAL_TRUST_ALLOW;
    if (!domain_ok(domain)) return CNET_SEAL_TRUST_REFUSE;

    lcb = cnet_seal_trust_lcb(st, domain);
    if (lcb + 1e-15 >= st->cfg.min_lcb) {
        /* LCB clear is not enough: require live serve/explore outcomes so
         * seed_prior cannot alone convert offline certify into self-trust. */
        const CnetSealDomainRow *rr = cnet_seal_trust_find(st, domain);
        uint64_t live = rr ? rr->live_outcomes : 0;
        if (live >= st->cfg.min_live_outcomes)
            return CNET_SEAL_TRUST_ALLOW;
        /* LCB ok but insufficient live — fall through to refuse/explore */
    }

    /* refuse path — maybe explore (warmup rate while cold) */
    r = find_mut(st, domain, 1);
    if (r) r->refuses++;
    if (should_explore_rate(st, domain, effective_explore_rate(st, domain)))
        return CNET_SEAL_TRUST_EXPLORE;
    return CNET_SEAL_TRUST_REFUSE;
}

int cnet_seal_trust_note_seal(CnetSealTrust *st, const char *domain,
                              int is_explore) {
    CnetSealDomainRow *r;
    if (!st || !st->loaded || !domain_ok(domain)) return -1;
    r = find_mut(st, domain, 1);
    if (!r) return -2;
    r->pending++;
    if (is_explore) {
        r->explores++;
        journal_event(st, domain, "seal", "explore", 0);
    } else {
        r->trust_seals++;
        journal_event(st, domain, "seal", "trust", 0);
    }
    return 0;
}

int cnet_seal_trust_note_outcome(CnetSealTrust *st, const char *domain,
                                 int correct) {
    CnetSealDomainRow *r;
    if (!st || !st->loaded || !domain_ok(domain)) return -1;
    r = find_mut(st, domain, 1);
    if (!r) return -2;
    if (r->pending > 0) r->pending--;
    r->seals++;
    r->live_outcomes++;
    if (correct) r->correct++;
    /* outcome kind unknown without pairing — journal as outcome only */
    journal_event(st, domain, "outcome", "live", correct ? 1 : 0);
    return 0;
}

int cnet_seal_trust_seed_prior(CnetSealTrust *st, const char *domain,
                               size_t correct, size_t n) {
    CnetSealDomainRow *r;
    if (!st || !st->loaded || !domain_ok(domain)) return -1;
    if (n == 0) return 0;
    if (correct > n) correct = n;
    r = find_mut(st, domain, 1);
    if (!r) return -2;
    r->seals += (uint64_t)n;
    r->correct += (uint64_t)correct;
    r->prior_seeded += (uint64_t)n;
    journal_event(st, domain, "seed_prior", "prior", (int)correct);
    /* journal only carries correct flag as int — also log n via second line */
    {
        FILE *f;
        if (st->journal_path[0] && (f = fopen(st->journal_path, "a"))) {
            fprintf(f,
                    "{\"domain\":\"%s\",\"event\":\"seed_prior_n\",\"kind\":\"prior\","
                    "\"correct\":%zu,\"n\":%zu}\n",
                    domain, correct, n);
            fclose(f);
        }
    }
    return 0;
}

int cnet_seal_trust_save(const CnetSealTrust *st, const char *path) {
    FILE *f;
    size_t i;
    if (!st || !path || !path[0]) return -1;
    f = fopen(path, "w");
    if (!f) return -2;
    fprintf(f,
            "# cnet_seal_trust v3 min_lcb=%.6f z=%.4f explore=%.4f "
            "warmup_explore=%.4f warmup_n=%llu min_live=%llu\n",
            st->cfg.min_lcb, st->cfg.wilson_z, st->cfg.explore_rate,
            st->cfg.explore_warmup_rate,
            (unsigned long long)st->cfg.explore_warmup_until_n,
            (unsigned long long)st->cfg.min_live_outcomes);
    fprintf(f, "# domain seals correct explores trust_seals prior_seeded "
               "live_outcomes pending refuses\n");
    for (i = 0; i < st->n_rows; i++) {
        const CnetSealDomainRow *r = &st->rows[i];
        fprintf(f, "%s %llu %llu %llu %llu %llu %llu %llu %llu\n", r->domain,
                (unsigned long long)r->seals, (unsigned long long)r->correct,
                (unsigned long long)r->explores,
                (unsigned long long)r->trust_seals,
                (unsigned long long)r->prior_seeded,
                (unsigned long long)r->live_outcomes,
                (unsigned long long)r->pending,
                (unsigned long long)r->refuses);
    }
    fclose(f);
    return 0;
}

int cnet_seal_trust_load(CnetSealTrust *st, const char *path) {
    FILE *f;
    char line[512];
    if (!st || !path || !path[0]) return -1;
    f = fopen(path, "r");
    if (!f) return -2;
    st->n_rows = 0;
    while (fgets(line, sizeof line, f)) {
        char dom[CNET_SEAL_TRUST_DOMAIN_MAX];
        unsigned long long seals, correct, explores, pending, refuses;
        unsigned long long trust_seals = 0, prior_seeded = 0, live_outcomes = 0;
        CnetSealDomainRow *r;
        int nfield;
        if (line[0] == '#' || line[0] == '\n') continue;
        /* v3: 9 fields; v2: 8 fields; v1: 6 fields */
        nfield = sscanf(line, "%63s %llu %llu %llu %llu %llu %llu %llu %llu",
                        dom, &seals, &correct, &explores, &trust_seals,
                        &prior_seeded, &live_outcomes, &pending, &refuses);
        if (nfield == 9) {
            /* full v3 */
        } else if (sscanf(line, "%63s %llu %llu %llu %llu %llu %llu %llu", dom,
                          &seals, &correct, &explores, &trust_seals,
                          &prior_seeded, &pending, &refuses) == 8) {
            live_outcomes = 0;
        } else if (sscanf(line, "%63s %llu %llu %llu %llu %llu", dom, &seals,
                          &correct, &explores, &pending, &refuses) == 6) {
            trust_seals = 0;
            prior_seeded = 0;
            live_outcomes = 0;
        } else {
            continue;
        }
        if (!domain_ok(dom)) continue;
        r = find_mut(st, dom, 1);
        if (!r) break;
        r->seals = seals;
        r->correct = correct;
        r->explores = explores;
        r->trust_seals = trust_seals;
        r->prior_seeded = prior_seeded;
        r->live_outcomes = live_outcomes;
        r->pending = pending;
        r->refuses = refuses;
    }
    fclose(f);
    return 0;
}

/* ---- process global ----------------------------------------------------- */

static CnetSealTrust g_st;
static int g_st_ready;

CnetSealTrust *cnet_seal_trust_global(void) {
    const char *dis;
    const char *path;
    const char *jpath;
    CnetSealTrustConfig cfg;
    if (g_st_ready) return &g_st;

    cnet_seal_trust_config_defaults(&cfg);
    /* Opt-in: CNET_SEAL_TRUST=1 enables. Default off so existing Tier-A
     * habitats keep working until a ledger + outcomes are wired. */
    cfg.enabled = 0;
    dis = getenv("CNET_SEAL_TRUST");
    if (dis && dis[0] == '1' && dis[1] == '\0') cfg.enabled = 1;
    if (dis && dis[0] == '0' && dis[1] == '\0') cfg.enabled = 0;
    {
        const char *er = getenv("CNET_SEAL_TRUST_EXPLORE");
        const char *lcb = getenv("CNET_SEAL_TRUST_MIN_LCB");
        const char *wr = getenv("CNET_SEAL_TRUST_WARMUP_EXPLORE");
        const char *wn = getenv("CNET_SEAL_TRUST_WARMUP_N");
        const char *ml = getenv("CNET_SEAL_TRUST_MIN_LIVE");
        if (er && er[0]) cfg.explore_rate = strtod(er, NULL);
        if (lcb && lcb[0]) cfg.min_lcb = strtod(lcb, NULL);
        if (wr && wr[0]) cfg.explore_warmup_rate = strtod(wr, NULL);
        if (wn && wn[0]) cfg.explore_warmup_until_n =
                             (uint64_t)strtoull(wn, NULL, 10);
        if (ml && ml[0]) cfg.min_live_outcomes =
                             (uint64_t)strtoull(ml, NULL, 10);
    }
    path = getenv("CNET_SEAL_TRUST_LEDGER");
    (void)cnet_seal_trust_open(&g_st, path, &cfg);
    jpath = getenv("CNET_SEAL_TRUST_JOURNAL");
    if (jpath && jpath[0]) cnet_seal_trust_set_journal(&g_st, jpath);
    g_st_ready = 1;
    return &g_st;
}

void cnet_seal_trust_global_shutdown(void) {
    if (!g_st_ready) return;
    cnet_seal_trust_close(&g_st);
    g_st_ready = 0;
}
