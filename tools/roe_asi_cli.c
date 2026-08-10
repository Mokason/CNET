/* ROE-ASI serve / live train CLI.
 *   roe_asi_cli train [--live] [--catalog DIR]
 *   roe_asi_cli ask "query" [--live] [--catalog DIR] [--accept]
 *   roe_asi_cli bench [--live] [--catalog DIR]
 * Env: ROE_LIVE=1 ROE_LLM_MODEL=qwen2.5:7b ROE_LLM_URL=...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_roe_asi.h"
#include "../include/cnet_roe_net.h"

static void seed_base(RoeAsi *R) {
    roe_add_skill(R, "greet", "greet", "hello", "Hello. ROE-ASI local skill.", 0, 1);
    roe_add_skill(R, "identity", "identity", "who are you",
                  "I am ROE-ASI: local skills first, escalate on miss.", 0, 1);
    roe_add_skill(R, "math", "math_add", "2+2", "4", 0, 1);
    roe_add_lookup(R, "cnet specialized",
                   "CNET builds Artificial Specialized Intelligence with CERT packs.");
    roe_add_lookup(R, "fail-closed",
                   "Fail-closed means abstain outside certified coverage.");
    roe_add_teach(R, "roe-asi", "roe_def",
                  "ROE-ASI serves certified local skills and only uses LLM/lookup on miss.");
    roe_add_teach(R, "token", "token_cost",
                  "Repeated work should hit local skills and avoid LLM tokens.");
    roe_add_teach(R, "shell", "shell_law",
                  "The shell verifies and admits; the LLM never self-CERTs.");
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage:\n"
            "  %s train [--live] [--catalog DIR]\n"
            "  %s ask \"query\" [--live] [--catalog DIR] [--accept]\n"
            "  %s bench [--live] [--catalog DIR]\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv) {
    RoeAsi R;
    RoeNet net;
    const char *cmd = NULL;
    const char *catalog = "artifacts/roe_catalog";
    const char *ask_q = NULL;
    int live = 0, accept = 0, i;
    char stats[700];

    for (i = 1; i < argc; i++) {
        if (!cmd && argv[i][0] != '-')
            cmd = argv[i];
        else if (!strcmp(argv[i], "--live"))
            live = 1;
        else if (!strcmp(argv[i], "--accept"))
            accept = 1;
        else if (!strcmp(argv[i], "--catalog") && i + 1 < argc)
            catalog = argv[++i];
        else if (!strcmp(argv[i], "ask") ) {
            cmd = "ask";
        } else if (cmd && !strcmp(cmd, "ask") && argv[i][0] != '-' && !ask_q)
            ask_q = argv[i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]);
            return 0;
        }
    }
    if (!cmd) {
        usage(argv[0]);
        return 2;
    }

    roe_init(&R);
    roe_net_init(&net);
    if (live) {
        setenv("ROE_LIVE", "1", 1);
        roe_net_from_env(&net);
        roe_set_net(&R, &net);
        printf("live backends: llm=%s model=%s lookup=%d\n", net.llm_url, net.llm_model,
               net.enable_lookup);
    }
    roe_set_catalog_dir(&R, catalog);
    {
        int loaded = roe_load_catalog(&R);
        printf("catalog_dir=%s loaded=%d\n", catalog, loaded);
    }
    if (R.n_skills == 0) seed_base(&R);

    if (!strcmp(cmd, "train") || !strcmp(cmd, "bench")) {
        const char *batch[] = {
            "hello",
            "who are you",
            "what is 2+2",
            "explain roe-asi",
            "token savings idea",
            "what does the shell law say",
            "cnet specialized intelligence",
            "fail-closed meaning",
            "explain roe-asi",
            "token savings idea",
            "what does the shell law say",
            "hello",
            "explain roe-asi",
            "token savings idea",
            "who are you",
            "what is 2+2",
            "cnet specialized intelligence",
            "fail-closed meaning",
            "explain roe-asi",
            "token savings idea",
            "what does the shell law say",
            "hello",
            "unknown never taught zzqq",
        };
        int n = (int)(sizeof batch / sizeof batch[0]);
        RoeTrainReport tr;
        int e;
        for (e = 1; e <= 3; e++) {
            roe_reset_stats(&R);
            roe_train_epoch(&R, batch, n, &tr);
            printf("epoch %d hit=%.1f%% save=%.1f%% prom=%d tok=%llu/%llu skills=%zu "
                   "live_lu=%llu live_llm=%llu\n",
                   e, 100.0 * tr.local_hit_rate, 100.0 * tr.token_save_ratio, tr.promotes,
                   (unsigned long long)tr.tokens_used,
                   (unsigned long long)tr.tokens_baseline, R.n_skills,
                   (unsigned long long)R.n_live_lookup, (unsigned long long)R.n_live_llm);
        }
        (void)roe_save_catalog(&R);
        roe_dump_stats(&R, stats, sizeof stats);
        printf("saved catalog → %s\n%s\n", catalog, stats);
        if (tr.token_save_ratio < 0.25) {
            printf("ROE_ASI_CLI_FAIL\n");
            return 1;
        }
        printf("ROE_ASI_CLI_PASS\n");
        return 0;
    }

    if (!strcmp(cmd, "ask")) {
        RoeReply rep;
        if (!ask_q) {
            usage(argv[0]);
            return 2;
        }
        roe_turn(&R, ask_q, &rep);
        printf("source=%s(%d) verified=%d tokens=%llu skill=%s\n", rep.source_name,
               rep.source, rep.verified, (unsigned long long)rep.tokens_est,
               rep.skill_id[0] ? rep.skill_id : "-");
        printf("A: %s\n", rep.answer);
        printf("inventory: %s\n", rep.inventory_line);
        if (rep.source == ROE_SRC_LLM || rep.source == ROE_SRC_LOOKUP) {
            int p = roe_feedback_verify(&R, ask_q, NULL, accept);
            printf("verify_promote=%d\n", p);
            if (p) printf("promoted+saved under %s\n", catalog);
        }
        roe_dump_stats(&R, stats, sizeof stats);
        printf("%s\n", stats);
        printf("ROE_ASI_ASK_PASS\n");
        return 0;
    }

    usage(argv[0]);
    return 2;
}
