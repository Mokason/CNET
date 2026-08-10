/* Teacher → skill_* position capsules → import fresh → generate_fifo.
 * Compare in-memory CERT path vs capsule round-trip. Benchmark both.
 *
 * make skill_capsule_generate -> SKILL_CAPSULE_GENERATE_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../include/base.h"
#include "../include/cnet_capsule.h"
#include "../include/cnet_generate_fifo.h"
#include "../include/contract/contract.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/specialist.h"

static int failures, checks;
static void check(int ok, const char *m) {
    checks++;
    printf("  %-62s %s\n", m, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static const char *TEACHER = "hello_cnet_chess_fifo_neurons_generate_text_not_parrot";
static const char *CAP_ROOT = "artifacts/skill_pos_capsules";

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static void oh(double *v, int hot, int n) {
    int i;
    for (i = 0; i < n; i++) v[i] = (i == hot) ? 1.0 : 0.0;
}

static int build_alphabet(const char *s, char *charset, int *cmap, int maxA) {
    int n = 0, i;
    memset(cmap, -1, 256 * sizeof(int));
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (cmap[c] >= 0) continue;
        if (n >= maxA) break;
        cmap[c] = n;
        charset[n++] = (char)c;
    }
    charset[n] = 0;
    return n;
}

static void rm_rf(const char *path) {
    char cmd[700];
    int rc;
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    rc = system(cmd);
    (void)rc;
}

/* Seal one position skill into base + coverage (unique port tags per square).
 * unit_name must be stable heap storage (base borrows the name). */
static int seal_pos_skill(CnetBase *base, HybridAi *cov, int n_pos, int A, int pos,
                          int out_hot, char *unit_name_stable) {
    BinaryTransformNetwork *btn;
    double *in, *tg;
    Contract c;
    Port pin, pout;
    unsigned seed = 19u + (unsigned)pos * 41u + (unsigned)out_hot;

    memset(&pin, 0, sizeof pin);
    memset(&pout, 0, sizeof pout);
    pin.family = pout.family = PORT_ONEHOT;
    pin.field_width = (size_t)n_pos;
    pout.field_width = (size_t)A;
    pin.field_count = pout.field_count = 1;
    snprintf(pin.tag, sizeof pin.tag, "in%02d_%02d", pos, (n_pos - 1 - pos));
    snprintf(pout.tag, sizeof pout.tag, "ot%02d_%02d", pos, (n_pos - 1 - pos) ^ 0x3f);

    in = (double *)calloc((size_t)n_pos, sizeof(double));
    tg = (double *)calloc((size_t)A, sizeof(double));
    btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
    if (!in || !tg || !btn) {
        free(in);
        free(tg);
        free(btn);
        return -1;
    }
    if (btn_init(btn, (size_t)n_pos, (size_t)A, 16, 64, 0.5, seed) != 0) {
        fprintf(stderr, "btn_init fail n_pos=%d A=%d\n", n_pos, A);
        free(in);
        free(tg);
        free(btn);
        return -1;
    }
    if (btn_set_ports(btn, pin, pout) != 0) {
        btn_free(btn);
        free(btn);
        free(in);
        free(tg);
        return -1;
    }
    oh(in, pos, n_pos);
    oh(tg, out_hot, A);
    {
        unsigned s;
        int exact = 0;
        for (s = seed; s < seed + 48 && !exact; s++) {
            if (s != seed) {
                btn_free(btn);
                if (btn_init(btn, (size_t)n_pos, (size_t)A, 16, 64, 0.5, s) != 0) continue;
                if (btn_set_ports(btn, pin, pout) != 0) continue;
            }
            btn_train_dynamic(btn, in, tg, 1, 30000, 100, 1e-9, 1e-12);
            btn_train(btn, in, tg, 1, 12000);
            {
                const double *y = btn_forward(btn, in);
                int j, ok = 1;
                if (!y) continue;
                for (j = 0; j < A; j++) {
                    int pred = y[j] > 0.5 ? 1 : 0;
                    int want = tg[j] > 0.5 ? 1 : 0;
                    if (pred != want) ok = 0;
                }
                /* also require argmax */
                {
                    int bi = 0, k;
                    for (k = 1; k < A; k++)
                        if (y[k] > y[bi]) bi = k;
                    if (bi != out_hot) ok = 0;
                }
                exact = ok;
            }
        }
        if (!exact) {
            fprintf(stderr, "train not exact pos=%d\n", pos);
            btn_free(btn);
            free(btn);
            free(in);
            free(tg);
            return -1;
        }
    }
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, unit_name_stable, btn, in, tg, 1) != 0) {
        fprintf(stderr, "contract_init fail unit=%s\n", unit_name_stable);
        btn_free(btn);
        free(btn);
        free(in);
        free(tg);
        return -1;
    }
    {
        int arc = cnb_add_unit(base, btn, &c, NULL);
        if (arc != 0) {
            fprintf(stderr, "cnb_add_unit rc=%d unit=%s\n", arc, unit_name_stable);
            contract_free(&c);
            btn_free(btn);
            free(btn);
            free(in);
            free(tg);
            return -1;
        }
    }
    if (hybrid_coverage_record(cov, pin, pout, unit_name_stable, in, tg, 1, (size_t)n_pos,
                               (size_t)A) != 0) {
        fprintf(stderr, "coverage_record fail unit=%s\n", unit_name_stable);
        contract_free(&c);
        btn_free(btn);
        free(btn);
        free(in);
        free(tg);
        return -1;
    }
    contract_free(&c);
    btn_free(btn);
    free(btn);
    free(in);
    free(tg);
    return 0;
}

/* Load registry from base for generate (cnb_load_registry). */
static int bridge_reg(CnetBase *base, PrimitiveRegistry *reg) {
    size_t skipped = 0;
    registry_init(reg);
    return cnb_load_registry(base, reg, &skipped);
}

static int run_generate(CnetBase *base, HybridAi *cov, PrimitiveRegistry *reg, int n_pos,
                        int A, const char *charset, char *out, size_t ocap, size_t *olen,
                        int steps) {
    CnetGenEngine e;
    int i;
    cnet_gen_engine_init(&e, A, 8192);
    cnet_gen_engine_set_dims(&e, n_pos, A);
    cnet_gen_engine_set_advance(&e, CNET_GEN_ADVANCE_POSITION, n_pos);
    /* ports unused for coverage when btn ports preferred; still set dims */
    e.in_port.family = PORT_ONEHOT;
    e.out_port.family = PORT_ONEHOT;
    e.in_port.field_width = (size_t)n_pos;
    e.out_port.field_width = (size_t)A;
    e.in_port.field_count = e.out_port.field_count = 1;
    for (i = 0; i < n_pos; i++) {
        char name[64];
        snprintf(name, sizeof name, "skill_pos_%02d", i);
        if (cnb_has_unit(base, name)) cnet_gen_bind(&e, i, name);
    }
    cnet_gen_run(&e, cov, reg, 0, steps, 0, charset, out, ocap, olen);
    {
        int ab = (int)e.n_abstain;
        cnet_gen_engine_free(&e);
        return ab;
    }
}

int main(void) {
    char charset[CNET_GEN_MAX_UNITS + 1];
    int cmap[256], ids[512];
    int A, L, n_pos, i;
    CnetBase src, dst;
    HybridAi cov_src, cov_dst;
    PrimitiveRegistry reg_src, reg_dst;
    int have_reg_src = 0, have_reg_dst = 0, src_inited = 0, dst_inited = 0;
    char out_mem[512], out_cap[512], out_long[8192];
    size_t n_mem = 0, n_cap = 0, n_long = 0;
    double t0, t1;
    double t_seal, t_export, t_import, t_gen_mem, t_gen_cap;
    int exported = 0, imported = 0, ab_long;
    CnetCapsuleReport rep;

    failures = checks = 0;
    printf("== skill capsules: teacher → skill_pos_* → import → generate ==\n");
    printf("teacher=\"%s\"\n", TEACHER);

    A = build_alphabet(TEACHER, charset, cmap, CNET_GEN_MAX_UNITS);
    L = (int)strlen(TEACHER);
    for (i = 0; i < L; i++) ids[i] = cmap[(unsigned char)TEACHER[i]];
    n_pos = L - 1;
    check(n_pos >= 4 && n_pos <= CNET_GEN_MAX_UNITS, "board size ok");

    rm_rf(CAP_ROOT);
    mkdir("artifacts", 0755);
    mkdir(CAP_ROOT, 0755);

    cnb_init(&src);
    src_inited = 1;
    hybrid_ai_init(&cov_src);

    /* 1) Seal all position skills in source base */
    t0 = wall_s();
    for (i = 0; i < n_pos; i++) {
        char tmp[64];
        char *stable;
        snprintf(tmp, sizeof tmp, "skill_pos_%02d", i);
        stable = (char *)malloc(strlen(tmp) + 1);
        if (!stable) {
            check(0, "seal skill oom");
            goto done;
        }
        memcpy(stable, tmp, strlen(tmp) + 1);
        if (seal_pos_skill(&src, &cov_src, n_pos, A, i, ids[i + 1], stable) != 0) {
            free(stable);
            check(0, "seal skill");
            goto done;
        }
        /* stable name owned for process lifetime (base borrows) */
    }
    t1 = wall_s();
    t_seal = t1 - t0;
    check((int)src.unit_count == n_pos, "source unit_count == n_pos");
    printf("  sealed_skills=%d seal_wall=%.3fs (%.1f ms/skill)\n", n_pos, t_seal,
           1000.0 * t_seal / (double)n_pos);

    /* 2) Export each as capsule */
    t0 = wall_s();
    for (i = 0; i < n_pos; i++) {
        char name[64], dir[400];
        snprintf(name, sizeof name, "skill_pos_%02d", i);
        snprintf(dir, sizeof dir, "%s/%s", CAP_ROOT, name);
        mkdir(dir, 0755);
        memset(&rep, 0, sizeof rep);
        if (cnet_capsule_export(&src, &cov_src, name, dir, &rep) != 0) {
            printf("  export fail %s reason=%s\n", name, rep.reject_reason);
            check(0, "export skill capsule");
            goto done;
        }
        exported++;
    }
    t1 = wall_s();
    t_export = t1 - t0;
    check(exported == n_pos, "all skills exported");
    printf("  exported=%d export_wall=%.3fs\n", exported, t_export);

    /* 3) In-memory generate (source base) */
    check(bridge_reg(&src, &reg_src) == 0, "bridge source registry");
    have_reg_src = 1;
    t0 = wall_s();
    (void)run_generate(&src, &cov_src, &reg_src, n_pos, A, charset, out_mem, sizeof out_mem,
                       &n_mem, n_pos);
    t1 = wall_s();
    t_gen_mem = t1 - t0;
    check(n_mem == (size_t)n_pos, "mem gen full length");
    check(strcmp(out_mem, TEACHER + 1) == 0, "mem gen exact teacher path");
    printf("  mem_gen len=%zu wall=%.4fs out=\"%s\"\n", n_mem, t_gen_mem, out_mem);

    /* 4) Fresh import all capsules */
    cnb_init(&dst);
    dst_inited = 1;
    hybrid_ai_init(&cov_dst);
    t0 = wall_s();
    for (i = 0; i < n_pos; i++) {
        char name[64], dir[400];
        snprintf(name, sizeof name, "skill_pos_%02d", i);
        snprintf(dir, sizeof dir, "%s/%s", CAP_ROOT, name);
        memset(&rep, 0, sizeof rep);
        if (cnet_capsule_import(&dst, &cov_dst, dir, &rep) != 0) {
            printf("  import fail %s reason=%s\n", name, rep.reject_reason);
            check(0, "import skill capsule");
            goto done;
        }
        imported++;
    }
    t1 = wall_s();
    t_import = t1 - t0;
    check(imported == n_pos, "all skills imported");
    check((int)dst.unit_count == n_pos, "dst unit_count");
    check(hybrid_coverage_count(&cov_dst) == (size_t)n_pos, "dst coverage count");
    printf("  imported=%d import_wall=%.3fs coverage=%zu\n", imported, t_import,
           hybrid_coverage_count(&cov_dst));

    /* 5) Generate from imported capsules only */
    check(bridge_reg(&dst, &reg_dst) == 0, "bridge dest registry");
    have_reg_dst = 1;
    {
        size_t cert = 0, e;
        for (e = 0; e < reg_dst.count; e++)
            if (reg_dst.entries[e].certified) cert++;
        check(cert == (size_t)n_pos, "all imported registry CERT");
    }
    t0 = wall_s();
    (void)run_generate(&dst, &cov_dst, &reg_dst, n_pos, A, charset, out_cap, sizeof out_cap,
                       &n_cap, n_pos);
    t1 = wall_s();
    t_gen_cap = t1 - t0;
    check(n_cap == (size_t)n_pos, "capsule gen full length");
    check(strcmp(out_cap, TEACHER + 1) == 0, "capsule gen exact teacher path");
    check(strcmp(out_cap, out_mem) == 0, "capsule gen == mem gen");
    printf("  cap_gen len=%zu wall=%.4fs out=\"%s\"\n", n_cap, t_gen_cap, out_cap);

    /* 6) Long ask fail-closed on imported board */
    ab_long = run_generate(&dst, &cov_dst, &reg_dst, n_pos, A, charset, out_long,
                           sizeof out_long, &n_long, 4096);
    check(n_long == (size_t)n_pos, "long gen stops at board");
    check(ab_long >= 1, "long gen abstains past end");
    printf("  long_ask=4096 len=%zu abstain>=1 ok\n", n_long);

    /* 7) Scoreboard */
    printf("\n== BENCH COMPARE ==\n");
    printf("  path                wall_s\n");
    printf("  seal+train+admit    %.4f\n", t_seal);
    printf("  capsule export      %.4f\n", t_export);
    printf("  capsule import      %.4f\n", t_import);
    printf("  generate (mem)      %.4f\n", t_gen_mem);
    printf("  generate (capsule)  %.4f\n", t_gen_cap);
    printf("  roundtrip total     %.4f  (export+import+gen)\n", t_export + t_import + t_gen_cap);
    printf("  learn+mem gen       %.4f\n", t_seal + t_gen_mem);
    printf("  quality             mem=exact capsule=exact match_each_other=yes\n");
    printf("  skills              %d portable dirs under %s\n", n_pos, CAP_ROOT);

    check(t_gen_cap < 0.05, "capsule generate still cheap");
    check(t_import > 0.0 && exported == imported, "import did real work");

done:
    printf("cleanup...\n");
    fflush(stdout);
    if (have_reg_src) registry_free(&reg_src);
    if (have_reg_dst) registry_free(&reg_dst);
    printf("free src\n");
    fflush(stdout);
    if (src_inited) cnb_free(&src);
    printf("free dst\n");
    fflush(stdout);
    if (dst_inited) cnb_free(&dst);
    hybrid_ai_free(&cov_src);
    if (dst_inited) hybrid_ai_free(&cov_dst);

    printf("\nchecks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("SKILL_CAPSULE_GENERATE_PASS\n");
        return 0;
    }
    printf("SKILL_CAPSULE_GENERATE_FAIL\n");
    return 1;
}
