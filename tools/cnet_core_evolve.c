/* Unattended CORE evolve tick.
 * Reads live miss_log + optional factory seeds; writes .lut bricks.
 * Never residual auto-CERT. Propose ≠ admit (admit only after TABLE≥0.95).
 *
 * Usage:
 *   cnet_core_evolve [--once]
 * Env:
 *   CNET_CORE_BUS_BRICKS_DIR   brick output dir (required)
 *   CNET_BONSAI_GGUF           host weights (optional factory)
 *   CNET_MISS_LOG              live miss jsonl (optional path3)
 *   CNET_CORE_EVOLVE_FACTORY=1 run default 2-brick factory if bank empty
 */
#include "cnet_core_bus.h"
#include "cnet_core_paths.h"
#include "cnet_core_serve.h"
#include "cnet_agi_scenario.h"
#include "cnet_agi_scenario2.h"
#include "cnet_agi_scenario3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int count_luts(const char *dir) {
    CnetServeBank b;
    if (cnet_serve_bank_load_dir(&b, dir) != 0) return 0;
    return b.n;
}

int main(int argc, char **argv) {
    const char *dir = getenv("CNET_CORE_BUS_BRICKS_DIR");
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    const char *miss = getenv("CNET_MISS_LOG");
    const char *fac = getenv("CNET_CORE_EVOLVE_FACTORY");
    CnetCoreBus bus;
    int n0, n1;
    int did = 0;
    (void)argc;
    (void)argv;

    if (!dir || !dir[0]) {
        fprintf(stderr, "cnet_core_evolve: set CNET_CORE_BUS_BRICKS_DIR\n");
        return 2;
    }
    mkdir(dir, 0755);
    if (!bonsai || !bonsai[0])
        bonsai = "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";

    n0 = count_luts(dir);
    cnet_core_bus_init(&bus);
    setenv("CNET_CORE_BUS_BRICKS_DIR", dir, 1);

    /* Path3: miss-log with complete nibble table → admit brick */
    if (miss && miss[0] && access(miss, R_OK) == 0) {
        CnetPath3Bench pb;
        char gguf[768];
        snprintf(gguf, sizeof gguf, "%s/evolve_miss_domain.gguf", dir);
        memset(&pb, 0, sizeof pb);
        if (cnet_path3_miss_to_admit(&bus, miss, gguf, "evolve_miss_brick",
                                     "evolve_nibble", &pb) == 0 &&
            pb.served_ok) {
            printf("evolve: miss→admit ok serve=%d admit_delta=%d ms=%.3f\n",
                   pb.served_ok, pb.admit_calls_delta, pb.ms_total);
            did = 1;
        } else {
            printf("evolve: miss path propose_rc=%d table=%d admit=%d "
                   "(no full table yet is OK)\n",
                   pb.propose_rc, pb.table_ok, pb.admit_ok);
        }
    }

    /* Factory seed if bank empty or forced */
    if ((fac && fac[0] == '1') || n0 == 0) {
        CnetPath2Bench fb;
        CnetPath2Spec specs[2];
        specs[0] =
            (CnetPath2Spec){"blk.0.attn_q.weight", "q1_add16", "brick_q_add", 0};
        specs[1] =
            (CnetPath2Spec){"blk.0.attn_k.weight", "q1_xor16", "brick_k_xor", 1};
        if (access(bonsai, R_OK) == 0) {
            if (cnet_path2_factory_run(&bus, bonsai, dir, specs, 2, &fb) == 0) {
                printf("evolve: factory built=%d served=%d ms=%.3f\n", fb.n_built,
                       fb.n_served_ok, fb.ms_total);
                did = 1;
            } else {
                printf("evolve: factory rc fail built=%d\n", fb.n_built);
            }
        }
    }

    /* Ensure .lut files exist for every parked brick */
    {
        int i;
        for (i = 0; i < bus.n_bricks; ++i) {
            if (!bus.bricks[i].live || !bus.bricks[i].certified) continue;
            (void)cnet_serve_save_lut(dir, bus.bricks[i].domain_tag,
                                      bus.bricks[i].name,
                                      bus.bricks[i].lut_table);
        }
    }

    /* AGI-scenario evolve tick: given/miss valuation path */
    {
        CnetAgiScenario agi;
        const char *missp = getenv("CNET_MISS_LOG");
        cnet_agi_scenario_init(&agi, dir, missp && missp[0] ? missp : "agi_evolve_miss.jsonl",
                               bonsai);
        /* reload bus bricks into agi bus via serve luts only */
        (void)cnet_serve_bank_load_dir(&agi.serve, dir);
        if (cnet_agi_scenario_evolve_tick(&agi) == 0) {
            printf("evolve: agi_scenario_tick did work\n");
            did = 1;
        }
        cnet_agi_scenario_free(&agi);
    }
    if (getenv("CNET_AGI2") && getenv("CNET_AGI2")[0] == '1') {
        CnetAgiScenario2 s2;
        char ws[768];
        snprintf(ws, sizeof ws, "%s/agi2_workspace.txt", dir);
        cnet_agi2_init(&s2, dir, getenv("CNET_MISS_LOG") ? getenv("CNET_MISS_LOG")
                                                         : "agi2_evolve_miss.jsonl",
                       bonsai, ws);
        (void)cnet_agi2_restore(&s2);
        (void)cnet_serve_bank_load_dir(&s2.base.serve, dir);
        (void)cnet_agi_scenario_evolve_tick(&s2.base);
        (void)cnet_agi2_persist(&s2);
        printf("evolve: agi2 workspace persist\n");
        cnet_agi2_free(&s2);
        did = 1;
    }


    /* Layer 3: open goals queued by cnetd → CERT-only plan synth + run */
    {
        char gpath[768];
        FILE *gf;
        snprintf(gpath, sizeof gpath, "%s/pending_goals.txt", dir);
        gf = fopen(gpath, "r");
        if (gf) {
            CnetAgiScenario3 s3;
            char ws[768], line[512];
            int n_goals = 0, n_ok = 0, n_rej = 0;
            snprintf(ws, sizeof ws, "%s/agi3_workspace.txt", dir);
            cnet_agi3_init(&s3, dir, miss && miss[0] ? miss : "agi3_evolve_miss.jsonl",
                           bonsai, ws);
            (void)cnet_agi3_boot(&s3, n0 == 0 ? 1 : 0);
            while (fgets(line, sizeof line, gf)) {
                char *nl = strchr(line, '\n');
                int pi;
                if (nl) *nl = 0;
                if (!line[0] || line[0] == '#') continue;
                n_goals++;
                pi = cnet_agi3_synthesize(&s3, line);
                if (pi == -2) {
                    n_rej++;
                    printf("evolve: goal REJECT non-cert | %s\n", line);
                    continue;
                }
                if (pi >= 0 && cnet_agi3_plan_run(&s3, pi) == 0) {
                    n_ok++;
                    printf("evolve: goal OK | %s\n", line);
                    did = 1;
                } else {
                    printf("evolve: goal fail/partial | %s\n", line);
                }
            }
            fclose(gf);
            /* rewrite queue empty after processing */
            gf = fopen(gpath, "w");
            if (gf) {
                fprintf(gf, "# processed\n");
                fclose(gf);
            }
            (void)cnet_agi3_specialists_sync(&s3);
            {
                int i;
                for (i = 0; i < s3.L2.base.bus.n_bricks; ++i) {
                    if (!s3.L2.base.bus.bricks[i].live) continue;
                    (void)cnet_serve_save_lut(dir, s3.L2.base.bus.bricks[i].domain_tag,
                                              s3.L2.base.bus.bricks[i].name,
                                              s3.L2.base.bus.bricks[i].lut_table);
                }
            }
            printf("evolve: agi3 goals total=%d ok=%d reject=%d specialists=%d\n",
                   n_goals, n_ok, n_rej, s3.n_specialists);
            cnet_agi3_free(&s3);
        }
    }

    cnet_core_bus_free(&bus);
    n1 = count_luts(dir);
    printf("CNET_CORE_EVOLVE_OK luts_before=%d luts_after=%d did=%d\n", n0, n1,
           did);
    printf("switch=1 self_evolve_tick=1 agi3_goals=1 residual_auto_cert=0\n");
    return 0;
}
