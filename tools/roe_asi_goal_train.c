/* Goal map + microsplit + tidy learn. ROE_ASI_GOAL_PASS */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_roe_goal.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-64s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void print_plan(const RoeGoalPlan *p) {
    int i;
    printf("  goal: %s\n", p->goal);
    for (i = 0; i < p->n_splits; i++) {
        const RoeGoalSplit *s = &p->splits[i];
        const char *st =
            s->status == ROE_GOAL_HAVE      ? "HAVE"
            : s->status == ROE_GOAL_LEARNED ? "LEARNED"
            : s->status == ROE_GOAL_MISS    ? "MISS"
                                              : "BLOCK";
        printf("    [%s] %s/%s :: %s\n", st, s->cat, s->sub, s->text);
        if (s->answer[0]) printf("         → %s\n", s->answer);
    }
    printf("  %s\n", p->summary);
}

int main(void) {
    RoeGoalEngine G;
    RoeGoalPlan plan;
    char stats[600];
    const char *cat = "artifacts/roe_goal_catalog";

    failures = checks = 0;
    printf("=== ROE goal map / microsplit / tidy learn ===\n");
    roe_goal_init(&G);
    roe_goal_set_catalog(&G, cat);
    check(roe_goal_seed_default(&G) == 0, "seed taxonomy+knowledge");
    check(G.n_cats >= 3, "categories exist");
    check(G.roe.n_skills >= 5, "starter skills");

    /* Goal 1: basic coding — mix HAVE + LEARN */
    check(roe_goal_run(&G, "learn basic coding", 1, &plan) == 0 ||
              plan.n_splits >= 5,
          "run basic coding goal");
    print_plan(&plan);
    check(plan.n_splits >= 6, "microsplit produced several steps");
    check(plan.knowledge_rate > 0.0, "some prior knowledge");
    {
        int learned = 0, have = 0, i;
        for (i = 0; i < plan.n_splits; i++) {
            if (plan.splits[i].status == ROE_GOAL_LEARNED) learned++;
            if (plan.splits[i].status == ROE_GOAL_HAVE) have++;
        }
        printf("  have=%d learned=%d\n", have, learned);
        check(have >= 2, "reused known skills");
        check(learned >= 2, "learned missing splits");
    }

    /* Goal 2: same goal again — should be mostly HAVE, big token save */
    {
        uint64_t t0 = G.tokens_used;
        RoeGoalPlan p2;
        check(roe_goal_run(&G, "learn basic coding", 1, &p2) == 0, "second run resolved");
        print_plan(&p2);
        check(p2.knowledge_rate >= 0.7, "second run knowledge high");
        check(G.tokens_used == t0 || p2.token_save >= 0.5, "second run cheap");
    }

    /* Goal 3: debug compound */
    {
        RoeGoalPlan p3;
        (void)roe_goal_run(&G, "debug traceback with pytest and project memory", 1,
                           &p3);
        print_plan(&p3);
        check(p3.n_splits >= 4, "debug goal split");
        {
            int i, has_dbg = 0;
            for (i = 0; i < p3.n_splits; i++)
                if (!strcmp(p3.splits[i].cat, "debug")) has_dbg = 1;
            check(has_dbg, "debug category used");
        }
    }

    /* Goal 4: web api style compound */
    {
        RoeGoalPlan p4;
        (void)roe_goal_run(&G, "build web api with tests", 1, &p4);
        print_plan(&p4);
        check(p4.n_splits >= 4, "api goal split");
    }

    check(roe_goal_save(&G) >= 1, "save tidy catalog");
    check(G.roe.n_skills >= 10, "skills grew via learn");

    /* reload */
    {
        RoeGoalEngine G2;
        RoeGoalPlan p;
        roe_goal_init(&G2);
        roe_goal_set_catalog(&G2, cat);
        roe_goal_seed_default(&G2);
        check(roe_goal_load(&G2) >= 5, "load skills");
        check(roe_goal_run(&G2, "learn basic coding", 0, &p) == 0 ||
                  p.knowledge_rate >= 0.5,
              "loaded knowledge serves without learn");
        print_plan(&p);
    }

    roe_goal_dump_stats(&G, stats, sizeof stats);
    printf("\n  %s\n", stats);
    printf("  catalog → %s\n", cat);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_GOAL_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_GOAL_PASS\n");
    return 0;
}
