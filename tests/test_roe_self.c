/* Hermetic gate: ROE self-model loop closure.
 * make roe_asi_self → ROE_ASI_SELF_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../include/cnet_roe_self.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-66s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    RoeAsi R;
    RoeGoalEngine *G = NULL;
    RoeDocAsset *D = NULL;
    RoeSelfModel M;
    RoeSelfReport rep;
    RoeReply out;
    char dump[512];
    const char *cat = "artifacts/roe_self_model_test";
    const char *pack = "artifacts/roe_self_model_test/pack";
    int rc;

    failures = checks = 0;
    printf("=== ROE self-model (close all loops) ===\n");

    mkdir("artifacts", 0755);
    mkdir(cat, 0755);

    roe_init(&R);
    roe_set_catalog_dir(&R, cat);
    roe_add_skill(&R, "identity", "identity", "who are you",
                  "I am ROE-ASI: CERT local first, abstain outside coverage.", 0,
                  1);
    roe_add_skill(&R, "law", "shell_law", "self-cert",
                  "Never self-CERT. Verify then promote.", 0, 1);
    roe_add_skill(&R, "uncert_stub", "tmp", "zz_uncert_only", "draft", 0, 0);
    roe_add_lookup(&R, "fail-closed", "Abstain outside certified coverage.");
    roe_add_teach(&R, "roe self model", "self_def",
                  "Instrumented inventory of skills coverage health tree goals.");

    /* 1) per-turn inventory */
    check(roe_turn(&R, "who are you", &out) == ROE_OK, "turn local identity");
    check(out.source == ROE_SRC_LOCAL, "source LOCAL");
    check(strcmp(out.source_name, "LOCAL") == 0, "source_name filled");
    check(out.inventory_line[0] != 0, "inventory_line filled");
    check(strstr(out.inventory_line, "never_self_cert=1") != NULL,
          "inventory encodes never_self_cert");
    check(strstr(out.inventory_line, "skill=identity") != NULL, "skill_id in inventory");

    check(roe_turn(&R, "what is fail-closed", &out) == ROE_OK, "turn lookup");
    check(out.source == ROE_SRC_LOOKUP && strcmp(out.source_name, "LOOKUP") == 0,
          "lookup inventory");
    check(out.verified == 0, "lookup untrusted");

    check(roe_turn(&R, "explain roe self model please", &out) == ROE_OK,
          "turn teach llm path");
    check(out.source == ROE_SRC_LLM && out.verified == 0, "llm untrusted");

    check(roe_turn(&R, "totally unknown zzqqxx never", &out) == ROE_OK ||
              out.source == ROE_SRC_ABSTAIN || out.source == ROE_SRC_ASK_USER,
          "OOD path returns");
    /* force abstain inventory shape */
    {
        RoeReply a;
        memset(&a, 0, sizeof a);
        a.source = ROE_SRC_ABSTAIN;
        roe_reply_fill_inventory(&R, &a);
        check(strcmp(a.source_name, "ABSTAIN") == 0, "abstain name");
    }

    /* 2) health scan */
    {
        RoeSkillHealth H[16];
        size_t n = 0, ready = 0;
        check(roe_self_health_scan(&R, H, 16, &n, &ready) == 0, "health scan");
        check(n >= 2, "health rows for active skills");
        check(ready >= 1, "at least one production_ready cert skill");
        {
            size_t i;
            int saw_uncert_fail = 0;
            for (i = 0; i < n; i++) {
                if (strcmp(H[i].skill_id, "uncert_stub") == 0) {
                    check(H[i].layer[3] == ROE_HL_FAIL, "uncert fails SEMANTIC");
                    saw_uncert_fail = 1;
                }
                if (strcmp(H[i].skill_id, "identity") == 0)
                    check(H[i].production_ready == 1, "identity production_ready");
            }
            check(saw_uncert_fail, "uncert skill observed");
        }
    }

    /* 3) self model + tree evidence + export */
    roe_self_init(&M);
    roe_self_bind_roe(&M, &R);
    roe_self_set_out_dir(&M, cat);
    roe_self_set_repo_root(&M, ".");
    roe_self_set_record_thoughts(&M, 1);
    check(roe_self_ensure_ocr_tree(&M) == 0, "seed ocr tree");
    check(roe_self_apply_gate_evidence(&M) == 0, "apply gate evidence");
    check(roe_tree_get(M.tree, "rec_hermetic") != NULL, "hermetic node exists");
    /* doctrine roots always ranked */
    check(roe_tree_get(M.tree, "root_shell")->rank >= 2, "shell root ranked");
    check(roe_tree_get(M.tree, "beat_quality")->rank == 0,
          "quality beat stays locked without SOTA claim");

    check(roe_self_snapshot(&M, &rep) == 0, "snapshot");
    check(rep.never_self_cert == 1 && rep.second_brain == 0, "doctrine flags");
    check(rep.n_skills_cert >= 2, "cert skill count");
    check(rep.has_tree == 1 && rep.tree_snap.nodes_unlocked >= 3, "tree unlocked");
    check(rep.n_production_ready >= 1, "ready skills in report");
    check(rep.inventory_line[0] != 0, "report inventory_line");

    /* 5) optional doc coverage bind */
    D = calloc(1, sizeof *D);
    check(D != NULL, "alloc doc asset");
    if (D) {
        roe_doc_init(D);
        roe_doc_set_catalog(D, cat);
        roe_doc_seed_asset(D);
        roe_self_bind_doc(&M, D);
        check(roe_self_snapshot(&M, &rep) == 0, "snapshot with doc");
        check(rep.has_doc == 1, "doc coverage present");
    }

    /* 4) goal engine + probe AFTER final snapshot so export keeps goal */
    G = calloc(1, sizeof *G);
    check(G != NULL, "alloc goal engine");
    if (G) {
        roe_goal_init(G);
        roe_goal_set_catalog(G, cat);
        roe_goal_seed_default(G);
        G->roe = R;
        roe_self_bind_goal(&M, G);
        rc = roe_self_goal_probe(&M, "who are you and shell law on self-cert",
                                &rep);
        check(rc == 0, "goal probe");
        check(rep.has_goal == 1, "goal marked");
        check(rep.goal_have + rep.goal_miss + rep.goal_learned + rep.goal_blocked >
                  0,
              "goal split statuses counted");
    }

    /* 6) pack export + validate */
    mkdir(pack, 0755);
    check(roe_self_export_pack(&M, &rep, pack) == 0, "export pack");
    check(roe_self_pack_validate(pack) == 1, "pack validate");
    {
        FILE *f = fopen("artifacts/roe_self_model_test/pack/self_report.json", "r");
        char buf[256];
        int has = 0;
        check(f != NULL, "self_report.json exists");
        if (f) {
            while (fgets(buf, sizeof buf, f))
                if (strstr(buf, "never_self_cert")) has = 1;
            fclose(f);
        }
        check(has, "json encodes never_self_cert");
    }

    roe_self_dump(&rep, dump, sizeof dump);
    printf("  dump: %s\n", dump);
    check(strstr(dump, "never_self_cert") != NULL, "dump mentions law");

    /* save catalog of cert skills */
    check(roe_save_catalog(&R) >= 2, "save skill catalog");

    free(G);
    free(D);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_SELF_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_SELF_PASS\n");
    return 0;
}
