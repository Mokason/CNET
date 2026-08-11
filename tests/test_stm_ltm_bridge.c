/* STM/LTM bridge unit gate */
#include "../include/cce/cce_stm_ltm_bridge.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(c, m)                                                          \
    do {                                                                     \
        if (c)                                                               \
            printf("  ok   %s\n", m);                                        \
        else {                                                               \
            printf("  FAIL %s\n", m);                                        \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int fake_cert(const char *q, char *sk, size_t cap, void *ud) {
    (void)ud;
    if (q && strstr(q, "who are you")) {
        snprintf(sk, cap, "soul_who");
        return 1;
    }
    return 0;
}

int main(void) {
    cce_stm_ltm_bridge B;
    cce_stm_ltm_opts o;
    cce_stm_ltm_job jobs[32];
    int n = 0, i, act[256], na = 0;
    int ok_hot = 1;
    char skill[64], line[320];
    cce_recall_status st;

    failures = 0;
    printf("=== stm_ltm bridge tests ===\n");

    cce_stm_ltm_opts_default(&o, 512);
    o.budget.max_tokens = 32;
    o.budget.initial_tokens = 4;
    o.budget.recent_tokens = 16;
    o.async = 1;
    o.simulated_cold_ms = 1.0;
    o.cert_fn = fake_cert;
    o.k_slot = 64;
    o.v_slot = 64;

    CHECK(cce_stm_ltm_open(&B, &o) == CCE_OK, "open");

    for (i = 0; i < 200; i++) {
        if (cce_stm_ltm_hot_append(&B, i, NULL, 0) != CCE_OK) ok_hot = 0;
    }
    CHECK(ok_hot, "200 HOT appends never fail");
    CHECK(B.n_hot_steps == 200, "hot step counter");
    CHECK(B.stm.active_n > 0 && B.stm.active_n <= 32, "STM budgeted");

    CHECK(cce_stm_ltm_pin(&B, 5) == CCE_OK, "pin");
    st = cce_stm_ltm_request_recall(&B, 199, NULL);
    CHECK(st == CCE_RECALL_STM_HIT, "recent pos is STM hit");
    st = cce_stm_ltm_request_recall(&B, 5, NULL);
    CHECK(st == CCE_RECALL_STM_HIT, "pinned pos is STM hit");

    st = cce_stm_ltm_request_recall(&B, 50, "old topic");
    CHECK(st == CCE_RECALL_LTM_QUEUED || st == CCE_RECALL_STM_HIT ||
              st == CCE_RECALL_LTM_READY,
          "cold recall non-blocking");

    st = cce_stm_ltm_cert_probe(&B, "who are you", skill, sizeof skill);
    CHECK(st == CCE_RECALL_CERT_HIT && strcmp(skill, "soul_who") == 0, "cert hit");

    ok_hot = 1;
    for (i = 200; i < 260; i++) {
        if (cce_stm_ltm_hot_append(&B, i, NULL, 0) != CCE_OK) ok_hot = 0;
        (void)cce_stm_ltm_request_recall(&B, i - 180, "ltm");
    }
    CHECK(ok_hot, "HOT continues during LTM queue");

    cce_stm_ltm_sync(&B);
    CHECK(cce_stm_ltm_poll(&B, jobs, 32, &n) == 0, "poll");
    CHECK(B.n_ltm_done + B.n_stm_hits + B.n_cert_hits > 0, "activity");

    CHECK(cce_stm_ltm_stm_active(&B, act, 256, &na) == CCE_OK && na > 0,
          "stm active export");
    CHECK(cce_stm_ltm_format(&B, line, sizeof line) > 0, "format");
    printf("  %s\n", line);

    cce_stm_ltm_close(&B);

    if (failures) {
        printf("STM_LTM_BRIDGE_FAIL failures=%d\n", failures);
        return 1;
    }
    printf("STM_LTM_BRIDGE_PASS\n");
    return 0;
}
