/* Certified capsule composition — the milestone the report kept WITHHELD.
 *
 * Claim under test: two or more INDEPENDENTLY certified, separately portable
 * capsules are chained automatically by the existing DAG planner through exact
 * typed ports, with certified coverage enforced at EVERY primitive hop,
 * including the intermediate values a caller never sees.
 *
 * Framing: ASI = Artificial Specialized Intelligence. NOT AGI, NOT
 * superintelligence. This measures a typed composition MECHANISM on a bounded
 * 4-bit arithmetic chain. It says nothing about reasoning or about composition
 * beyond the chain actually executed here.
 *
 * Nothing is hand-wired: no DagNode is constructed by this file. Units are
 * exported as capsules, imported into a FRESH base, bridged into a registry
 * through cnb_load_registry, and the planner is asked only for the final goal.
 *
 * make knowledge_composition_bench -> KNOWLEDGE_COMPOSITION_BENCH_PASS
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../include/cnet_capsule.h"
#include "../include/base.h"
#include "../include/hybrid_ai.h"
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/specialist.h"

#define W 4                 /* 4-bit binary ports */
#define DOMAIN 16
#define A_UNIT "kc_incr"
#define B_UNIT "kc_double"
#define C_UNIT "kc_offset"

static int failures, checks;
static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Distinct operations, not three spellings of one. */
static int op_a(int x) { return (x + 1) % DOMAIN; }   /* increment */
static int op_b(int y) { return (y * 2) % DOMAIN; }   /* double    */
static int op_c(int z) { return (z + 3) % DOMAIN; }   /* offset    */
static int composed(int x) { return op_c(op_b(op_a(x))); }

/* Tags differ in >=2 characters: cnb_tag_mint refuses Levenshtein-1 near-misses. */
static Port BP(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_BINARY_MSB;
    p.field_width = W;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}
static Port P_RAW(void)   { return BP("num_raw"); }
static Port P_INCR(void)  { return BP("val_incr"); }
static Port P_DBL(void)   { return BP("qty_dbl"); }
static Port P_FINAL(void) { return BP("res_final"); }

static void enc(double *v, int x) {
    int i;
    for (i = 0; i < W; i++) v[i] = (x >> (W - 1 - i)) & 1 ? 1.0 : 0.0;
}
static int dec(const double *v) {
    int i, x = 0;
    for (i = 0; i < W; i++) x = (x << 1) | (v[i] > 0.5 ? 1 : 0);
    return x;
}

/* ---- per-hop guard: check the exact input against HybridCoverage --------- */
typedef struct {
    HybridAi *cov;
    char seen[8][64];   /* hops the guard was consulted for, in order */
    int seen_n;
    char refused_at[64];
    char refuse_kind[32];
    int refusals;
} GuardTrace;

/* Guard v1.
 *
 * Binds the hop to the EXECUTING primitive's typed contract via `btn`, not to
 * the unit name. Name-only matching (hybrid_coverage_admits_unit) admits a hop
 * whose record describes different ports, and default-allows an unsupported
 * family or a dimension mismatch — so "coverage enforced at every hop" was not
 * actually true of the ports being executed.
 *
 * Shape support is explicit: exactly one input port and one output port.
 * Branching / multi-input nodes are UNSUPPORTED under v1 and fail closed while
 * the guard is enabled; they are not merely untested. */
static int guard_allow(const char *unit, const BinaryTransformNetwork *btn,
                       const double *in, size_t in_len, void *ctx) {
    GuardTrace *g = (GuardTrace *)ctx;
    if (g->seen_n < 8)
        snprintf(g->seen[g->seen_n++], 64, "%s", unit ? unit : "?");
    if (!unit || !btn) {
        snprintf(g->refused_at, sizeof g->refused_at, "%s", unit ? unit : "?");
        g->refusals++;
        return -1;
    }
    if (btn->input_port_count != 1 || btn->output_port_count != 1) {
        snprintf(g->refused_at, sizeof g->refused_at, "%s", unit);
        snprintf(g->refuse_kind, sizeof g->refuse_kind, "unsupported_shape");
        g->refusals++;
        return -1;
    }
    if (!hybrid_coverage_admits_exact(g->cov, unit, btn->input_ports[0],
                                      btn->output_ports[0], in, in_len)) {
        snprintf(g->refused_at, sizeof g->refused_at, "%s", unit);
        snprintf(g->refuse_kind, sizeof g->refuse_kind, "coverage");
        g->refusals++;
        return -1;
    }
    return 0;
}

/* Build+certify one primitive, seal it, and record coverage over `cov_n`
   inputs chosen from `covered[]`. */
static int make_unit(CnetBase *base, HybridAi *cov, const char *name,
                     Port in_port, Port out_port, int (*fn)(int),
                     const int *covered, int cov_n) {
    BinaryTransformNetwork *btn;
    static double in[DOMAIN][W], tg[DOMAIN][W];
    static double cin[DOMAIN][W], ctg[DOMAIN][W];
    Contract c;
    Specialist s;
    int i, rc = -1;

    for (i = 0; i < DOMAIN; i++) {
        enc(in[i], i);
        enc(tg[i], fn(i));
    }
    /* Bounded DETERMINISTIC seed sweep. 4-bit modular arithmetic with a carry
       chain does not converge to exactness under one fixed configuration —
       measured: +1 stalled at 15/16 and *2 at 12/16 under the recipe that made
       +3 exact. The certification bar is unchanged (the contract must still be
       reproduced exactly); this only keeps training until it is met, and the
       seed that worked is reported so the run stays reproducible. */
    {
        unsigned seed;
        int exact = 0;
        for (seed = 1; seed <= 24 && exact != DOMAIN; seed++) {
            btn = (BinaryTransformNetwork *)calloc(1, sizeof *btn);
            if (!btn) return -1;
            if (btn_init(btn, W, W, 1, 128, 0.8, seed) != 0) {
                free(btn);
                return -1;
            }
            btn_set_ports(btn, in_port, out_port);
            btn_train_dynamic(btn, (const double *)in, (const double *)tg,
                              DOMAIN, 160000, 1000, 0.0015, 0.01);
            btn_train(btn, (const double *)in, (const double *)tg, DOMAIN, 20000);
            exact = 0;
            for (i = 0; i < DOMAIN; i++) {
                const double *o = btn_forward(btn, in[i]);
                if (o && dec(o) == fn(i)) exact++;
            }
            if (exact == DOMAIN) {
                printf("      %s exact 16/16 at seed=%u\n", name, seed);
                break;
            }
            btn_free(btn);
            free(btn);
            btn = NULL;
        }
        if (!btn || exact != DOMAIN) {
            printf("      %s NOT exact within the seed sweep — not certifiable\n",
                   name);
            if (btn) { btn_free(btn); free(btn); }
            return -2;
        }
    }
    memset(&c, 0, sizeof c);
    if (contract_init_borrowed(&c, name, btn, (const double *)in,
                               (const double *)tg, DOMAIN) != 0) {
        btn_free(btn); free(btn); return -1;
    }
    memset(&s, 0, sizeof s);
    /* Real certification through the specialist door — no test-only sealing. */
    if (specialist_wrap_btn(&s, btn, name) == 0 &&
        cnb_add_unit(base, btn, &c, NULL) == 0)
        rc = 0;
    if (rc == 0) {
        for (i = 0; i < cov_n; i++) {
            enc(cin[i], covered[i]);
            enc(ctg[i], fn(covered[i]));
        }
        if (hybrid_coverage_record(cov, in_port, out_port, name,
                                   (const double *)cin, (const double *)ctg,
                                   (size_t)cov_n, W, W) != 0)
            rc = -1;
    }
    contract_free(&c);
    btn_free(btn);
    free(btn);
    return rc;
}

static void rm_pack(const char *dir) {
    char p[512];
    snprintf(p, sizeof p, "%s/unit.cnb", dir);
    (void)remove(p);
    snprintf(p, sizeof p, "%s/manifest.cknow", dir);
    (void)remove(p);
    (void)rmdir(dir);
}

/* Does the plan contain exactly these primitive names? */
static void walk(const DagNode *n, char out[8][64], int *cnt) {
    size_t i;
    if (!n || *cnt >= 8) return;
    if (n->kind == DAG_PRIMITIVE && n->name) {
        int seen = 0, k;
        for (k = 0; k < *cnt; k++)
            if (strcmp(out[k], n->name) == 0) seen = 1;
        if (!seen) snprintf(out[(*cnt)++], 64, "%s", n->name);
    }
    for (i = 0; i < n->child_count; i++) walk(n->children[i], out, cnt);
}

int main(void) {
    CnetBase src, dst;
    HybridAi cov_src, cov_dst;
    PrimitiveRegistry reg;
    CnetCapsuleReport rep;
    DagSource srcs[1];
    DagPlan plan;
    GuardTrace gt;
    double xin[W], out[W];
    char members[8][64];
    int mcount = 0, i, exported = 0, imported = 0;
    /* Registry BORROWS registered BTNs, so this must outlive registry_free. */
    BinaryTransformNetwork *shortcut = NULL;
    size_t skipped = 0;
    /* A covers all but 9; B covers all but 4; C covers everything. */
    static const int cov_a[] = {0,1,2,3,4,5,6,7,8,10,11,12,13,14,15};
    static const int cov_b[] = {0,1,2,3,5,6,7,8,9,10,11,12,13,14,15};
    static const int cov_c[] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};

    printf("== certified capsule composition (ASI = Artificial Specialized "
           "Intelligence; NOT AGI) ==\n");
    printf("   chain: num_raw --%s--> val_incr --%s--> qty_dbl --%s--> res_final\n",
           A_UNIT, B_UNIT, C_UNIT);

    cnb_init(&src); cnb_init(&dst);
    hybrid_ai_init(&cov_src); hybrid_ai_init(&cov_dst);
    registry_init(&reg);

    check(make_unit(&src, &cov_src, A_UNIT, P_RAW(), P_INCR(), op_a,
                    cov_a, (int)(sizeof cov_a / sizeof cov_a[0])) == 0,
          "build: A increment certified");
    check(make_unit(&src, &cov_src, B_UNIT, P_INCR(), P_DBL(), op_b,
                    cov_b, (int)(sizeof cov_b / sizeof cov_b[0])) == 0,
          "build: B double certified");
    check(make_unit(&src, &cov_src, C_UNIT, P_DBL(), P_FINAL(), op_c,
                    cov_c, (int)(sizeof cov_c / sizeof cov_c[0])) == 0,
          "build: C offset certified");

    /* ---- export each as its OWN capsule, import into a FRESH runtime ---- */
    {
        const char *names[3] = {A_UNIT, B_UNIT, C_UNIT};
        const char *dirs[3] = {"tmp_kc_a", "tmp_kc_b", "tmp_kc_c"};
        for (i = 0; i < 3; i++) {
            rm_pack(dirs[i]);
            memset(&rep, 0, sizeof rep);
            if (cnet_capsule_export(&src, &cov_src, names[i], dirs[i], &rep) == 0)
                exported++;
        }
        check(exported == 3, "capsule: all three exported independently");
        for (i = 0; i < 3; i++) {
            memset(&rep, 0, sizeof rep);
            if (cnet_capsule_import(&dst, &cov_dst, dirs[i], &rep) == 0)
                imported++;
            else
                printf("      import %s refused: %s\n", names[i],
                       rep.reject_reason);
        }
        check(imported == 3, "capsule: all three imported into a FRESH base");
        for (i = 0; i < 3; i++) rm_pack(dirs[i]);
    }

    /* ---- bridge base -> registry through the real API, certified-only ---- */
    check(cnb_load_registry(&dst, &reg, &skipped) == 0,
          "bridge: cnb_load_registry populated the planner registry");
    printf("      registry count=%zu skipped=%zu base_units=%zu\n",
           reg.count, skipped, dst.unit_count);
    for (i = 0; i < (int)reg.count; i++)
        printf("      reg[%d] name=%s certified=%d state=%d\n", i,
               reg.entries[i].name ? reg.entries[i].name : "?",
               reg.entries[i].certified, (int)reg.entries[i].state);
    reg.require_certified = 1;
    check(reg.count == 3, "bridge: exactly three primitives registered");

    /* ---- plan: ask ONLY for the final goal from the raw source ---------- */
    for (i = 0; i < W; i++) xin[i] = 0.0;
    srcs[0].type = P_RAW();
    srcs[0].values = xin;
    memset(&plan, 0, sizeof plan);
    check(dag_plan(&reg, srcs, 1, P_FINAL(), &plan) == 0 && plan.root != NULL,
          "plan: planner found a route num_raw -> res_final");
    walk(plan.root, members, &mcount);
    printf("      plan members (%d):", mcount);
    for (i = 0; i < mcount; i++) printf(" %s", members[i]);
    printf("\n");
    check(mcount == 3, "plan: chain has exactly three primitive members");
    {
        int has_a = 0, has_b = 0, has_c = 0;
        for (i = 0; i < mcount; i++) {
            if (!strcmp(members[i], A_UNIT)) has_a = 1;
            if (!strcmp(members[i], B_UNIT)) has_b = 1;
            if (!strcmp(members[i], C_UNIT)) has_c = 1;
        }
        check(has_a && has_b && has_c,
              "plan: contains A, B and C by name (no shortcut primitive)");
    }
    check(!cnb_has_unit(&dst, "kc_direct"),
          "plan: no direct num_raw->res_final composite exists");

    /* ---- positive: composed tasks no unit was trained on ---------------- */
    {
        int correct = 0, ran = 0;
        for (i = 0; i < DOMAIN; i++) {
            if (i == 9) continue;            /* outside A coverage */
            if (op_a(i) == 4) continue;      /* outside B coverage */
            enc(xin, i);
            memset(out, 0, sizeof out);
            memset(&gt, 0, sizeof gt);
            gt.cov = &cov_dst;
            plan.guard.allow = guard_allow;
            plan.guard.ctx = &gt;
            if (dag_execute(&plan, srcs, 1, out, W) == 0) {
                ran++;
                if (dec(out) == composed(i)) correct++;
            }
        }
        printf("      composed: ran=%d correct=%d (expected f(x)=((x+1)*2+3) mod 16)\n",
               ran, correct);
        check(ran > 0 && correct == ran,
              "compose: every in-coverage composed task exact under the guard");
    }

    /* ---- refusal 1: ROOT input outside A coverage ----------------------- */
    {
        int rc;
        enc(xin, 9);
        memset(&gt, 0, sizeof gt);
        gt.cov = &cov_dst;
        plan.guard.allow = guard_allow;
        plan.guard.ctx = &gt;
        rc = dag_execute(&plan, srcs, 1, out, W);
        check(rc == DAG_EXEC_REFUSED_GUARD, "refuse-root: distinct guard refusal code");
        check(gt.refusals == 1 && strcmp(gt.refused_at, A_UNIT) == 0,
              "refuse-root: refused AT A");
        check(gt.seen_n == 1 && strcmp(gt.seen[0], A_UNIT) == 0,
              "refuse-root: B and C never consulted (A never served)");
    }

    /* ---- refusal 2: INTERMEDIATE outside B coverage --------------------- */
    {
        int rc, x = -1;
        for (i = 0; i < DOMAIN; i++)
            if (i != 9 && op_a(i) == 4) { x = i; break; }
        check(x >= 0, "refuse-mid: found x in A coverage whose A(x) is outside B");
        enc(xin, x);
        memset(&gt, 0, sizeof gt);
        gt.cov = &cov_dst;
        plan.guard.allow = guard_allow;
        plan.guard.ctx = &gt;
        rc = dag_execute(&plan, srcs, 1, out, W);
        check(rc == DAG_EXEC_REFUSED_GUARD, "refuse-mid: distinct guard refusal code");
        check(gt.refusals == 1 && strcmp(gt.refused_at, B_UNIT) == 0,
              "refuse-mid: refused AT B, not A");
        check(gt.seen_n == 2 && strcmp(gt.seen[0], A_UNIT) == 0 &&
                  strcmp(gt.seen[1], B_UNIT) == 0,
              "refuse-mid: A ran, B checked and stopped, C never consulted");

        /* causality: same input, guard OFF, must execute */
        plan.guard.allow = NULL;
        plan.guard.ctx = NULL;
        rc = dag_execute(&plan, srcs, 1, out, W);
        check(rc == 0, "causality: guard OFF -> the same chain executes");
        check(dec(out) == composed(x),
              "causality: unguarded chain even produces the right answer");
        printf("      causality: x=%d intermediate=%d (outside B coverage)\n",
               x, op_a(x));
    }

    /* ================= exact port binding (RED on 0395a6b) ==================
       Every case below leaves the OWNER NAME and the certified ROWS correct and
       changes only the ports the record is bound to. The old name-only query
       admitted all of them; several it default-allowed outright. */
    {
        static double brow[DOMAIN][W], btg[DOMAIN][W];
        const int *cb = cov_b;
        int nb = (int)(sizeof cov_b / sizeof cov_b[0]);
        int k, x_ok = -1;
        const BinaryTransformNetwork *bbtn = NULL;
        unsigned long s0, f0;

        for (k = 0; k < nb; k++) { enc(brow[k], cb[k]); enc(btg[k], op_b(cb[k])); }
        for (i = 0; i < (int)reg.count; i++)
            if (reg.entries[i].name && !strcmp(reg.entries[i].name, B_UNIT))
                bbtn = reg.entries[i].btn;
        check(bbtn != NULL, "bind: located B's live BTN in the registry");
        /* an x whose intermediate IS inside B's certified rows */
        for (i = 0; i < DOMAIN; i++)
            if (i != 9 && op_a(i) != 4) { x_ok = i; break; }
        check(x_ok >= 0, "bind: found a fully in-coverage composed input");

/* Setup must be PROVEN, not assumed: if the forget or the record silently
   failed, the record would simply be absent and every wrong-binding negative
   below would "pass" for the missing-metadata reason instead of the port-binding
   reason it claims to test. Fail immediately with a specific setup reason. */
#define REBIND_DIM(IP, OP, IND, LBL)                                           \
        do {                                                                   \
            int rc_;                                                           \
            /* Prior record may legitimately be absent (the missing-metadata    \
               case ran just before), so the forget count is not asserted; what \
               MUST hold is that the intended wrong binding actually landed. */ \
            (void)hybrid_coverage_forget_unit(&cov_dst, B_UNIT);               \
            rc_ = hybrid_coverage_record(&cov_dst, (IP), (OP), B_UNIT,         \
                                         (const double *)brow,                 \
                                         (const double *)btg, (size_t)nb,      \
                                         (size_t)(IND), W);                    \
            check(rc_ == 0, LBL ": SETUP wrong-binding record stored");        \
            if (rc_ != 0) { failures++; goto bind_setup_failed; }              \
            check(hybrid_coverage_has_unit(&cov_dst, B_UNIT) == 1,             \
                  LBL ": SETUP metadata present (so refusal is about binding)"); \
        } while (0)

#define REBIND(IP, OP, LBL) REBIND_DIM(IP, OP, W, LBL)

#define EXPECT_REFUSED_AT_B(label)                                             \
        do {                                                                   \
            int rc_;                                                           \
            s0 = bbtn ? bbtn->output_successes : 0;                            \
            f0 = bbtn ? bbtn->output_failures : 0;                             \
            enc(xin, x_ok);                                                    \
            memset(&gt, 0, sizeof gt);                                         \
            gt.cov = &cov_dst;                                                 \
            plan.guard.allow = guard_allow;                                    \
            plan.guard.ctx = &gt;                                              \
            rc_ = dag_execute(&plan, srcs, 1, out, W);                         \
            check(rc_ == DAG_EXEC_REFUSED_GUARD, label ": refused (-2)");      \
            check(strcmp(gt.refused_at, B_UNIT) == 0, label ": refused AT B"); \
            check(gt.seen_n == 2 && !strcmp(gt.seen[1], B_UNIT),               \
                  label ": C never consulted");                                \
            check(bbtn && bbtn->output_successes == s0 &&                      \
                      bbtn->output_failures == f0,                             \
                  label ": B's btn_forward did not run");                      \
        } while (0)

        /* 1. wrong input TAG (family/dims correct, rows correct) */
        REBIND(BP("wrong_intag"), P_DBL(), "bind-in-tag");
        EXPECT_REFUSED_AT_B("bind-in-tag");

        /* 2. wrong input width/dim */
        {
            Port narrow = P_INCR();
            narrow.field_width = 2;
            REBIND(narrow, P_DBL(), "bind-in-width");
            EXPECT_REFUSED_AT_B("bind-in-width");
        }

        /* 2b. EXACT ports on both sides, but the record's own in_dim disagrees
           with the assembled input length. The ports match perfectly here, so
           only the independent dimension check can refuse this — a port-width
           change would have been caught by the port comparison instead. */
        REBIND_DIM(P_INCR(), P_DBL(), 2, "bind-covdim");
        check(hybrid_coverage_admits_exact(&cov_dst, B_UNIT, P_INCR(), P_DBL(),
                                           brow[0], W) == 0,
              "bind-covdim: strict query refuses in_dim != assembled length");
        EXPECT_REFUSED_AT_B("bind-covdim");

        /* 3. wrong OUTPUT tag (input side perfectly correct) */
        REBIND(P_INCR(), BP("wrong_outtag"), "bind-out-tag");
        EXPECT_REFUSED_AT_B("bind-out-tag");

        /* 4. wrong output shape */
        {
            Port wide = P_DBL();
            wide.field_count = 2;
            REBIND(P_INCR(), wide, "bind-out-shape");
            EXPECT_REFUSED_AT_B("bind-out-shape");
        }

        /* 5. missing metadata entirely */
        check(hybrid_coverage_forget_unit(&cov_dst, B_UNIT) == 1,
              "bind-missing: SETUP record actually removed");
        check(hybrid_coverage_has_unit(&cov_dst, B_UNIT) == 0,
              "bind-missing: SETUP metadata really absent");
        EXPECT_REFUSED_AT_B("bind-missing");

        /* restore the correct binding and prove the chain works again */
        REBIND(P_INCR(), P_DBL(), "bind-restore");
        enc(xin, x_ok);
        memset(&gt, 0, sizeof gt);
        gt.cov = &cov_dst;
        plan.guard.allow = guard_allow;
        plan.guard.ctx = &gt;
        check(dag_execute(&plan, srcs, 1, out, W) == 0 &&
                  dec(out) == composed(x_ok),
              "bind-restore: correct port binding admits and computes exactly");
#undef REBIND
#undef REBIND_DIM
#undef EXPECT_REFUSED_AT_B
bind_setup_failed:
        ;
    }

    /* ---- guard v1 shape support: multi-port primitives fail closed ------- */
    {
        BinaryTransformNetwork fake;
        GuardTrace g2;
        double probe[W];
        memset(&fake, 0, sizeof fake);
        fake.input_port_count = 2;   /* branching node */
        fake.output_port_count = 1;
        fake.input_ports[0] = P_INCR();
        fake.output_ports[0] = P_DBL();
        memset(&g2, 0, sizeof g2);
        g2.cov = &cov_dst;
        for (i = 0; i < W; i++) probe[i] = 0.0;
        check(guard_allow(B_UNIT, &fake, probe, W, &g2) != 0,
              "shape-v1: multi-INPUT primitive refused (unsupported, not allowed)");
        check(strcmp(g2.refuse_kind, "unsupported_shape") == 0,
              "shape-v1: refusal names the unsupported shape");
        memset(&g2, 0, sizeof g2);
        g2.cov = &cov_dst;
        fake.input_port_count = 1;
        fake.output_port_count = 2;
        check(guard_allow(B_UNIT, &fake, probe, W, &g2) != 0,
              "shape-v1: multi-OUTPUT primitive refused");
    }

    /* ---- adversarial: an UNCERTIFIED compatible shortcut must be excluded - */
    {
        DagPlan p4;
        char m4[8][64];
        int n4 = 0, rc4;
        static double si[DOMAIN][W], st[DOMAIN][W];
        shortcut = (BinaryTransformNetwork *)calloc(1, sizeof *shortcut);
        check(shortcut != NULL, "uncert: shortcut btn allocated");
        for (i = 0; i < DOMAIN; i++) { enc(si[i], i); enc(st[i], composed(i)); }
        if (shortcut && btn_init(shortcut, W, W, 1, 128, 0.8, 3) == 0) {
            btn_set_ports(shortcut, P_RAW(), P_FINAL());
            btn_train_dynamic(shortcut, (const double *)si, (const double *)st,
                              DOMAIN, 40000, 1000, 0.0015, 0.01);
            /* registry_add admits WITHOUT certification */
            check(registry_add(&reg, shortcut, "kc_shortcut") == 0,
                  "uncert: uncertified direct num_raw->res_final registered");
            check(reg.entries[reg.count - 1].certified == 0,
                  "uncert: it really is uncertified");

            /* With the certified chain intact the planner picks it regardless
               of the flag (measured), so that comparison isolates nothing.
               Break the chain instead: reset B, leaving the uncertified
               shortcut as the ONLY num_raw -> res_final route, then toggle the
               flag. Now the flag is the only variable. */
            registry_set_state(&reg, B_UNIT, PRIM_RESET);
            reg.lifecycle_enabled = 1;

            reg.require_certified = 1;
            memset(&p4, 0, sizeof p4);
            rc4 = dag_plan(&reg, srcs, 1, P_FINAL(), &p4);
            check(rc4 != 0 || p4.root == NULL,
                  "uncert: require_certified=1 -> uncertified shortcut NOT usable");
            if (rc4 == 0) dag_free(&p4);

            reg.require_certified = 0;
            n4 = 0;
            memset(&p4, 0, sizeof p4);
            rc4 = dag_plan(&reg, srcs, 1, P_FINAL(), &p4);
            check(rc4 == 0 && p4.root != NULL,
                  "uncert: causality — with the gate off the same shortcut IS usable");
            if (rc4 == 0) {
                walk(p4.root, m4, &n4);
                printf("      uncert causality members (%d):", n4);
                for (i = 0; i < n4; i++) printf(" %s", m4[i]);
                printf("\n");
                check(n4 == 1 && !strcmp(m4[0], "kc_shortcut"),
                      "uncert: and the chosen member is exactly kc_shortcut");
                dag_free(&p4);
            }

            reg.require_certified = 1;
            reg.lifecycle_enabled = 0;
            registry_set_state(&reg, B_UNIT, PRIM_FROZEN);
        }
    }

    /* ---- negative: remove B, composite goal must become unplannable ----- */
    {
        DagPlan p2;
        int rc2;
        registry_set_state(&reg, B_UNIT, PRIM_RESET);
        reg.lifecycle_enabled = 1;
        memset(&p2, 0, sizeof p2);
        rc2 = dag_plan(&reg, srcs, 1, P_FINAL(), &p2);
        check(rc2 != 0 || p2.root == NULL,
              "negative: with B reset, num_raw -> res_final is unplannable");
        if (rc2 == 0) dag_free(&p2);
        reg.lifecycle_enabled = 0;
        registry_set_state(&reg, B_UNIT, PRIM_FROZEN);
    }

    /* ---- negative: unreachable goal tag yields no plan ------------------ */
    {
        DagPlan p3;
        int rc3;
        memset(&p3, 0, sizeof p3);
        rc3 = dag_plan(&reg, srcs, 1, BP("zz_absent"), &p3);
        check(rc3 != 0 || p3.root == NULL,
              "negative: unknown goal tag yields no plan (typed refusal)");
        if (rc3 == 0) dag_free(&p3);
    }

    dag_free(&plan);
    registry_free(&reg);
    if (shortcut) { btn_free(shortcut); free(shortcut); }
    cnb_free(&src); cnb_free(&dst);
    hybrid_ai_free(&cov_src); hybrid_ai_free(&cov_dst);

    printf("checks=%d failures=%d\n", checks, failures);
    if (failures == 0) {
        printf("KNOWLEDGE_COMPOSITION_BENCH_PASS members=%d hops_guarded=every "
               "refusals=root+intermediate\n", mcount);
        return 0;
    }
    printf("KNOWLEDGE_COMPOSITION_BENCH_FAIL failures=%d\n", failures);
    return 1;
}
