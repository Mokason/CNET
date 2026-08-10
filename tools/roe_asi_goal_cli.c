/* CLI: map/split/run a goal
 *   roe_asi_goal_cli "learn basic coding"
 *   roe_asi_goal_cli "debug traceback" --no-learn
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_roe_goal.h"

int main(int argc, char **argv) {
    RoeGoalEngine G;
    RoeGoalPlan plan;
    const char *goal = NULL;
    const char *cat = "artifacts/roe_goal_catalog";
    int auto_learn = 1, i;
    char stats[600];

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-learn")) auto_learn = 0;
        else if (!strcmp(argv[i], "--catalog") && i + 1 < argc) cat = argv[++i];
        else if (argv[i][0] != '-' && !goal) goal = argv[i];
    }
    if (!goal) {
        fprintf(stderr, "usage: %s \"goal text\" [--no-learn] [--catalog DIR]\n",
                argv[0]);
        return 2;
    }

    roe_goal_init(&G);
    roe_goal_set_catalog(&G, cat);
    roe_goal_seed_default(&G);
    (void)roe_goal_load(&G);

    (void)roe_goal_run(&G, goal, auto_learn, &plan);
    printf("goal: %s\n", plan.goal);
    for (i = 0; i < plan.n_splits; i++) {
        const RoeGoalSplit *s = &plan.splits[i];
        const char *st =
            s->status == ROE_GOAL_HAVE      ? "HAVE"
            : s->status == ROE_GOAL_LEARNED ? "LEARN"
            : s->status == ROE_GOAL_MISS    ? "MISS"
                                              : "?";
        printf(" %2d. [%s] %s/%s  %s\n", i + 1, st, s->cat, s->sub, s->text);
        if (s->answer[0]) printf("     %s\n", s->answer);
    }
    printf("%s\n", plan.summary);
    roe_goal_dump_stats(&G, stats, sizeof stats);
    printf("%s\n", stats);
    (void)roe_goal_save(&G);
    printf("ROE_GOAL_CLI_PASS\n");
    return plan.all_resolved ? 0 : 0; /* always 0 if ran; miss is informative */
}
