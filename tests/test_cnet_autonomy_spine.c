/* Integration: production-shaped autonomy spine.
 * miss harvest → pending_goals → teach admit → seal decide;
 * distrust reroute law on refuse.
 * make autonomy_spine → AUTONOMY_SPINE_PASS
 */
#include "cnet_distrust.h"
#include "cnet_live_miss.h"
#include "cnet_seal_trust.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fails;

static void check(int c, const char *m) {
    if (!c) {
        fprintf(stderr, "FAIL: %s\n", m);
        fails++;
    } else {
        printf("  ok  %s\n", m);
    }
}

static int file_has(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    char line[512];
    int n = 0;
    if (!f) return 0;
    while (fgets(line, sizeof line, f))
        if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

int main(void) {
    char work[] = "/tmp/cnet-autonomy-spine-XXXXXX";
    char *w;
    char miss[640], bricks[640], goals[640], journal[640];
    CnetAutonomyTickResult atr;
    CnetSealTrust st;
    CnetSealTrustConfig cfg;
    CnetDistrustReroute rr;
    int i;

    fails = 0;
    printf("=== autonomy_spine (production path shape) ===\n");

    w = mkdtemp(work);
    check(w != NULL, "workdir");
    if (!w) return 1;

    snprintf(miss, sizeof miss, "%s/miss.jsonl", w);
    snprintf(bricks, sizeof bricks, "%s/bricks", w);
    snprintf(goals, sizeof goals, "%s/bricks/pending_goals.txt", w);
    snprintf(journal, sizeof journal, "%s/distrust_journal.jsonl", w);

    /* 1) freeform residual never CERT */
    check(cnet_autonomy_tick(w, "please invent a skill about cats", NULL, &atr) ==
              0,
          "residual turn");
    check(atr.residual_rejected == 1 && atr.admitted == 0, "residual no CERT");

    /* 2) query miss → goal queue (unattended prove queue) */
    memset(&atr, 0, sizeof atr);
    check(cnet_autonomy_tick(w, "spine_dom 2", NULL, &atr) == 0, "query miss");
    check(atr.goals_queued >= 1, "goal queued");
    check(file_has(goals, "prove spine_dom") >= 1, "pending_goals on disk");

    /* 3) structured teach admits + live seal outcomes */
    for (i = 0; i < 6; i++) {
        char t[64];
        snprintf(t, sizeof t, "teach spine_dom %d %d", i, (i * 2) & 15);
        check(cnet_autonomy_tick(w, t, NULL, &atr) == 0, i == 0 ? "teach" : "t");
    }
    check(atr.admitted == 1, "teach admitted");
    check(file_has(miss, "typed_miss") >= 1, "miss.jsonl typed rows");

    /* 4) distrust: cold target blocked; residual explicit ok */
    cnet_seal_trust_config_defaults(&cfg);
    cfg.min_lcb = 0.90;
    cfg.explore_rate = 0;
    cfg.explore_warmup_rate = 0;
    cfg.min_live_outcomes = 5;
    check(cnet_seal_trust_open(&st, NULL, &cfg) == 0, "seal open");
    rr = cnet_distrust_reroute(&st, "spine_dom", "cold_alt", 0, journal);
    check(rr == CNET_DISTRUST_REROUTE_BLOCKED, "cold alt blocked");
    rr = cnet_distrust_reroute(&st, "spine_dom", "tier_c_residual", 1, journal);
    check(rr == CNET_DISTRUST_REROUTE_RESIDUAL, "residual path ok");
    check(file_has(journal, "blocked") >= 1, "journal blocked");
    check(file_has(journal, "residual") >= 1, "journal residual");
    cnet_seal_trust_close(&st);

    if (fails) {
        printf("AUTONOMY_SPINE_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("AUTONOMY_SPINE_PASS\n");
    return 0;
}
