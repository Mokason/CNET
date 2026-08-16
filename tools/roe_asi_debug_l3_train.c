/* Train ROE debug to L3 (project memory). ROE_ASI_DEBUG_L3_PASS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_roe_debug.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-66s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Synthetic multi-project world */
static const char *TB_A1 =
    "Traceback (most recent call last):\n  File \"app.py\", line 10, in <module>\n    "
    "from util import helper\nImportError: cannot import name 'helper' from partially "
    "initialized module 'util' (most likely due to a circular import)";

static const char *TB_A2 =
    "Traceback (most recent call last):\n  File \"tests/test_api.py\", line 4\n    "
    "assert client.get('/x').status_code == 200\nAssertionError";

static const char *TB_B1 =
    "Traceback (most recent call last):\n  File \"worker.py\", line 22, in run\n    "
    "cfg['host']\nKeyError: 'host'";

static const char *TB_B2 =
    "Traceback (most recent call last):\n  File \"worker.py\", line 30\n    "
    "data[i]\nIndexError: list index out of range";

static const char *TB_C1 =
    "Traceback (most recent call last):\n  File \"ml/train.py\", line 50\n    "
    "model.fit(x)\nAttributeError: 'NoneType' object has no attribute 'fit'";

int main(void) {
  /* HEAP, NOT STACK: this struct exceeds the 2 MB MinGW stack reserve
   * (RoeAsi 3.01 MB, RoeDebug 4.15 MB, RoeOcr 6.15 MB since ROE_ANSWER_MAX
   * went 512 -> 4096 in 85c433e and is embedded 640x). A stack instance
   * dies inside ___chkstk_ms in the prologue, before any statement runs. */
    RoeDebug *D = (RoeDebug *)calloc(1, sizeof *D);
    RoeDbgReply r;
    char stats[800];
    const char *cat = "artifacts/roe_debug_catalog";
    int i, t;
    int l3_hits = 0;

    failures = checks = 0;
    printf("=== ROE-ASI Debug L1–L3 train ===\n");
    roe_dbg_init(D);
    roe_dbg_set_catalog(D, cat);
    check(roe_dbg_seed_curriculum(D) == 0, "seed L1/L2 curriculum");
    check(roe_dbg_add_project(D, "proj_webapi") == 0, "add proj_webapi");
    check(roe_dbg_add_project(D, "proj_worker") == 0, "add proj_worker");
    check(roe_dbg_add_project(D, "proj_ml") == 0, "add proj_ml");

    /* --- Phase 0: L1 recipe hits without project memory --- */
    check(roe_dbg_turn(D, "proj_webapi", "NameError: name 'foo' is not defined", &r) ==
                  ROE_OK &&
              r.level == ROE_DBG_L1_RECIPE,
          "L1 NameError recipe");
    check(roe_dbg_turn(D, "proj_webapi", "debug checklist please", &r) == ROE_OK &&
              r.level == ROE_DBG_L2_CHECKLIST,
          "L2 checklist");

    /* --- Phase 1: first sightings are NOT L3 (miss or L1) --- */
    check(roe_dbg_turn(D, "proj_webapi", TB_A1, &r) != ROE_ERR, "A1 turn");
    check(r.level != ROE_DBG_L3_PROJECT, "A1 not L3 before learn");
    /* verify with tests_passed → L3 learn */
    check(roe_dbg_verify(D, "proj_webapi", TB_A1,
                         "Break util<->app cycle: move helper to util_helpers.py; "
                         "import inside function",
                         1, 0) == 1,
          "A1 verified promote L3");

    check(roe_dbg_verify(D, "proj_webapi", TB_A2,
                         "Fixture client missing auth header; set Authorization in conftest",
                         1, 0) == 1,
          "A2 promote L3");

    check(roe_dbg_verify(D, "proj_worker", TB_B1,
                         "Load defaults: cfg.setdefault('host', '127.0.0.1') before use",
                         1, 0) == 1,
          "B1 promote L3");
    check(roe_dbg_verify(D, "proj_worker", TB_B2,
                         "Guard empty batch: if not data: return; else use data[i]", 1,
                         0) == 1,
          "B2 promote L3");
    check(roe_dbg_verify(D, "proj_ml", TB_C1,
                         "model is None because build_model failed; check checkpoint path "
                         "and init before fit",
                         1, 0) == 1,
          "C1 promote L3");

    check(D->n_mem >= 5, ">=5 project memories");
    check(D->n_promote_l3 >= 5, "promote_l3 counted");

    /* --- Phase 2: same bugs hit L3 local (project-scoped) --- */
    for (t = 0; t < 5; t++) {
        check(roe_dbg_turn(D, "proj_webapi", TB_A1, &r) == ROE_OK &&
                  r.level == ROE_DBG_L3_PROJECT && r.tokens_est == 0,
              t == 0 ? "A1 L3 local free" : "A1 L3 repeat");
        if (r.level == ROE_DBG_L3_PROJECT) l3_hits++;
        check(roe_dbg_turn(D, "proj_worker", TB_B1, &r) == ROE_OK &&
                  r.level == ROE_DBG_L3_PROJECT,
              "B1 L3 local");
        if (r.level == ROE_DBG_L3_PROJECT) l3_hits++;
        check(roe_dbg_turn(D, "proj_ml", TB_C1, &r) == ROE_OK &&
                  r.level == ROE_DBG_L3_PROJECT,
              "C1 L3 local");
        if (r.level == ROE_DBG_L3_PROJECT) l3_hits++;
    }

    /* Project isolation: same ImportError circular text in worker should NOT use webapi L3
     * unless signature matches AND project matches — our mem is project-keyed */
    {
        int lvl;
        check(roe_dbg_turn(D, "proj_worker", TB_A1, &r) == ROE_OK || r.level != 0,
              "worker+A1 turn");
        lvl = r.level;
        /* may be L1 ImportError recipe, not L3 webapi memory */
        check(lvl != ROE_DBG_L3_PROJECT || strstr(r.answer, "proj_worker") != NULL,
              "L3 not leaked from other project");
        if (lvl == ROE_DBG_L3_PROJECT)
            check(strstr(r.answer, "proj_webapi") == NULL, "no webapi fix on worker");
    }

    /* Refuse promote without tests/user */
    check(roe_dbg_verify(D, "proj_ml", "RuntimeError: boom", "delete production db", 0,
                         0) == 0,
          "no promote without verify");
    check(D->n_verify_fail >= 1, "verify_fail counted");

    /* Save + cold load */
    check(roe_dbg_save(D) >= 5, "save project memory");
    {
        RoeDebug D2;
        RoeDbgReply r2;
        roe_dbg_init(&D2);
        roe_dbg_set_catalog(&D2, cat);
        check(roe_dbg_seed_curriculum(&D2) == 0, "seed on D2");
        check(roe_dbg_load(&D2) >= 5, "load project memory");
        check(roe_dbg_turn(&D2, "proj_webapi", TB_A1, &r2) == ROE_OK &&
                  r2.level == ROE_DBG_L3_PROJECT,
              "cold L3 after reload");
        check(roe_dbg_turn(&D2, "proj_worker", TB_B2, &r2) == ROE_OK &&
                  r2.level == ROE_DBG_L3_PROJECT,
              "cold L3 B2");
        printf("  reloaded mem=%zu projects=%zu\n", D2.n_mem, D2.n_projects);
    }

    /* Soak: 200 recurring L3 queries across projects — token save */
    {
        uint64_t base0 = D->tokens_baseline, used0 = D->tokens_used;
        uint64_t l3_0 = D->n_l3;
        for (i = 0; i < 200; i++) {
            const char *proj = (i % 3 == 0) ? "proj_webapi"
                               : (i % 3 == 1) ? "proj_worker"
                                                : "proj_ml";
            const char *tb = (i % 3 == 0) ? TB_A1 : (i % 3 == 1) ? TB_B1 : TB_C1;
            (void)roe_dbg_turn(D, proj, tb, &r);
            if (r.level == ROE_DBG_L3_PROJECT) l3_hits++;
        }
        {
            uint64_t dbase = D->tokens_baseline - base0;
            uint64_t dused = D->tokens_used - used0;
            double save = dbase ? 1.0 - (double)dused / (double)dbase : 0.0;
            double l3r = (D->n_l3 - l3_0) / 200.0;
            printf("  soak200 L3_rate=%.2f token_save=%.2f (used=%llu base=%llu)\n", l3r,
                   save, (unsigned long long)dused, (unsigned long long)dbase);
            check(l3r >= 0.95, "soak L3 rate >= 95%");
            check(save >= 0.90, "soak token save >= 90%");
        }
    }

    roe_dbg_dump_stats(D, stats, sizeof stats);
    printf("\n  %s\n", stats);
    printf("  catalog → %s  l3_hits_total~=%d\n", cat, l3_hits);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures) {
        printf("ROE_ASI_DEBUG_L3_FAIL\n");
        return 1;
    }
    printf("ROE_ASI_DEBUG_L3_PASS\n");
    return 0;
}
