/* Unattended CORE evolve tick — steered by evolve_direction.conf
 *
 * Usage: cnet_core_evolve [--once]
 * Env:
 *   CNET_CORE_BUS_BRICKS_DIR   required
 *   CNET_BONSAI_GGUF           host weights
 *   CNET_MISS_LOG              live miss jsonl
 *   CNET_EVOLVE_DIRECTION      optional path to direction conf
 *   CNET_CORE_EVOLVE_FACTORY=1 force factory even if conf says otherwise
 */
#include "cnet_agi_scenario.h"
#include "cnet_agi_scenario2.h"
#include "cnet_agi_scenario3.h"
#include "cnet_core_bus.h"
#include "cnet_core_paths.h"
#include "cnet_core_serve.h"
#include "cnet_evolve_dir.h"
#include "cnet_live_miss.h"
#include "cnet_obsidian_learn.h"
#include "cnet_grok_guide.h"

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

static int lut_exists(const char *dir, const char *tag) {
    char p[768];
    snprintf(p, sizeof p, "%s/%s.lut", dir, tag);
    return access(p, R_OK) == 0;
}

int main(int argc, char **argv) {
    const char *dir = getenv("CNET_CORE_BUS_BRICKS_DIR");
    const char *bonsai = getenv("CNET_BONSAI_GGUF");
    const char *miss = getenv("CNET_MISS_LOG");
    const char *fac_env = getenv("CNET_CORE_EVOLVE_FACTORY");
    CnetEvolveDirection dirn;
    CnetCoreBus bus;
    int n0, n1, new_count = 0;
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
    /* The factory reads a single tensor out of a ~1.1GB GGUF. Without mmap the
     * read fails and the whole mint reports "partial built=0" -- silently, and
     * (before the temp+rename fix) after deleting the brick it was replacing.
     * cnetd.service sets CNET_GGUF_MMAP=1, so remint works as a cnetd child and
     * fails from a timer, a cron, or an ops shell: the unattended path was the
     * only one that could not self-heal. Default it on here so every caller
     * gets the working configuration; an explicit setting still wins. */
    setenv("CNET_GGUF_MMAP", "1", 0);

    (void)cnet_evolve_dir_load(&dirn, dir);
    printf("evolve: direction from %s (live=%d factory=%d goals=%d max_new=%d)\n",
           dirn.loaded_from, dirn.allow_live_miss, dirn.allow_factory,
           dirn.allow_goals, dirn.max_new_per_tick);

    n0 = count_luts(dir);
    cnet_core_bus_init(&bus);
    setenv("CNET_CORE_BUS_BRICKS_DIR", dir, 1);

    if (dirn.allow_obsidian) {
        CnetObsidianLearnReport orep;
        char missbuf[768];
        const char *mp = (miss && miss[0]) ? miss : NULL;
        const char *vault = dirn.obsidian_vault[0] ? dirn.obsidian_vault : NULL;
        if (!mp) {
            snprintf(missbuf, sizeof missbuf, "%s/obsidian_miss.jsonl", dir);
            mp = missbuf;
            miss = mp;
        }
        memset(&orep, 0, sizeof orep);
        if (cnet_obsidian_learn(vault, mp, dir, dirn.obsidian_max_files, &orep) == 0) {
            printf("evolve: obsidian vault=%s scanned=%d hit=%d teaches=%d domains=%d goals=%d\n",
                   orep.vault, orep.files_scanned, orep.files_hit, orep.teaches,
                   orep.domains_complete_emitted, orep.goals_queued);
            if (orep.teaches > 0 || orep.goals_queued > 0) did = 1;
        } else {
            printf("evolve: obsidian skip (no vault or error)\n");
        }
    }

    /* 1) Live miss domains (steered by prefer/deny) */
    if (dirn.allow_live_miss && miss && miss[0] && access(miss, R_OK) == 0) {
        char doms[16][CNET_LIVE_DOM_NAME];
        int nd = cnet_live_miss_complete_domains(miss, doms, 16);
        int di;
        for (di = 0; di < nd; ++di) {
            CnetPath3Bench pb;
            char gguf[768], bname[80];
            float lut[16];
            if (!cnet_evolve_dir_domain_ok(&dirn, doms[di])) {
                printf("evolve: skip domain %s (direction filter)\n", doms[di]);
                continue;
            }
            if (lut_exists(dir, doms[di])) continue;
            if (new_count >= dirn.max_new_per_tick) break;
            if (cnet_live_miss_domain_pairs(miss, doms[di], lut) != 16) continue;
            snprintf(gguf, sizeof gguf, "%s/live_%s.gguf", dir, doms[di]);
            snprintf(bname, sizeof bname, "live_%s", doms[di]);
            if (bus.state != CNET_CORE_BUS_IDLE) {
                if (bus.wo.bound) (void)cnet_core_bus_unbind(&bus);
                bus.state = CNET_CORE_BUS_IDLE;
            }
            memset(&pb, 0, sizeof pb);
            if (cnet_path3_miss_to_admit(&bus, miss, gguf, bname, doms[di],
                                         &pb) == 0 &&
                pb.served_ok) {
                printf("evolve: live domain %s admitted ms=%.3f\n", doms[di],
                       pb.ms_total);
                did = 1;
                new_count++;
            }
        }
        printf("evolve: complete_domains scanned=%d new=%d\n", nd, new_count);
    }

    /* 2) Factory curriculum from direction.
     * CNET_CORE_EVOLVE_FACTORY=1 forces factory even when conf has
     * allow_factory=0 (ops / empty-bank recovery).
     * CNET_CORE_AUTO_EVOLVE=1 also mints *missing* curriculum tags so the
     * live self-improve path does not hard-stop on known factory domains.
     * Conf still owns the curriculum list; residual never auto-CERTs. */
    {
        const char *auto_ev = getenv("CNET_CORE_AUTO_EVOLVE");
        int force_fac = (fac_env && fac_env[0] == '1');
        int auto_mint = (auto_ev && auto_ev[0] == '1');
        int allow_fac = force_fac || dirn.allow_factory || auto_mint;
        int want_fac =
            force_fac ||
            (dirn.allow_factory && dirn.factory_if_empty && n0 == 0) ||
            (auto_mint && dirn.n_factory > 0);
        /* also build missing curriculum tags if factory allowed/forced/auto */
        if (allow_fac && dirn.n_factory > 0) {
            CnetPath2Spec todo[CNET_EVDIR_MAX_FACTORY];
            int nt = 0, i;
            for (i = 0; i < dirn.n_factory; ++i) {
                if (lut_exists(dir, dirn.factory[i].tag)) continue;
                if (nt >= dirn.max_new_per_tick - new_count) break;
                todo[nt++] = dirn.factory[i];
            }
            if (nt > 0 && access(bonsai, R_OK) == 0) {
                CnetPath2Bench fb;
                if (cnet_path2_factory_run(&bus, bonsai, dir, todo, nt, &fb) ==
                    0) {
                    printf("evolve: factory curriculum built=%d ms=%.3f%s%s\n",
                           fb.n_built, fb.ms_total,
                           force_fac ? " force=1" : "",
                           auto_mint && !force_fac ? " auto_improve=1" : "");
                    did = 1;
                    new_count += fb.n_built;
                } else if (want_fac) {
                    /* Loud: a partial factory run means coverage did NOT grow.
                     * This used to be indistinguishable from "nothing to do". */
                    printf("evolve: factory curriculum partial built=%d "
                           "requested=%d gate=spec_rate/teacher_unbound/prove "
                           "bonsai=%s mmap=%s\n",
                           fb.n_built, fb.n_requested, bonsai,
                           getenv("CNET_GGUF_MMAP") ? getenv("CNET_GGUF_MMAP") : "0");
                    fprintf(stderr,
                            "evolve: FACTORY_MINT_FAILED tags_requested=%d built=%d "
                            "(brick left unchanged)\n",
                            fb.n_requested, fb.n_built);
                }
            }
        } else if (want_fac && access(bonsai, R_OK) == 0 && n0 == 0) {
            CnetPath2Bench fb;
            if (cnet_path2_factory_run(&bus, bonsai, dir, dirn.factory,
                                       dirn.n_factory > 2 ? 2 : dirn.n_factory,
                                       &fb) == 0) {
                printf("evolve: factory seed built=%d ms=%.3f\n", fb.n_built,
                       fb.ms_total);
                did = 1;
            }
        }
    }

    /* flush bus bricks to .lut */
    {
        int i;
        for (i = 0; i < bus.n_bricks; ++i) {
            if (!bus.bricks[i].live || !bus.bricks[i].certified) continue;
            (void)cnet_serve_save_lut(dir, bus.bricks[i].domain_tag,
                                      bus.bricks[i].name,
                                      bus.bricks[i].lut_table);
        }
    }

    /* 3) Direction seed goals + pending_goals.txt */
    if (dirn.allow_goals) {
        char gpath[768];
        FILE *gf;
        int gi;
        /* prepend curriculum goals once per tick if not present as luts only */
        snprintf(gpath, sizeof gpath, "%s/pending_goals.txt", dir);
        if (dirn.n_goals > 0) {
            gf = fopen(gpath, "a");
            if (gf) {
                for (gi = 0; gi < dirn.n_goals; ++gi)
                    fprintf(gf, "%s\n", dirn.goals[gi]);
                fclose(gf);
            }
        }
        gf = fopen(gpath, "r");
        if (gf) {
            CnetAgiScenario3 s3;
            char ws[768], line[512];
            int n_goals = 0, n_ok = 0, n_rej = 0;
            snprintf(ws, sizeof ws, "%s/agi3_workspace.txt", dir);
            cnet_agi3_init(&s3, dir, miss && miss[0] ? miss : "agi3_evolve_miss.jsonl",
                           bonsai, ws);
            (void)cnet_agi3_boot(&s3, 0);
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
            gf = fopen(gpath, "w");
            if (gf) {
                fprintf(gf, "# processed\n");
                fclose(gf);
            }
            {
                int i;
                for (i = 0; i < s3.L2.base.bus.n_bricks; ++i) {
                    if (!s3.L2.base.bus.bricks[i].live) continue;
                    (void)cnet_serve_save_lut(
                        dir, s3.L2.base.bus.bricks[i].domain_tag,
                        s3.L2.base.bus.bricks[i].name,
                        s3.L2.base.bus.bricks[i].lut_table);
                }
            }
            printf("evolve: agi3 goals total=%d ok=%d reject=%d specialists=%d\n",
                   n_goals, n_ok, n_rej, s3.n_specialists);
            cnet_agi3_free(&s3);
        }
    }

    /* 4) AGI ticks */
    if (dirn.allow_agi_tick) {
        CnetAgiScenario agi;
        const char *missp = getenv("CNET_MISS_LOG");
        cnet_agi_scenario_init(&agi, dir,
                               missp && missp[0] ? missp : "agi_evolve_miss.jsonl",
                               bonsai);
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
        cnet_agi2_init(&s2, dir,
                       getenv("CNET_MISS_LOG") ? getenv("CNET_MISS_LOG")
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

    /* Optional Grok guide (non-CERT), 30min/q default, few questions */
    {
        CnetGrokGuideReport gr;
        int mq = 3, sec = 1800;
        const char *ms = getenv("CNET_GROK_GUIDE_MAX_Q");
        const char *ss = getenv("CNET_GROK_GUIDE_SECONDS");
        if (ms && ms[0]) mq = atoi(ms);
        if (ss && ss[0]) sec = atoi(ss);
        if (sec > 1800) sec = 1800;
        if (cnet_grok_guide_run(dir, mq, sec, &gr) == 0 && gr.enabled) {
            printf("evolve: grok_guide asked=%d answered=%d err=%d timeout=%d log=%s\n",
                   gr.asked, gr.answered, gr.errors, gr.timed_out, gr.log_path);
            if (gr.answered > 0) did = 1;
        }
    }

    cnet_core_bus_free(&bus);
    n1 = count_luts(dir);
    printf("CNET_CORE_EVOLVE_OK luts_before=%d luts_after=%d did=%d "
           "direction=%s\n",
           n0, n1, did, dirn.loaded_from);
    printf("switch=1 self_evolve_tick=1 direction=1 residual_auto_cert=0\n");
    return 0;
}
