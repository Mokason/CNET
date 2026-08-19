#include "cnet_distrust.h"
#include "cnet_live_miss.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void jwrite(const char *path, const char *line) {
    FILE *f;
    if (!path || !path[0] || !line) return;
    f = fopen(path, "a");
    if (!f) return;
    fputs(line, f);
    if (line[0] && line[strlen(line) - 1] != '\n') fputc('\n', f);
    fclose(f);
}

static int path_mkdir(const char *path) {
    if (!path || !path[0]) return -1;
    if (mkdir(path, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void copy_dom(char *dst, size_t cap, const char *src) {
    size_t n;
    if (!dst || !cap) return;
    dst[0] = '\0';
    if (!src) return;
    n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

CnetDistrustReroute cnet_distrust_reroute(CnetSealTrust *st,
                                          const char *source_domain,
                                          const char *target_domain,
                                          int residual_explicit,
                                          const char *journal_path) {
    CnetSealTrustDecision td;
    char line[384];
    if (!st || !target_domain || !target_domain[0])
        return CNET_DISTRUST_REROUTE_ERROR;

    if (residual_explicit) {
        snprintf(line, sizeof line,
                 "{\"event\":\"reroute\",\"source\":\"%s\",\"target\":\"%s\","
                 "\"kind\":\"residual\",\"result\":\"residual\"}\n",
                 source_domain ? source_domain : "", target_domain);
        jwrite(journal_path, line);
        return CNET_DISTRUST_REROUTE_RESIDUAL;
    }

    td = cnet_seal_trust_decide(st, target_domain);
    if (td == CNET_SEAL_TRUST_ERROR) return CNET_DISTRUST_REROUTE_ERROR;
    if (td == CNET_SEAL_TRUST_ALLOW) {
        snprintf(line, sizeof line,
                 "{\"event\":\"reroute\",\"source\":\"%s\",\"target\":\"%s\","
                 "\"kind\":\"trust\",\"result\":\"allow\"}\n",
                 source_domain ? source_domain : "", target_domain);
        jwrite(journal_path, line);
        return CNET_DISTRUST_REROUTE_ALLOW;
    }
    if (td == CNET_SEAL_TRUST_EXPLORE) {
        snprintf(line, sizeof line,
                 "{\"event\":\"reroute\",\"source\":\"%s\",\"target\":\"%s\","
                 "\"kind\":\"explore\",\"result\":\"explore\"}\n",
                 source_domain ? source_domain : "", target_domain);
        jwrite(journal_path, line);
        return CNET_DISTRUST_REROUTE_EXPLORE;
    }

    snprintf(line, sizeof line,
             "{\"event\":\"reroute\",\"source\":\"%s\",\"target\":\"%s\","
             "\"kind\":\"refuse\",\"result\":\"blocked\"}\n",
             source_domain ? source_domain : "", target_domain);
    jwrite(journal_path, line);
    return CNET_DISTRUST_REROUTE_BLOCKED;
}

void cnet_distrust_scope_init(CnetDistrustScopeTable *t, const char *path) {
    if (!t) return;
    memset(t, 0, sizeof *t);
    if (path && path[0]) {
        size_t n = strlen(path);
        if (n >= sizeof t->path) n = sizeof t->path - 1;
        memcpy(t->path, path, n);
        t->path[n] = '\0';
    }
}

static CnetDistrustScopeRow *scope_find(CnetDistrustScopeTable *t,
                                        const char *domain, int create) {
    size_t i;
    if (!t || !domain || !domain[0]) return NULL;
    for (i = 0; i < t->n; i++) {
        if (strcmp(t->rows[i].domain, domain) == 0) return &t->rows[i];
    }
    if (!create || t->n >= CNET_DISTRUST_SCOPE_MAX) return NULL;
    copy_dom(t->rows[t->n].domain, sizeof t->rows[t->n].domain, domain);
    return &t->rows[t->n++];
}

int cnet_distrust_scope_out(CnetDistrustScopeTable *t, const char *domain,
                            const char *admit_token, uint64_t probe_every_n) {
    CnetDistrustScopeRow *r;
    if (!t || !domain || !domain[0]) return -1;
    if (!admit_token || !admit_token[0]) return -2;
    if (probe_every_n < 1) return -3;
    r = scope_find(t, domain, 1);
    if (!r) return -4;
    r->admitted = 1;
    r->probe_every_n = probe_every_n;
    r->decides_since_probe = 0;
    r->active = 1;
    {
        char line[256];
        snprintf(line, sizeof line,
                 "{\"event\":\"scope_out\",\"domain\":\"%s\",\"probe_every\":%llu,"
                 "\"admit\":1}\n",
                 domain, (unsigned long long)probe_every_n);
        jwrite(t->path, line);
    }
    return 0;
}

int cnet_distrust_scope_is_active(const CnetDistrustScopeTable *t,
                                  const char *domain) {
    size_t i;
    if (!t || !domain) return 0;
    for (i = 0; i < t->n; i++) {
        if (strcmp(t->rows[i].domain, domain) == 0)
            return t->rows[i].active != 0;
    }
    return 0;
}

int cnet_distrust_scope_may_decide(CnetDistrustScopeTable *t,
                                   const char *domain) {
    CnetDistrustScopeRow *r;
    if (!t || !domain) return 1;
    r = scope_find(t, domain, 0);
    if (!r || !r->active) return 1;
    r->decides_since_probe++;
    if (r->decides_since_probe >= r->probe_every_n) {
        r->decides_since_probe = 0;
        {
            char line[200];
            snprintf(line, sizeof line,
                     "{\"event\":\"scope_probe\",\"domain\":\"%s\"}\n", domain);
            jwrite(t->path, line);
        }
        return 1;
    }
    return 0;
}

int cnet_distrust_scope_note_probe(CnetDistrustScopeTable *t, const char *domain,
                                   int correct,
                                   uint64_t recover_streak_needed) {
    CnetDistrustScopeRow *r;
    static uint64_t streak[CNET_DISTRUST_SCOPE_MAX];
    size_t idx = 0;
    size_t i;
    if (!t || !domain) return -1;
    r = scope_find(t, domain, 0);
    if (!r || !r->active) return 0;
    if (recover_streak_needed < 1) recover_streak_needed = 1;
    for (i = 0; i < t->n; i++) {
        if (&t->rows[i] == r) {
            idx = i;
            break;
        }
    }
    if (correct) {
        streak[idx]++;
        if (streak[idx] >= recover_streak_needed) {
            r->active = 0;
            streak[idx] = 0;
            {
                char line[200];
                snprintf(line, sizeof line,
                         "{\"event\":\"scope_reverse\",\"domain\":\"%s\"}\n",
                         domain);
                jwrite(t->path, line);
            }
            return 1;
        }
    } else {
        streak[idx] = 0;
    }
    return 0;
}

static void ensure_dirs(const char *workdir) {
    char b[600];
    if (!workdir) return;
    (void)path_mkdir(workdir);
    snprintf(b, sizeof b, "%s/bricks", workdir);
    (void)path_mkdir(b);
}

int cnet_autonomy_tick(const char *workdir, const char *turn, CnetSealTrust *st,
                       CnetAutonomyTickResult *out) {
    char miss_path[600], bricks[600], ledger[600], journal[600];
    char tag[CNET_DISTRUST_DOM_MAX];
    unsigned in_n = 0, out_n = 0;
    CnetSealTrust local;
    CnetSealTrust *use = st;
    int own = 0;
    CnetSealTrustConfig cfg;
    int harvested = 0;

    if (out) memset(out, 0, sizeof *out);
    if (!workdir || !workdir[0]) return -1;

    ensure_dirs(workdir);
    snprintf(miss_path, sizeof miss_path, "%s/miss.jsonl", workdir);
    snprintf(bricks, sizeof bricks, "%s/bricks", workdir);
    snprintf(ledger, sizeof ledger, "%s/seal_ledger", workdir);
    snprintf(journal, sizeof journal, "%s/seal_journal.jsonl", workdir);

    if (!use) {
        cnet_seal_trust_config_defaults(&cfg);
        cfg.min_lcb = 0.90;
        cfg.explore_rate = 0.0;
        cfg.explore_warmup_rate = 0.0;
        cfg.min_live_outcomes = 3;
        if (cnet_seal_trust_open(&local, ledger, &cfg) != 0) return -2;
        cnet_seal_trust_set_journal(&local, journal);
        (void)cnet_seal_trust_load(&local, ledger);
        use = &local;
        own = 1;
    }

    if (turn && turn[0]) {
        if (cnet_live_parse_teach(turn, tag, sizeof tag, &in_n, &out_n) == 0) {
            if (out) copy_dom(out->domain, sizeof out->domain, tag);
            if (cnet_live_miss_append(miss_path, tag, in_n, 1, out_n) == 0) {
                if (out) out->miss_rows_appended++;
            }
            /* Structured teach only — never residual prose. */
            if (cnet_seal_trust_note_seal(use, tag, 0) == 0 &&
                cnet_seal_trust_note_outcome(use, tag, 1) == 0) {
                if (out) out->admitted = 1;
            }
            if (cnet_live_miss_queue_goal(bricks, tag, in_n) == 0 && out)
                out->goals_queued++;
        } else if (cnet_live_parse_tag_n(turn, tag, sizeof tag, &in_n) == 0) {
            if (out) copy_dom(out->domain, sizeof out->domain, tag);
            if (cnet_live_miss_append(miss_path, tag, in_n, 0, 0) == 0 && out)
                out->miss_rows_appended++;
            if (cnet_live_miss_queue_goal(bricks, tag, in_n) == 0 && out)
                out->goals_queued++;
        } else {
            harvested = cnet_live_miss_harvest_turn(miss_path, turn);
            if (harvested > 0 && out) out->miss_rows_appended += harvested;
            /* Freeform / chain residual cannot mint CERT. */
            if (out) {
                out->residual_rejected = 1;
                if (out->domain[0] == '\0')
                    copy_dom(out->domain, sizeof out->domain, "residual");
            }
        }
    }

    if (out) {
        if (out->domain[0] && strcmp(out->domain, "residual") != 0)
            out->seal_decision = cnet_seal_trust_decide(use, out->domain);
        else
            out->seal_decision = CNET_SEAL_TRUST_REFUSE;
        if (out->admitted)
            snprintf(out->detail, sizeof out->detail, "admitted_teach");
        else if (out->residual_rejected)
            snprintf(out->detail, sizeof out->detail, "residual_no_cert");
        else if (out->goals_queued)
            snprintf(out->detail, sizeof out->detail, "goal_queued");
        else
            snprintf(out->detail, sizeof out->detail, "idle");
    }

    if (own) {
        (void)cnet_seal_trust_save(use, ledger);
        cnet_seal_trust_close(use);
    }
    return 0;
}
