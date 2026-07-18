/* CLI: Hermes-style learn cycle in C.
 *
 *   bin/cnet_learn_cycle "Who is Grace Hopper?"
 *   bin/cnet_learn_cycle --seed-gap "Alan Turing"
 *
 * Env: CNET_SKILLS_DIR CNET_GAP_INBOX CNET_LEARN_SEED_GAP
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_learn_loop.h"

int main(int argc, char **argv) {
    CnetLearnConfig cfg;
    CnetLearnReport rep;
    const char *q = NULL;
    int i;

    cnet_learn_config_from_env(&cfg);
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--seed-gap") == 0) {
            cfg.seed_gap = 1;
        } else if (strcmp(argv[i], "--no-skill") == 0) {
            cfg.write_skill = 0;
        } else if (strcmp(argv[i], "--no-tools") == 0) {
            cfg.use_tools = 0;
        } else if (strcmp(argv[i], "--force-tools") == 0) {
            cfg.force_tools = 1;
        } else if (argv[i][0] != '-') {
            q = argv[i];
        }
    }
    if (!q) {
        fprintf(stderr,
                "usage: %s [--seed-gap] [--force-tools] [--no-skill] [--no-tools] \"query\"\n",
                argv[0]);
        return 2;
    }

    if (cnet_learn_cycle(q, &cfg, &rep) != 0 && rep.source == CNET_LEARN_SRC_NONE) {
        fprintf(stderr, "cnet_learn_cycle: no useful answer\n");
        fprintf(stderr, "detail: %s\n", rep.detail);
        return 1;
    }

    printf("source=%s cache=%d memorized=%d skill_written=%d skill_reused=%d gap=%d\n",
           cnet_learn_source_name(rep.source), rep.from_cache, rep.memorized,
           rep.skill_written, rep.skill_reused, rep.gap_seeded);
    if (rep.skill_path[0]) printf("skill=%s\n", rep.skill_path);
    printf("answer=%s\n", rep.answer);
    printf("detail=%s\n", rep.detail);
    printf("CNET_LEARN_CYCLE_OK\n");
    return 0;
}
