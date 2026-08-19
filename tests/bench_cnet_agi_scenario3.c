#include "cnet_agi_scenario3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
    CnetAgiScenario3 S;
    CnetAgiScenario3Bench B;
    const char *dir = "agi3_bricks";
    const char *miss = "agi3_miss.jsonl";
    const char *ws = "agi3_workspace.txt";
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    int rc, i;

    if (!bonsai || !bonsai[0])
        bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";

    printf("== AGI scenario layer 3 (plan synthesis + specialists) ==\n");
    {
        char cmd[256];
        snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", dir, dir);
        if (system(cmd) != 0) {
        }
    }
    unlink(miss);
    unlink(ws);

    cnet_agi3_init(&S, dir, miss, bonsai, ws);
    rc = cnet_agi3_run_episode(&S, &B);

    printf("AGI3 %s turns=%d cert=%d abstain=%d synth_ok=%d synth_rej=%d "
           "goals_done=%d routes=%d committee_ok=%d chains=%d parrot=%d "
           "bricks=%d cert_rate=%.3f synth_prec=%.3f committee_rate=%.3f "
           "honesty=%.3f ms=%.3f\n",
           B.pass ? "PASS" : "FAIL", B.turns, B.cert_answers, B.abstains,
           B.synth_ok, B.synth_reject, B.goals_completed, B.specialist_routes,
           B.committee_ok, B.chains_ok, B.parrot_blocks, B.bricks_end,
           B.cert_rate, B.synth_precision, B.committee_rate, B.honesty,
           B.ms_total);

    printf("specialists=%d plans=%d\n", S.n_specialists, S.n_plans);
    for (i = 0; i < S.n_specialists; ++i)
        printf("  sp[%d] tag=%s rel=%.2f consults=%d\n", i, S.specialists[i].tag,
               S.specialists[i].reliability, S.specialists[i].consults);
    for (i = 0; i < S.n_plans; ++i)
        printf("  plan[%d] syn=%d rej=%d ok=%d | %s\n", i, S.plans[i].synthesized,
               S.plans[i].rejected_non_cert, S.plans[i].ok, S.plans[i].raw);

    cnet_agi3_free(&S);
    if (rc != 0 || !B.pass) return 1;
    printf("CNET_AGI_SCENARIO3_PASS\n");
    printf("layer3=1 plan_synth=1 cert_only_plans=1 specialists=1 committee=1 "
           "parrot_mouth=0 residual_auto_cert=0 synth_prec=%.3f ms=%.3f\n",
           B.synth_precision, B.ms_total);
    return 0;
}
