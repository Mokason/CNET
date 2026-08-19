/* seal_trust unit gate — domain LCB refuse + explore + seed + cold-start math.
 * make seal_trust → SEAL_TRUST_PASS
 *
 * Mechanism citation: Conformal Risk Control (Angelopoulos, Bates et al.) for
 * threshold-a-risk-estimate-and-refuse; local object is Wilson binomial LCB.
 */
#include "cnet_seal_trust.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    }
}

static int count_journal_kind(const char *path, const char *kind) {
    FILE *f;
    char line[512];
    int n = 0;
    char needle[80];
    if (!path || !kind) return -1;
    snprintf(needle, sizeof needle, "\"kind\":\"%s\"", kind);
    f = fopen(path, "r");
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, needle)) n++;
    }
    fclose(f);
    return n;
}

int main(void) {
    CnetSealTrust st;
    CnetSealTrustConfig cfg;
    char path[] = "/tmp/cnet-seal-trust-XXXXXX";
    char jpath[] = "/tmp/cnet-seal-journal-XXXXXX";
    int fd, jfd;
    double lcb;
    int i, explores = 0, refuses = 0;
    int explores5 = 0, refuses5 = 0;
    CnetSealTrustDecision d;
    size_t n95, n90, n95_one_err;

    fails = 0;

    /* ---- cold-start math (ops pricing) ---- */
    n95 = cnet_seal_trust_min_n_all_correct(0.95, 1.96);
    n90 = cnet_seal_trust_min_n_all_correct(0.90, 1.96);
    check(n95 >= 70 && n95 <= 80, "min n all-correct for LCB>=0.95 ~73");
    check(n90 >= 30 && n90 <= 40, "min n all-correct for LCB>=0.90 ~35");
    /* one error: find smallest n with (n-1)/n correct clearing 0.95 — hard;
       just show n95 pure is already expensive */
    {
        size_t n;
        n95_one_err = 0;
        for (n = 2; n < 500; n++) {
            /* LCB of (n-1) correct of n */
            CnetSealTrust tmp;
            CnetSealTrustConfig c;
            cnet_seal_trust_config_defaults(&c);
            c.min_lcb = 0.95;
            c.explore_rate = 0;
            cnet_seal_trust_open(&tmp, NULL, &c);
            cnet_seal_trust_seed_prior(&tmp, "e", n - 1, n);
            if (cnet_seal_trust_lcb(&tmp, "e") >= 0.95) {
                n95_one_err = n;
                cnet_seal_trust_close(&tmp);
                break;
            }
            cnet_seal_trust_close(&tmp);
        }
        check(n95_one_err > n95, "one error requires more n than all-correct");
        check(n95_one_err >= 100 && n95_one_err <= 130,
              "one-error n for 0.95 roughly ~110");
    }
    printf("cold_start min_n_all_correct 0.95=%zu 0.90=%zu one_err_0.95=%zu\n",
           n95, n90, n95_one_err);

    cnet_seal_trust_config_defaults(&cfg);
        cfg.min_lcb = 0.90; /* slightly looser for finite samples in unit test */
        cfg.wilson_z = 1.96;
        cfg.explore_rate = 0.0; /* off for first block */
        cfg.explore_warmup_rate = 0.0;
        cfg.min_live_outcomes = 10;

    check(cnet_seal_trust_open(&st, NULL, &cfg) == 0, "open empty");
    check(cnet_seal_trust_lcb(&st, "dom_a") == 0.0, "cold LCB is 0");
    check(cnet_seal_trust_decide(&st, "dom_a") == CNET_SEAL_TRUST_REFUSE,
          "cold start refuses");
    check(cnet_seal_trust_decide(&st, "") == CNET_SEAL_TRUST_REFUSE,
          "empty domain refuses");
    check(cnet_seal_trust_decide(&st, "bad domain!") == CNET_SEAL_TRUST_REFUSE,
          "invalid domain refuses");

    /* n=4 all correct: LCB still low under 0.90 */
    for (i = 0; i < 4; i++) {
        check(cnet_seal_trust_note_seal(&st, "sparse", 0) == 0, "note seal sparse");
        check(cnet_seal_trust_note_outcome(&st, "sparse", 1) == 0, "outcome ok");
    }
    lcb = cnet_seal_trust_lcb(&st, "sparse");
    check(lcb < cfg.min_lcb, "n=4 all-correct still below 0.90 LCB bar");
    check(fabs(lcb - 0.5101) < 0.05 || lcb < 0.70,
          "n=4 LCB in refuse-worthy band");
    check(cnet_seal_trust_decide(&st, "sparse") == CNET_SEAL_TRUST_REFUSE,
          "sparse high point-est still refuses on LCB");

    /* seed_prior: cheap cold-start from acquire/certify-shaped counts.
     * LCB may clear, but prior ALONE must not ALLOW (min_live gate). */
    check(cnet_seal_trust_seed_prior(&st, "from_acquire", 100, 100) == 0,
          "seed prior 100/100");
    check(cnet_seal_trust_lcb(&st, "from_acquire") >= 0.90,
          "seeded prior LCB clears 0.90");
    {
        CnetSealTrustDecision pd =
            cnet_seal_trust_decide(&st, "from_acquire");
        check(pd != CNET_SEAL_TRUST_ALLOW,
              "prior alone never ALLOW (min_live)");
        check(pd == CNET_SEAL_TRUST_REFUSE || pd == CNET_SEAL_TRUST_EXPLORE,
              "prior-only is refuse or explore");
    }
    {
        const CnetSealDomainRow *r = cnet_seal_trust_find(&st, "from_acquire");
        check(r && r->prior_seeded == 100, "prior_seeded counter");
        check(r && r->live_outcomes == 0, "seed is not live");
        check(r && r->trust_seals == 0, "seed is not a live trust seal");
    }
    /* after min_live live outcomes + good LCB → ALLOW */
    cfg.min_live_outcomes = 10;
    st.cfg.min_live_outcomes = 10;
    st.cfg.explore_rate = 0;
    st.cfg.explore_warmup_rate = 0;
    for (i = 0; i < 10; i++) {
        (void)cnet_seal_trust_note_seal(&st, "from_acquire", 1);
        (void)cnet_seal_trust_note_outcome(&st, "from_acquire", 1);
    }
    check(cnet_seal_trust_decide(&st, "from_acquire") == CNET_SEAL_TRUST_ALLOW,
          "prior + min_live live allows");

    /* large correct history clears bar without seed */
    for (i = 0; i < 200; i++) {
        (void)cnet_seal_trust_note_seal(&st, "mature", 0);
        (void)cnet_seal_trust_note_outcome(&st, "mature", 1);
    }
    lcb = cnet_seal_trust_lcb(&st, "mature");
    check(lcb >= cfg.min_lcb, "mature domain LCB clears bar");
    check(cnet_seal_trust_decide(&st, "mature") == CNET_SEAL_TRUST_ALLOW,
          "mature allows");
    {
        const CnetSealDomainRow *r = cnet_seal_trust_find(&st, "mature");
        check(r && r->trust_seals == 200, "trust_seals counted separately");
        check(r && r->live_outcomes == 200, "live_outcomes from note_outcome");
    }

    /* bad domain */
    for (i = 0; i < 50; i++) {
        (void)cnet_seal_trust_note_seal(&st, "poison", 0);
        (void)cnet_seal_trust_note_outcome(&st, "poison", i < 10 ? 1 : 0);
    }
    check(cnet_seal_trust_decide(&st, "poison") == CNET_SEAL_TRUST_REFUSE,
          "poison domain refuses");

    /* ---- explore at SHIPPED 5% + journal kind labels ---- */
    cnet_seal_trust_close(&st);
    cnet_seal_trust_config_defaults(&cfg);
    cfg.min_lcb = 0.95;
    cfg.explore_rate = 0.05; /* shipped default */
    cfg.explore_warmup_rate = 0.0; /* isolate steady 5% */
    cfg.explore_warmup_until_n = 0;
    jfd = mkstemp(jpath);
    check(jfd >= 0, "journal tmp");
    if (jfd >= 0) close(jfd);
    check(cnet_seal_trust_open(&st, NULL, &cfg) == 0, "open explore5");
    cnet_seal_trust_set_journal(&st, jpath);
    for (i = 0; i < 200; i++) {
        d = cnet_seal_trust_decide(&st, "ex5");
        if (d == CNET_SEAL_TRUST_EXPLORE) {
            explores5++;
            (void)cnet_seal_trust_note_seal(&st, "ex5", 1);
            (void)cnet_seal_trust_note_outcome(&st, "ex5", 1);
        } else if (d == CNET_SEAL_TRUST_REFUSE) {
            refuses5++;
        } else if (d == CNET_SEAL_TRUST_ALLOW) {
            fails++;
            fprintf(stderr, "FAIL: ALLOW under cold 5%% explore\n");
        }
    }
    check(explores5 >= 5 && explores5 <= 25,
          "shipped 5% explore fires ~10/200 (±)");
    check(refuses5 > explores5, "5%: most still refuse");
    {
        int jk = count_journal_kind(jpath, "explore");
        int jt = count_journal_kind(jpath, "trust");
        check(jk == explores5, "journal labels every explore seal");
        check(jt == 0, "no trust seals while cold under refuse");
    }
    unlink(jpath);

    /* ---- explore at 20% (stress path) + warmup ---- */
    cnet_seal_trust_close(&st);
    cnet_seal_trust_config_defaults(&cfg);
    cfg.min_lcb = 0.95;
    cfg.explore_rate = 0.20;
    cfg.explore_warmup_rate = 0.0;
    check(cnet_seal_trust_open(&st, NULL, &cfg) == 0, "reopen explore20");
    explores = refuses = 0;
    for (i = 0; i < 100; i++) {
        d = cnet_seal_trust_decide(&st, "explore_dom");
        if (d == CNET_SEAL_TRUST_EXPLORE) {
            explores++;
            (void)cnet_seal_trust_note_seal(&st, "explore_dom", 1);
            (void)cnet_seal_trust_note_outcome(&st, "explore_dom", (i & 1));
        } else if (d == CNET_SEAL_TRUST_REFUSE) {
            refuses++;
        } else if (d == CNET_SEAL_TRUST_ALLOW) {
            fails++;
            fprintf(stderr, "FAIL: unexpected ALLOW during explore phase\n");
        }
    }
    check(explores > 0, "20% exploration fires some seals");
    check(refuses > explores, "most decisions still refuse when bad/cold");
    {
        const CnetSealDomainRow *r = cnet_seal_trust_find(&st, "explore_dom");
        check(r && r->seals == (uint64_t)explores, "outcomes match explores");
        check(r && r->explores == (uint64_t)explores, "explore counter");
        check(r && r->trust_seals == 0, "explore path never increments trust");
    }

    /* warmup: higher explore while n < warmup_until */
    cnet_seal_trust_close(&st);
    cnet_seal_trust_config_defaults(&cfg);
    cfg.min_lcb = 0.95;
    cfg.explore_rate = 0.05;
    cfg.explore_warmup_rate = 0.50;
    cfg.explore_warmup_until_n = 1000; /* stay in warmup for this block */
    check(cnet_seal_trust_open(&st, NULL, &cfg) == 0, "warmup open");
    {
        int ew = 0;
        for (i = 0; i < 40; i++) {
            if (cnet_seal_trust_decide(&st, "warm") == CNET_SEAL_TRUST_EXPLORE)
                ew++;
        }
        check(ew >= 12, "warmup explore rate >> steady 5%");
    }

    /* persist v2 */
    fd = mkstemp(path);
    check(fd >= 0, "tmp path");
    if (fd >= 0) close(fd);
    check(cnet_seal_trust_seed_prior(&st, "persist_dom", 80, 80) == 0, "seed");
    check(cnet_seal_trust_save(&st, path) == 0, "save");
    {
        CnetSealTrust st2;
        const CnetSealDomainRow *r;
        check(cnet_seal_trust_open(&st2, path, &cfg) == 0, "reload");
        r = cnet_seal_trust_find(&st2, "persist_dom");
        check(r && r->seals == 80 && r->prior_seeded == 80, "reloaded prior");
        cnet_seal_trust_close(&st2);
    }
    unlink(path);

    /* disabled always allow */
    cnet_seal_trust_close(&st);
    cnet_seal_trust_config_defaults(&cfg);
    cfg.enabled = 0;
    check(cnet_seal_trust_open(&st, NULL, &cfg) == 0, "disabled open");
    check(cnet_seal_trust_decide(&st, "anything") == CNET_SEAL_TRUST_ALLOW,
          "disabled allows");
    cnet_seal_trust_close(&st);

    if (fails) {
        fprintf(stderr, "SEAL_TRUST_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("SEAL_TRUST_PASS checks_ok explore5=%d/%d explore20=%d/%d "
           "min_n95=%zu min_n90=%zu one_err95=%zu\n",
           explores5, explores5 + refuses5, explores, explores + refuses, n95,
           n90, n95_one_err);
    return 0;
}
