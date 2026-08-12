/* ROE self-model CLI — instrumented inventory dump + pack export.
 *
 *   roe_asi_self_cli snapshot [--catalog DIR] [--out DIR] [--repo ROOT]
 *                             [--goal "text"] [--thoughts]
 *   roe_asi_self_cli ask "query" [--catalog DIR]
 *   roe_asi_self_cli validate [--out DIR]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/cnet_roe_self.h"
#include "../include/cnet_platform.h"  /* cnet_setenv / cnet_mkdir */

static void seed_base(RoeAsi *R) {
    roe_add_skill(R, "greet", "greet", "hello", "Hello. ROE-ASI local skill.", 0, 1);
    roe_add_skill(R, "identity", "identity", "who are you",
                  "I am ROE-ASI: local CERT skills first; escalate on miss; "
                  "never self-CERT.",
                  0, 1);
    roe_add_skill(R, "self_model", "self_model", "self model",
                  "I expose coverage, skill health, tree ranks from gates, and "
                  "goal HAVE/MISS — instrumented self-model, not consciousness.",
                  0, 1);
    roe_add_lookup(R, "fail-closed",
                   "Fail-closed means abstain outside certified coverage.");
    roe_add_teach(R, "roe-asi", "roe_def",
                  "ROE-ASI serves certified local skills and only uses LLM on miss.");
}

static void usage(const char *a0) {
    fprintf(stderr,
            "usage:\n"
            "  %s snapshot [--catalog DIR] [--out DIR] [--repo ROOT] "
            "[--goal TEXT] [--thoughts]\n"
            "  %s ask \"query\" [--catalog DIR]\n"
            "  %s validate [--out DIR]\n",
            a0, a0, a0);
}

int main(int argc, char **argv) {
    const char *cmd = NULL;
    const char *catalog = "artifacts/roe_catalog";
    const char *out_dir = "artifacts/roe_self_model";
    const char *repo = ".";
    const char *goal = NULL;
    const char *ask_q = NULL;
    int thoughts = 0, i;
  /* HEAP, NOT STACK: this struct exceeds the 2 MB MinGW stack reserve
   * (RoeAsi 3.01 MB, RoeDebug 4.15 MB, RoeOcr 6.15 MB since ROE_ANSWER_MAX
   * went 512 -> 4096 in 85c433e and is embedded 640x). A stack instance
   * dies inside ___chkstk_ms in the prologue, before any statement runs. */
    RoeAsi *R = (RoeAsi *)calloc(1, sizeof *R);
    RoeSelfModel M;
    RoeSelfReport rep;
    RoeGoalEngine *G = NULL;
    RoeDocAsset *D = NULL;
    char dump[600];

    for (i = 1; i < argc; i++) {
        if (!cmd && argv[i][0] != '-')
            cmd = argv[i];
        else if (!strcmp(argv[i], "--catalog") && i + 1 < argc)
            catalog = argv[++i];
        else if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out_dir = argv[++i];
        else if (!strcmp(argv[i], "--repo") && i + 1 < argc)
            repo = argv[++i];
        else if (!strcmp(argv[i], "--goal") && i + 1 < argc)
            goal = argv[++i];
        else if (!strcmp(argv[i], "--thoughts"))
            thoughts = 1;
        else if (cmd && !strcmp(cmd, "ask") && argv[i][0] != '-' && !ask_q)
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

    cnet_mkdir("artifacts", 0755);

    if (!strcmp(cmd, "validate")) {
        int ok = roe_self_pack_validate(out_dir);
        printf("pack_dir=%s valid=%d\n", out_dir, ok);
        printf(ok ? "ROE_ASI_SELF_CLI_PASS\n" : "ROE_ASI_SELF_CLI_FAIL\n");
        return ok ? 0 : 1;
    }

    roe_init(R);
    roe_set_catalog_dir(R, catalog);
    {
        int n = roe_load_catalog(R);
        printf("catalog_dir=%s loaded=%d\n", catalog, n);
        if (R->n_skills == 0) seed_base(R);
    }

    if (!strcmp(cmd, "ask")) {
        RoeReply out;
        if (!ask_q) {
            usage(argv[0]);
            return 2;
        }
        roe_turn(R, ask_q, &out);
        printf("source=%s skill=%s verified=%d\n", out.source_name,
               out.skill_id[0] ? out.skill_id : "-", out.verified);
        printf("answer=%s\n", out.answer);
        printf("inventory=%s\n", out.inventory_line);
        printf("ROE_ASI_SELF_CLI_PASS\n");
        return 0;
    }

    if (strcmp(cmd, "snapshot") != 0) {
        usage(argv[0]);
        return 2;
    }

    roe_self_init(&M);
    roe_self_bind_roe(&M, R);
    roe_self_set_out_dir(&M, out_dir);
    roe_self_set_repo_root(&M, repo);
    roe_self_set_record_thoughts(&M, thoughts);

    /* Prefer loading doc/goal catalogs when present */
    D = calloc(1, sizeof *D);
    if (D) {
        roe_doc_init(D);
        roe_doc_set_catalog(D, catalog);
        if (roe_doc_load(D) >= 0) {
            roe_self_bind_doc(&M, D);
            printf("doc_asset bound n_mem=%zu\n", D->n_mem);
        } else {
            free(D);
            D = NULL;
        }
    }

    G = calloc(1, sizeof *G);
    if (G) {
        roe_goal_init(G);
        roe_goal_set_catalog(G, catalog);
        if (roe_goal_load(G) < 0) roe_goal_seed_default(G);
        /* keep goal.roe inventory aligned with primary R skills where empty */
        if (G->roe.n_skills == 0) G->roe = *R;
        roe_self_bind_goal(&M, G);
    }

    if (roe_self_snapshot(&M, &rep) != 0) {
        fprintf(stderr, "snapshot failed\n");
        free(G);
        free(D);
        return 1;
    }
    if (goal && G) {
        if (roe_self_goal_probe(&M, goal, &rep) != 0)
            fprintf(stderr, "goal probe failed (non-fatal)\n");
        else
            printf("goal HAVE=%d MISS=%d LEARNED=%d BLOCKED=%d\n", rep.goal_have,
                   rep.goal_miss, rep.goal_learned, rep.goal_blocked);
    }

    if (roe_self_export_pack(&M, &rep, out_dir) != 0) {
        fprintf(stderr, "export failed\n");
        free(G);
        free(D);
        return 1;
    }

    roe_self_dump(&rep, dump, sizeof dump);
    printf("%s\n", dump);
    printf("summary=%s\n", rep.summary);
    printf("pack=%s valid=%d\n", out_dir, roe_self_pack_validate(out_dir));
    if (rep.has_tree) {
        size_t k;
        printf("tree unlocked=%d/%d power=%.2f\n", rep.tree_snap.nodes_unlocked,
               rep.tree_snap.nodes_total, rep.tree_snap.total_power);
        for (k = 0; k < rep.n_tree_rows; k++) {
            if (rep.tree_rows[k].rank <= 0) continue;
            printf("  node %s %d/%d ev=%s\n", rep.tree_rows[k].node_id,
                   rep.tree_rows[k].rank, rep.tree_rows[k].max_rank,
                   rep.tree_rows[k].evidence);
        }
    }
    printf("health production_ready=%zu / %zu\n", rep.n_production_ready,
           rep.n_health);

    free(G);
    free(D);
    printf("ROE_ASI_SELF_CLI_PASS\n");
    return 0;
}
