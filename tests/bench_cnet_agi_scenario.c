#include "cnet_agi_scenario.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    CnetAgiScenario S;
    CnetAgiScenarioBench B;
    const char *dir = "agi_scenario_bricks";
    const char *miss = "agi_scenario_miss.jsonl";
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    int i, rc;

    if (!bonsai || !bonsai[0])
        bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";

    printf("== AGI-type CORE scenario (given-info + value + evolve + compose) ==\n");
    /* clean slate */
    {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", dir, dir);
        if (system(cmd) != 0) { /* best-effort clean */ }
    }
    unlink(miss);

    cnet_agi_scenario_init(&S, dir, miss, bonsai);
    rc = cnet_agi_scenario_run_episode(&S, &B);

    printf("AGI_SCENARIO %s turns=%d cert=%d abstain=%d evolve=%d compose=%d "
           "parrot_block=%d given=%d bricks=%d cert_rate=%.3f honesty=%.3f "
           "ms=%.3f\n",
           B.pass ? "PASS" : "FAIL", B.turns, B.cert_answers, B.abstains,
           B.evolves, B.composes, B.parrot_blocks, B.given_ingests, B.bricks_end,
           B.cert_rate, B.honesty, B.ms_total);
    for (i = 0; i < S.n_turns; ++i) {
        CnetAgiTurnLog *L = &S.turns[i];
        printf("  turn[%02d] p=%d a=%d e=%d c=%d par=%d v=%.2f %s | %s\n", i,
               L->proved, L->abstained, L->evolved, L->composed, L->parrot_blocked,
               L->value, L->detail, L->text);
    }

    cnet_agi_scenario_free(&S);
    if (rc != 0 || !B.pass) return 1;
    printf("CNET_AGI_SCENARIO_PASS\n");
    printf("agi_like=1 given_info=1 value_miss=1 prove_or_abstain=1 "
           "evolve=1 compose=1 parrot_mouth=0 residual_auto_cert=0 "
           "cert_rate=%.3f honesty=%.3f ms=%.3f\n",
           B.cert_rate, B.honesty, B.ms_total);
    return 0;
}
