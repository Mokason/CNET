/* distrust_loop + autonomy_tick gates.
 * make distrust_loop → DISTRUST_LOOP_PASS
 * make autonomy_tick  → AUTONOMY_TICK_PASS
 *
 * Covers plans/cnet_distrust_loop.md:
 *  1) distrust refuse (via seal_trust)
 *  2) reroute blocked when target cold
 *  3) reroute ALLOW/EXPLORE/residual only
 *  4) scope-out needs admit + probe; reverses on recovery
 *  5) autonomy tick: miss→goal→admit teach only; residual never CERT
 *  6) synthetic drift: LCB decays after corruption; refuse→reroute blocked
 */
#include "cnet_distrust.h"
#include "cnet_live_miss.h"
#include "cnet_seal_trust.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void check(int cond, const char *msg) {
    if (!cond) {
        fprintf(stderr, "FAIL: %s\n", msg);
        fails++;
    } else {
        printf("  ok  %s\n", msg);
    }
}

static int file_has(const char *path, const char *needle) {
    FILE *f;
    char line[512];
    int n = 0;
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, needle)) n++;
    }
    fclose(f);
    return n;
}

static void mature_domain(CnetSealTrust *st, const char *dom, int n_live) {
    int i;
    /* seed_prior alone cannot ALLOW — need min_live outcomes */
    cnet_seal_trust_seed_prior(st, dom, 80, 80);
    for (i = 0; i < n_live; i++) {
        cnet_seal_trust_note_seal(st, dom, 0);
        cnet_seal_trust_note_outcome(st, dom, 1);
    }
}

int main(void) {
    CnetSealTrust st;
    CnetSealTrustConfig cfg;
    CnetDistrustScopeTable scope;
    CnetAutonomyTickResult atr;
    char jpath[] = "/tmp/cnet-distrust-j-XXXXXX";
    char spath[] = "/tmp/cnet-distrust-scope-XXXXXX";
    char work[] = "/tmp/cnet-autonomy-tick-XXXXXX";
    int fd;
    CnetDistrustReroute rr;
    int i;

    fails = 0;
    printf("=== distrust_loop + autonomy_tick ===\n");

    cnet_seal_trust_config_defaults(&cfg);
    cfg.min_lcb = 0.90;
    cfg.explore_rate = 0.0;
    cfg.explore_warmup_rate = 0.0;
    cfg.min_live_outcomes = 5;

    check(cnet_seal_trust_open(&st, NULL, &cfg) == 0, "seal open");

    fd = mkstemp(jpath);
    check(fd >= 0, "journal mkstemp");
    if (fd >= 0) close(fd);
    unlink(jpath);
    cnet_seal_trust_set_journal(&st, jpath);

    /* 1) cold source refuses */
    check(cnet_seal_trust_decide(&st, "cold_src") == CNET_SEAL_TRUST_REFUSE,
          "cold source REFUSE");

    /* 2) reroute to cold target blocked */
    rr = cnet_distrust_reroute(&st, "cold_src", "cold_tgt", 0, jpath);
    check(rr == CNET_DISTRUST_REROUTE_BLOCKED, "reroute cold target BLOCKED");
    check(file_has(jpath, "\"result\":\"blocked\"") >= 1, "journal blocked edge");

    /* 3a) residual explicit allowed without target seal */
    rr = cnet_distrust_reroute(&st, "cold_src", "cold_tgt", 1, jpath);
    check(rr == CNET_DISTRUST_REROUTE_RESIDUAL, "explicit residual OK");
    check(file_has(jpath, "\"kind\":\"residual\"") >= 1, "journal residual");

    /* 3b) mature target allows reroute */
    mature_domain(&st, "hot_tgt", 8);
    check(cnet_seal_trust_decide(&st, "hot_tgt") == CNET_SEAL_TRUST_ALLOW,
          "hot target ALLOW");
    rr = cnet_distrust_reroute(&st, "cold_src", "hot_tgt", 0, jpath);
    check(rr == CNET_DISTRUST_REROUTE_ALLOW, "reroute to ALLOW target");

    /* 4) scope-out requires admit + probe */
    fd = mkstemp(spath);
    check(fd >= 0, "scope journal mkstemp");
    if (fd >= 0) close(fd);
    unlink(spath);
    cnet_distrust_scope_init(&scope, spath);
    check(cnet_distrust_scope_out(&scope, "scoped_dom", "", 5) < 0,
          "scope-out rejects empty admit");
    check(cnet_distrust_scope_out(&scope, "scoped_dom", "admit-token", 0) < 0,
          "scope-out rejects probe_every=0");
    check(cnet_distrust_scope_out(&scope, "scoped_dom", "admit-token", 3) == 0,
          "scope-out with admit+probe");
    check(cnet_distrust_scope_is_active(&scope, "scoped_dom"), "scope active");
    check(cnet_distrust_scope_may_decide(&scope, "scoped_dom") == 0,
          "scope blocks non-probe decide #1");
    check(cnet_distrust_scope_may_decide(&scope, "scoped_dom") == 0,
          "scope blocks non-probe decide #2");
    check(cnet_distrust_scope_may_decide(&scope, "scoped_dom") == 1,
          "probe turn allows decide");
    /* reverse on recovery streak */
    check(cnet_distrust_scope_note_probe(&scope, "scoped_dom", 1, 2) == 0,
          "first recover not enough");
    check(cnet_distrust_scope_is_active(&scope, "scoped_dom"), "still scoped");
    check(cnet_distrust_scope_note_probe(&scope, "scoped_dom", 1, 2) == 1,
          "second recover reverses scope");
    check(!cnet_distrust_scope_is_active(&scope, "scoped_dom"),
          "scope reversed");
    check(file_has(spath, "scope_reverse") >= 1, "journal scope_reverse");

    /* 5) synthetic drift: corrupt outcomes → LCB drops → reroute blocked */
    {
        CnetSealTrust drift;
        CnetSealTrustConfig dcfg;
        cnet_seal_trust_config_defaults(&dcfg);
        dcfg.min_lcb = 0.90;
        dcfg.explore_rate = 0.0;
        dcfg.explore_warmup_rate = 0.0;
        dcfg.min_live_outcomes = 5;
        check(cnet_seal_trust_open(&drift, NULL, &dcfg) == 0, "drift open");
        mature_domain(&drift, "drift_dom", 10);
        check(cnet_seal_trust_decide(&drift, "drift_dom") == CNET_SEAL_TRUST_ALLOW,
              "pre-corruption ALLOW");
        /* inject corruption */
        for (i = 0; i < 40; i++) {
            cnet_seal_trust_note_seal(&drift, "drift_dom", 0);
            cnet_seal_trust_note_outcome(&drift, "drift_dom", 0); /* wrong */
        }
        check(cnet_seal_trust_decide(&drift, "drift_dom") == CNET_SEAL_TRUST_REFUSE,
              "post-corruption REFUSE");
        rr = cnet_distrust_reroute(&st, "cold_src", "drift_dom", 0, jpath);
        /* st doesn't have drift_dom — use drift ledger */
        rr = cnet_distrust_reroute(&drift, "cold_src", "drift_dom", 0, jpath);
        check(rr == CNET_DISTRUST_REROUTE_BLOCKED,
              "reroute blocked after drift refuse");
        cnet_seal_trust_close(&drift);
    }

    /* 6) autonomy tick path */
    {
        char *w = mkdtemp(work);
        check(w != NULL, "workdir mkdtemp");
        if (w) {
            /* residual freeform — no CERT */
            check(cnet_autonomy_tick(w, "hello world please invent skill", NULL,
                                     &atr) == 0,
                  "tick residual");
            check(atr.residual_rejected == 1, "residual rejected for CERT");
            check(atr.admitted == 0, "residual not admitted");

            /* query miss → goal, no admit */
            memset(&atr, 0, sizeof atr);
            check(cnet_autonomy_tick(w, "growdom 4", NULL, &atr) == 0,
                  "tick query miss");
            check(atr.goals_queued >= 1, "goal queued");
            check(atr.admitted == 0, "query not admitted");
            {
                char gp[640];
                snprintf(gp, sizeof gp, "%s/bricks/pending_goals.txt", w);
                check(file_has(gp, "prove growdom") >= 1,
                      "pending_goals has prove");
            }

            /* structured teach → admit + live outcomes */
            memset(&atr, 0, sizeof atr);
            check(cnet_autonomy_tick(w, "teach growdom 4 9", NULL, &atr) == 0,
                  "tick teach");
            check(atr.admitted == 1, "teach admitted");
            check(atr.residual_rejected == 0, "teach not residual");
            check(atr.miss_rows_appended >= 1, "teach miss row");

            /* enough teaches to clear min_live for seal ALLOW path */
            for (i = 0; i < 5; i++) {
                char teach[64];
                snprintf(teach, sizeof teach, "teach growdom %d %d", i,
                         (i * 3) & 15);
                cnet_autonomy_tick(w, teach, NULL, &atr);
            }
            check(atr.admitted == 1, "batch teach admitted");
            /* seal_decision may still refuse if LCB not high enough with few
             * samples — that is correct fail-closed. At least not ERROR. */
            check(atr.seal_decision != CNET_SEAL_TRUST_ERROR,
                  "seal decide defined");
        }
    }

    cnet_seal_trust_close(&st);
    unlink(jpath);
    unlink(spath);

    if (fails) {
        printf("DISTRUST_LOOP_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("DISTRUST_LOOP_PASS\n");
    printf("AUTONOMY_TICK_PASS\n");
    return 0;
}
