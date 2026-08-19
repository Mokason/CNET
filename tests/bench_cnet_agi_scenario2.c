#include "cnet_agi_scenario2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    CnetAgiScenario2 S;
    CnetAgiScenario2Bench B;
    const char *dir = "agi2_bricks";
    const char *miss = "agi2_miss.jsonl";
    const char *ws = "agi2_workspace.txt";
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    int rc;

    if (!bonsai || !bonsai[0])
        bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";

    printf("== AGI scenario layer 2 (goals/gather/chain/persist) ==\n");
    {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", dir, dir);
        if (system(cmd) != 0) {
            /* ignore */
        }
    }
    unlink(miss);
    unlink(ws);

    cnet_agi2_init(&S, dir, miss, bonsai, ws);
    rc = cnet_agi2_run_episode(&S, &B);

    printf("AGI2 %s turns=%d cert=%d abstain=%d goals_done=%d gather_ask=%d "
           "gather_fill=%d chains_ok=%d persist=%d restore=%d transfer=%d "
           "parrot=%d bricks=%d cert_rate=%.3f goal_rate=%.3f gather_rate=%.3f "
           "chain_rate=%.3f honesty=%.3f ms=%.3f\n",
           B.pass ? "PASS" : "FAIL", B.turns, B.cert_answers, B.abstains,
           B.goals_completed, B.gathers_asked, B.gathers_filled, B.chains_ok,
           B.persists, B.restores, B.transfers, B.parrot_blocks, B.bricks_end,
           B.cert_rate, B.goal_rate, B.gather_fill_rate, B.chain_rate, B.honesty,
           B.ms_total);

    printf("goals_started=%d n_gaps=%d\n", S.goals_started, S.n_gaps);

    cnet_agi2_free(&S);
    if (rc != 0 || !B.pass) return 1;
    printf("CNET_AGI_SCENARIO2_PASS\n");
    printf("layer2=1 goals=1 active_gather=1 chains=1 persist=1 transfer=1 "
           "parrot_mouth=0 residual_auto_cert=0 goal_rate=%.3f chain_rate=%.3f "
           "ms=%.3f\n",
           B.goal_rate, B.chain_rate, B.ms_total);
    return 0;
}
