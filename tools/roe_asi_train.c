/* Train ROE-ASI concept assistant and print economics.
 * make roe_asi_train → ROE_ASI_TRAIN_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_roe_asi.h"

static void seed(RoeAsi *R) {
    roe_init(R);
    roe_add_skill(R, "greet", "greet", "hello",
                  "Hello. ROE-ASI local.", 0, 1);
    roe_add_skill(R, "identity", "identity", "who are you",
                  "I am ROE-ASI under a fail-closed shell.", 0, 1);
    roe_add_skill(R, "math", "math_add", "2+2", "4", 0, 1);

    roe_add_lookup(R, "cnet", "CNET builds Artificial Specialized Intelligence.");
    roe_add_lookup(R, "capsule", "A capsule is a portable CERT skill package.");
    roe_add_lookup(R, "abstain", "Abstain outside coverage; escalate on miss.");

    roe_add_teach(R, "roe-asi", "roe_def",
                  "Regulated open-ended ASI: local skills first, LLM only on miss.");
    roe_add_teach(R, "token", "token_cost",
                  "Repeated questions should become free local skills.");
    roe_add_teach(R, "shell", "shell_law",
                  "Shell admits competence; LLM never self-CERTs.");
    roe_add_teach(R, "lookup online", "lookup_policy",
                  "Lookup is cheaper than LLM and still untrusted until verify.");
}

int main(void) {
  /* HEAP, NOT STACK: this struct exceeds the 2 MB MinGW stack reserve
   * (RoeAsi 3.01 MB, RoeDebug 4.15 MB, RoeOcr 6.15 MB since ROE_ANSWER_MAX
   * went 512 -> 4096 in 85c433e and is embedded 640x). A stack instance
   * dies inside ___chkstk_ms in the prologue, before any statement runs. */
    RoeAsi *R = (RoeAsi *)calloc(1, sizeof *R);
    RoeTrainReport tr;
    char stats[640];
    int e, total_prom = 0;
    const char *batch[] = {
        "hello",
        "who are you",
        "what is 2+2",
        "explain roe-asi",
        "token savings idea",
        "what does the shell law say",
        "lookup online policy",
        "what is cnet",
        "what is a capsule",
        "when to abstain",
        "explain roe-asi",
        "token savings idea",
        "what does the shell law say",
        "lookup online policy",
        "hello",
        "explain roe-asi",
        "token savings idea",
        "what does the shell law say",
        "what is cnet",
        "what is a capsule",
        "who are you",
        "what is 2+2",
        "when to abstain",
        "explain roe-asi",
        "token savings idea",
        "unknown never taught zzzqqq",
        "hello",
        "explain roe-asi",
        "token savings idea",
        "what does the shell law say",
    };
    int n = (int)(sizeof batch / sizeof batch[0]);

    printf("=== ROE-ASI train ===\n");
    seed(R);

    for (e = 1; e <= 3; e++) {
        roe_reset_stats(R);
        roe_train_epoch(R, batch, n, &tr);
        total_prom += tr.promotes;
        printf("epoch %d: hit=%.1f%% save=%.1f%% promotes=%d tok=%llu/%llu skills=%zu\n",
               e, 100.0 * tr.local_hit_rate, 100.0 * tr.token_save_ratio, tr.promotes,
               (unsigned long long)tr.tokens_used,
               (unsigned long long)tr.tokens_baseline, R->n_skills);
    }

    roe_dump_stats(R, stats, sizeof stats);
    printf("final_skills=%zu total_promotes_logged=%d\n", R->n_skills, total_prom);
    printf("stats: %s\n", stats);

    /* Demo turns after train */
    {
        const char *demo[] = {"hello", "explain roe-asi", "token savings idea",
                              "mystery unknown"};
        int i;
        printf("-- demo --\n");
        for (i = 0; i < 4; i++) {
            RoeReply rep;
            roe_turn(R, demo[i], &rep);
            printf("  Q: %s\n  A[%d]: %s\n", demo[i], rep.source, rep.answer);
        }
    }

    if (R->n_skills < 4) {
        printf("ROE_ASI_TRAIN_FAIL skills\n");
        return 1;
    }
    if (tr.token_save_ratio < 0.2) {
        printf("ROE_ASI_TRAIN_FAIL save\n");
        return 1;
    }
    printf("ROE_ASI_TRAIN_PASS\n");
    return 0;
}
