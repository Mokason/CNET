/* 3-lane memory runtime load bench: STM / LTM / Forming.
 * Uses skill_pos capsules from skill_capsule_generate artifacts when present,
 * else seals a tiny set inline.
 *
 * make mem_runtime -> MEM_RUNTIME_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/cnet_capsule.h"
#include "../include/cnet_mem_runtime.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-62s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static int dir_ok(const char *p) {
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Minimal one-hot identity skill capsule for offline bench if artifacts missing */
static int make_tiny_capsule(const char *dir, const char *name, int dim, int hot) {
    CnetBase b;
    HybridAi h;
    BinaryTransformNetwork *btn;
    Contract c;
    Port pin, pout;
    double *in, *tg;
    CnetCapsuleReport rep;
    char *stable;
    int i, rc;

    {
        char cmd[900];
        int r;
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
        r = system(cmd);
        (void)r;
        snprintf(cmd, sizeof cmd, "mkdir -p '%s'", dir);
        r = system(cmd);
        (void)r;
    }
    cnb_init(&b);
    hybrid_ai_init(&h);
    memset(&pin, 0, sizeof pin);
    memset(&pout, 0, sizeof pout);
    pin.family = pout.family = PORT_ONEHOT;
    pin.field_width = pout.field_width = (size_t)dim;
    pin.field_count = pout.field_count = 1;
    /* far tags */
    snprintf(pin.tag, sizeof pin.tag, "mi%d", hot);
    snprintf(pout.tag, sizeof pout.tag, "mo%d", hot ^ 0x15);
    in = calloc((size_t)dim, sizeof(double));
    tg = calloc((size_t)dim, sizeof(double));
    btn = calloc(1, sizeof *btn);
    stable = strdup(name);
    if (!in || !tg || !btn || !stable) return -1;
    for (i = 0; i < dim; i++) {
        in[i] = (i == hot) ? 1.0 : 0.0;
        tg[i] = (i == hot) ? 1.0 : 0.0;
    }
    btn_init(btn, (size_t)dim, (size_t)dim, 12, 48, 0.5, 7u + (unsigned)hot);
    btn_set_ports(btn, pin, pout);
    {
        unsigned s;
        int exact = 0;
        for (s = 1; s <= 40 && !exact; s++) {
            if (s > 1) {
                btn_free(btn);
                btn_init(btn, (size_t)dim, (size_t)dim, 12, 48, 0.5, s + (unsigned)hot * 9);
                btn_set_ports(btn, pin, pout);
            }
            btn_train_dynamic(btn, in, tg, 1, 20000, 100, 1e-9, 1e-12);
            btn_train(btn, in, tg, 1, 8000);
            {
                const double *y = btn_forward(btn, in);
                int bi = 0, k;
                if (!y) continue;
                for (k = 1; k < dim; k++)
                    if (y[k] > y[bi]) bi = k;
                exact = (bi == hot);
            }
        }
        if (!exact) {
            free(stable);
            free(in);
            free(tg);
            btn_free(btn);
            free(btn);
            cnb_free(&b);
            hybrid_ai_free(&h);
            return -1;
        }
    }
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, stable, btn, in, tg, 1) != 0 ||
        cnb_add_unit(&b, btn, &c, NULL) != 0) {
        contract_free(&c);
        free(stable);
        free(in);
        free(tg);
        btn_free(btn);
        free(btn);
        cnb_free(&b);
        hybrid_ai_free(&h);
        return -1;
    }
    hybrid_coverage_record(&h, pin, pout, stable, in, tg, 1, (size_t)dim, (size_t)dim);
    memset(&rep, 0, sizeof rep);
    rc = cnet_capsule_export(&b, &h, stable, dir, &rep);
    contract_free(&c);
    btn_free(btn);
    free(btn);
    free(in);
    free(tg);
    /* leave stable for process */
    cnb_free(&b);
    hybrid_ai_free(&h);
    return rc;
}

int main(void) {
    CnetMemRuntime mem;
    const char *root = "artifacts/skill_pos_capsules";
    const char *names[8];
    char dirs[8][400];
    int n_skills = 0, i, r, t;
    const char *u = NULL;
    char stats[512];
    double t0, t1, load_s;
    const int LOAD_N = 5000;
    int use_prebuilt = 0;

    failures = checks = 0;
    printf("== cnet_mem_runtime (STM / LTM / FORM) ==\n");

    /* Prefer existing skill_pos_00..07 from skill_capsule_generate */
    for (i = 0; i < 8; i++) {
        char d[400], nm[64];
        snprintf(nm, sizeof nm, "skill_pos_%02d", i);
        snprintf(d, sizeof d, "%s/%s", root, nm);
        if (dir_ok(d)) {
            names[n_skills] = NULL; /* fill stable below */
            snprintf(dirs[n_skills], sizeof dirs[0], "%s", d);
            {
                char *s = strdup(nm);
                names[n_skills] = s;
            }
            n_skills++;
        }
    }
    if (n_skills >= 4) {
        use_prebuilt = 1;
        printf("  using prebuilt capsules n=%d under %s\n", n_skills, root);
    } else {
        n_skills = 4;
        printf("  minting tiny capsules n=%d\n", n_skills);
        mkdir("artifacts", 0755);
        mkdir("artifacts/mem_runtime_caps", 0755);
        for (i = 0; i < n_skills; i++) {
            char nm[64], d[400];
            snprintf(nm, sizeof nm, "mem_skill_%d", i);
            snprintf(d, sizeof d, "artifacts/mem_runtime_caps/%s", nm);
            check(make_tiny_capsule(d, nm, 4, i % 4) == 0, i == 0 ? "mint capsule" : "mint");
            names[i] = strdup(nm);
            snprintf(dirs[i], sizeof dirs[0], "%s", d);
        }
    }
    check(n_skills >= 4, "have >=4 skills");

    cnet_mem_init(&mem, 2); /* small STM to force LTM fetches */

    /* LTM index only — not loaded yet */
    for (i = 0; i < n_skills; i++)
        check(cnet_mem_ltm_add(&mem, names[i], dirs[i]) == 0, i == 0 ? "ltm_add" : "ltm");

    /* resolve cold → LTM fetch + STM pin */
    r = cnet_mem_resolve(&mem, names[0], &u);
    check(r == CNET_MEM_OK && u && strcmp(u, names[0]) == 0, "cold resolve via LTM");
    check(mem.n_ltm_fetch >= 1, "ltm_fetch counted");
    check(mem.stm_n >= 1, "stm pinned after fetch");

    /* hot STM hit */
    {
        uint64_t f0 = mem.n_ltm_fetch;
        r = cnet_mem_resolve(&mem, names[0], &u);
        check(r == CNET_MEM_OK, "stm hit resolve");
        check(mem.n_ltm_fetch == f0, "no extra ltm fetch on hit");
        check(mem.n_stm_hit >= 1, "stm_hit counted");
    }

    /* unknown → abstain */
    r = cnet_mem_resolve(&mem, "no_such_skill_zz", &u);
    check(r == CNET_MEM_ABSTAIN, "unknown abstain");

    /* FORM: reject not-better */
    check(cnet_mem_form_submit(&mem, names[1], dirs[1], 0.5, 0.9) == -2, "form reject worse");
    check(mem.n_form_reject >= 1, "form_reject stat");

    /* FORM: promote better (or new) — skill not yet in base if only 0 fetched */
    {
        int before = (int)mem.n_form_promote;
        check(cnet_mem_form_submit(&mem, names[1], dirs[1], 1.0, 0.0) == 0, "form submit new");
        check(cnet_mem_form_tick(&mem, 1) >= 1, "form tick promotes");
        check((int)mem.n_form_promote >= before + 1, "form_promote stat");
        r = cnet_mem_resolve(&mem, names[1], &u);
        check(r == CNET_MEM_OK, "resolve after promote");
    }

    /* feedback demote path does not crash */
    cnet_mem_feedback(&mem, names[0], 1);
    cnet_mem_feedback(&mem, names[0], 0);
    check(1, "feedback ok");

    /* ---- Product wire: ASI exec gate + episodes + conf ---- */
    printf("\n-- product wire --\n");
    {
        const uint32_t NEED = 0x3u;
        uint64_t ab0 = mem.n_abstain;
        uint64_t asi0 = mem.n_asi_block;
        uint64_t ep0 = mem.n_episode_log;
        check(cnet_mem_asi_configure(&mem, names[0], "primary sticky skill", NEED, 1,
                                     CNET_ASI_KIND_SPECIALIST, 0.30) == 0,
              "asi_configure names[0]");
        /* missing world bits → abstain before CERT */
        r = cnet_mem_resolve_ex(&mem, names[0], 0x0u, -1.0, &u);
        check(r == CNET_MEM_ABSTAIN, "resolve_ex exec block");
        check(mem.n_asi_block > asi0, "asi_block counted");
        check(mem.n_abstain > ab0, "abstain counted on asi block");

        /* world ok, residual high → conformal abstain */
        r = cnet_mem_resolve_ex(&mem, names[0], NEED, 0.9, &u);
        check(r == CNET_MEM_ABSTAIN, "resolve_ex conformal block");

        /* world ok, residual low → OK */
        r = cnet_mem_resolve_ex(&mem, names[0], NEED, 0.05, &u);
        check(r == CNET_MEM_OK && u && strcmp(u, names[0]) == 0,
              "resolve_ex serve when gated open");

        cnet_mem_feedback_ex(&mem, names[0], 1, NEED, 0.05);
        check(mem.n_episode_log > ep0, "episode logged on feedback_ex");
        check(cnet_mem_continual_regressions(&mem) == 0, "no regression after ok");

        /* inject fail episode then form refuse */
        cnet_mem_feedback_ex(&mem, names[2], 1, 0u, 0.0);
        cnet_mem_feedback_ex(&mem, names[2], 0, 0u, 0.8);
        check(cnet_mem_continual_regressions(&mem) >= 1, "regression after fail");
        {
            int rej0 = (int)mem.n_form_reject;
            check(cnet_mem_form_submit(&mem, names[2], dirs[2], 1.0, 0.0) == 0,
                  "form submit regress skill");
            check(cnet_mem_form_tick(&mem, 0) == 0, "form tick refuses regression");
            check((int)mem.n_form_reject > rej0, "form_reject on regression");
        }
        printf("  product wire asi_block=%llu ep_log=%llu regressions=%d\n",
               (unsigned long long)mem.n_asi_block,
               (unsigned long long)mem.n_episode_log,
               cnet_mem_continual_regressions(&mem));
    }

    /* LOAD: sticky traffic — 80% on first 2 skills (STM residents), 20% explore */
    t0 = wall_s();
    for (t = 0; t < LOAD_N; t++) {
        const char *nm;
        if ((t % 5) == 0)
            nm = names[2 + (t % (n_skills > 2 ? n_skills - 2 : 1))]; /* explore */
        else
            nm = names[t % 2]; /* sticky hot */
        if ((t % 50) == 0) (void)cnet_mem_form_tick(&mem, 1);
        r = cnet_mem_resolve(&mem, nm, &u);
        if (r == CNET_MEM_OK) cnet_mem_feedback(&mem, nm, 1);
    }
    t1 = wall_s();
    load_s = t1 - t0;

    cnet_mem_dump_stats(&mem, stats, sizeof stats);
    printf("\n-- load bench N=%d --\n", LOAD_N);
    printf("  wall=%.4fs  per_resolve_us=%.2f\n", load_s, 1e6 * load_s / (double)LOAD_N);
    printf("  %s\n", stats);
    printf("  stm_hit_rate=%.3f\n", cnet_mem_stm_hit_rate(&mem));

    check(mem.n_serve >= (uint64_t)LOAD_N, "serve count");
    check(cnet_mem_stm_hit_rate(&mem) > 0.3, "stm hit rate > 0.3 under load");
    check(mem.n_abstain >= 1, "had at least one abstain");
    check(load_s < 5.0, "load finishes < 5s");

    /* STM cap eviction: pin many, stm_n <= cap */
    for (i = 0; i < n_skills; i++) {
        (void)cnet_mem_resolve(&mem, names[i], &u);
    }
    check(mem.stm_n <= mem.stm_cap, "stm respects cap");

    printf("\n  prebuilt=%d stm_cap=%zu\n", use_prebuilt, mem.stm_cap);

    cnet_mem_free(&mem);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("MEM_RUNTIME_PASS\n");
        return 0;
    }
    printf("MEM_RUNTIME_FAIL\n");
    return 1;
}
