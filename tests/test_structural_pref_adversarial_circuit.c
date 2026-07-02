/*
 * test_structural_pref_adversarial_circuit.c -- the MULTI-ROOT (circuit) analogue
 * of the saturated-beam prune probe. Proves the ORDER_ONLY rank-artifact prior
 * exercises MEMBERSHIP authority on the circuit planner's root-pair set, and
 * locks the fix in.
 *
 * WHY THIS IS SHAPED DIFFERENTLY FROM THE SINGLE-ROOT PROBE. The circuit planner
 * has an exhaustive FALLBACK the single-root planner lacks
 * (src/router.c:2710-2733): if the prior's top-k pair restriction yields no
 * complete circuit, it disables the restriction and re-runs full. So the
 * single-root trick -- boost NON-completing pairs to evict the sole completer --
 * does NOT break feasibility here: every completer gets evicted, the restricted
 * pass fails, the fallback fires, and the completer is recovered. A pure
 * solvable->unsolvable break is therefore NOT achievable on the circuit path.
 *
 * The fallback-PROOF violation is a SUBOPTIMAL-LOCK. ORDER_ONLY + artifact sets
 * g_circuit_prune_active=1 and restricts root choices to the top-k pairs ranked
 * by a PRIOR-BIASED pair_score (src/router.c:2636,2669) -- directly
 * contradicting the code's own comment that ORDER_ONLY "Full set preserved, no
 * pruning" (src/router.c:2567). If the prior evicts the OPTIMAL completer O from
 * the top-k while a valid-but-WORSE completer W survives, the restricted pass
 * succeeds with W (first_pass_success -> NO fallback) and W is locked in. The
 * circuit branch-and-bound is reliability-weighted (score *= btn_reliability,
 * src/router.c:2305), so a reorder can NEVER change which completer wins; an
 * O->W selection flip can ONLY come from O being PRUNED out of candidacy. That
 * is the prior wielding membership authority under a mode that promises none.
 *
 * FIXTURE (genuine 2-root circuit): sources X1,X2; goals G1,G2.
 *   O : X1->G1, reliability HIGH (s=100,f=0)  -- the optimal G1 completer
 *   W : X1->G1, reliability LOW  (s=0,f=100)   -- a valid but worse G1 completer
 *   B : X2->G2, fresh                          -- the only G2 completer
 *   d1: Y ->G1, non-completing (input tag "y" unsatisfiable)
 * attention_prune_k = 3, viable pairs np = 4 (O,W,d1 -> G1; B -> G2) -> the
 * pair set is saturated (np > k). The artifact boosts W,B,d1 (B kept so G2 still
 * completes -> no fallback); that fills the top-3 and evicts O.
 *
 * EXPECTATION: unbiased (OFF / ORDER_ONLY-no-artifact) selects O for G1; today's
 * ORDER_ONLY+artifact selects W (O pruned). Step (4)'s invariant ("ORDER_ONLY
 * does not change the reliability-optimal root") is the RED gate; it goes GREEN
 * once enforce gates g_circuit_prune_active to PRUNE_WITH_FALLBACK only.
 *
 * TEST-ONLY: plans on a scalar COPY of the registry (entries + BTNs shared);
 * planning never writes BTN counters, so the production reg is untouched.
 */
#include "structural_pref_common.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

#define CKT_W 4
#define CKT_K 3   /* attention_prune_k: top-3 of 4 viable pairs -> saturated */

typedef struct {
    BinaryTransformNetwork prod[4];   /* 0=O, 1=W, 2=B, 3=d1 */
    PrimitiveRegistry reg;
    Port g1, g2;
    Port sx1, sx2;
    double v1[CKT_W], v2[CKT_W];
    DagSource sources[2];
    Port goals[2];
    int built;
} CktFixture;

/* Borrowed (program-lifetime) registry names. */
static const char *const CKT_NAMES[4] = { "O", "W", "B", "d1" };

/* A producer in_tag -> out_tag (ONEHOT CKT_W), untrained. 0 / -1. */
static int ckt_build_prod(BinaryTransformNetwork *b, const char *in_tag, const char *out_tag) {
    Port in  = { PORT_ONEHOT, CKT_W, 1, "" };
    Port out = { PORT_ONEHOT, CKT_W, 1, "" };
    port_set_tag(&in, in_tag);
    port_set_tag(&out, out_tag);
    memset(b, 0, sizeof *b);
    if (btn_init(b, CKT_W, CKT_W, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, in, out);
}

/* Set the learned-reliability counters (btn_reliability = (s+1)/(s+f+2)). The
   fields are _Atomic; plain assignment is a C11 atomic store. */
static void ckt_set_rel(BinaryTransformNetwork *b, unsigned long s, unsigned long f) {
    b->output_successes = s;
    b->output_failures = f;
}

static void ckt_free(CktFixture *f) {
    size_t i;
    if (f->built) {
        registry_free(&f->reg);
    }
    for (i = 0; i < 4; ++i) {
        btn_free(&f->prod[i]);
    }
}

static int ckt_build(CktFixture *f) {
    size_t i;
    memset(f, 0, sizeof *f);

    if (ckt_build_prod(&f->prod[0], "x1", "g1") != 0) return -1;   /* O */
    if (ckt_build_prod(&f->prod[1], "x1", "g1") != 0) { btn_free(&f->prod[0]); return -1; }  /* W */
    if (ckt_build_prod(&f->prod[2], "x2", "g2") != 0) { btn_free(&f->prod[0]); btn_free(&f->prod[1]); return -1; }  /* B */
    if (ckt_build_prod(&f->prod[3], "y",  "g1") != 0) { btn_free(&f->prod[0]); btn_free(&f->prod[1]); btn_free(&f->prod[2]); return -1; }  /* d1 */

    ckt_set_rel(&f->prod[0], 100, 0);    /* O: reliability ~0.990 (optimal) */
    ckt_set_rel(&f->prod[1], 0, 100);    /* W: reliability ~0.0098 (worse)  */
    /* B, d1 fresh (0,0 -> 0.5) */

    f->g1  = (Port){ PORT_ONEHOT, CKT_W, 1, "" };  port_set_tag(&f->g1,  "g1");
    f->g2  = (Port){ PORT_ONEHOT, CKT_W, 1, "" };  port_set_tag(&f->g2,  "g2");
    f->sx1 = (Port){ PORT_ONEHOT, CKT_W, 1, "" };  port_set_tag(&f->sx1, "x1");
    f->sx2 = (Port){ PORT_ONEHOT, CKT_W, 1, "" };  port_set_tag(&f->sx2, "x2");
    for (i = 0; i < CKT_W; ++i) { f->v1[i] = (i == 0) ? 1.0 : 0.0; f->v2[i] = (i == 0) ? 1.0 : 0.0; }
    f->sources[0].type = f->sx1; f->sources[0].values = f->v1;
    f->sources[1].type = f->sx2; f->sources[1].values = f->v2;
    f->goals[0] = f->g1; f->goals[1] = f->g2;

    registry_init(&f->reg);
    for (i = 0; i < 4; ++i) registry_add(&f->reg, &f->prod[i], CKT_NAMES[i]);
    f->reg.attention_prune_k = CKT_K;
    f->built = 1;
    return 0;
}

/* Build a frozen ORDER_ONLY artifact boosting W, B, d1 by `prior`. B is boosted
   so G2 still completes under the restriction (no fallback); W+d1 fill the rest
   of the top-3 and evict O. */
static void ckt_build_artifact(CircuitRankArtifact *a, double prior) {
    static const char *const boosted[3] = { "W", "B", "d1" };
    size_t i;
    circuit_rank_artifact_init(a);
    for (i = 0; i < 3; ++i) {
        CircuitRankArtifactRow *r = &a->rows[i];
        memset(r, 0, sizeof *r);
        r->task_key[0] = '\0';
        snprintf(r->producer_name, sizeof r->producer_name, "%s", boosted[i]);
        r->producer_output_port = 0;
        r->rank_prior = prior;
        r->order_only = 1;
        r->no_prune_authority = 1;
        r->no_cert_authority = 1;
        r->no_registry_authority = 1;
    }
    a->row_count = 3;
    a->order_only = 1;
    a->frozen = 1;
}

/* Plan the circuit under (mode, artifact) on a scalar COPY; copy the root-0
   (goal G1) producer name to out. Returns 1 if a complete circuit planned with
   a DAG_PRIMITIVE G1 root, else 0/-1. */
static int ckt_root0(const PrimitiveRegistry *reg, const DagSource *sources, size_t ns,
                     const Port *goals, size_t ng, CNETAttentionMode mode,
                     const CircuitRankArtifact *art, char *out, size_t cap) {
    PrimitiveRegistry copy = *reg;
    CircuitPlan cp;
    int rc, ret = 0;

    if (out && cap) out[0] = '\0';
    copy.attention_mode = mode;
    copy.rank_artifact = art;
    memset(&cp, 0, sizeof cp);
    rc = dag_plan_circuit(&copy, sources, ns, goals, ng, &cp);
    if (rc == 0 && cp.root_count == ng && cp.roots[0] != NULL &&
        cp.roots[0]->kind == DAG_PRIMITIVE && cp.roots[0]->name != NULL) {
        strncpy(out, cp.roots[0]->name, cap - 1);
        out[cap - 1] = '\0';
        ret = 1;
    }
    circuit_free(&cp);
    return ret;
}

static void test_adversarial_circuit_prune(void) {
    CktFixture f;
    CircuitRankArtifact art;
    char off[32] = "", ctrl[32] = "", biased[32] = "";
    unsigned long s_before[4], f_before[4], s_after[4], f_after[4];
    size_t i;
    int rc, locked;
    static const double SWEEP[] = { 0.1, 0.3, 1.0, 1000.0 };

    printf("structural_pref_adversarial_circuit: multi-root pair-prune probe "
           "(2 goals, prune_k=%d, 4 pairs):\n", CKT_K);

    if (ckt_build(&f) != 0) {
        CHECK(0, "fixture: 2-root saturated-pair circuit builds");
        ckt_free(&f);
        return;
    }
    CHECK(f.reg.count == 4, "registry: O,W,B,d1 (4 producers)");
    CHECK(f.reg.attention_prune_k == (size_t)CKT_K,
          "prune_k=3 over 4 viable pairs -> SATURATED pair set");

    for (i = 0; i < 4; ++i) {
        s_before[i] = (unsigned long)f.prod[i].output_successes;
        f_before[i] = (unsigned long)f.prod[i].output_failures;
    }

    /* (1) OFF: full search, reliability-weighted -> G1 root = O (the optimal
       completer; rel 0.99 vs W 0.0098). The circuit is SOLVABLE with O. */
    rc = ckt_root0(&f.reg, f.sources, 2, f.goals, 2, CNET_ATTENTION_OFF, NULL, off, sizeof off);
    CHECK(rc == 1 && strcmp(off, "O") == 0,
          "(1) OFF: G1 root is the reliability-optimal completer O");

    /* (2) CONTROL: ORDER_ONLY + no artifact -> do_pair_ranking is false, no
       restriction, full search -> still O. Isolates the prior from the mode. */
    rc = ckt_root0(&f.reg, f.sources, 2, f.goals, 2, CNET_ATTENTION_ORDER_ONLY, NULL, ctrl, sizeof ctrl);
    CHECK(rc == 1 && strcmp(ctrl, "O") == 0,
          "(2) CONTROL ORDER_ONLY + no artifact: G1 root still O "
          "(mode alone does not restrict the pair set)");

    /* (3) Prior-threshold sweep (informational): G1 root under ORDER_ONLY +
       artifact boosting W,B,d1. "O" = optimal kept; anything else = O pruned. */
    printf("  threshold sweep (boost W,B,d1; G1 root under ORDER_ONLY+artifact):\n");
    for (i = 0; i < sizeof(SWEEP) / sizeof(SWEEP[0]); ++i) {
        CircuitRankArtifact a;
        char nm[32] = "";
        ckt_build_artifact(&a, SWEEP[i]);
        rc = ckt_root0(&f.reg, f.sources, 2, f.goals, 2, CNET_ATTENTION_ORDER_ONLY, &a, nm, sizeof nm);
        printf("    rank_prior=%-8.4g -> G1 root=%-4s [%s]\n", SWEEP[i],
               (rc == 1 ? nm : "<NONE>"),
               (rc == 1 && strcmp(nm, "O") == 0) ? "O kept" : "O PRUNED -> suboptimal lock");
    }

    /* (4) THE MEMBERSHIP-GUARD INVARIANT at the PRODUCTION prior (1.0). Today the
       prior-biased top-3 = {W,B,d1} evicts O; the restricted pass completes with
       W (no fallback) and locks in the worse plan. A reorder could never do this
       (branch-and-bound is reliability-weighted) -> proof of a membership prune.
       GREEN once g_circuit_prune_active is gated to PRUNE_WITH_FALLBACK only. */
    ckt_build_artifact(&art, 1.0);
    rc = ckt_root0(&f.reg, f.sources, 2, f.goals, 2, CNET_ATTENTION_ORDER_ONLY, &art, biased, sizeof biased);
    locked = !(rc == 1 && strcmp(biased, "O") == 0);
    printf("  [OFF=%s | CONTROL(no-art)=%s | ORDER_ONLY+artifact@1.0=%s]\n",
           off, ctrl, (biased[0] ? biased : "<NONE>"));
    CHECK(rc == 1 && strcmp(biased, "O") == 0,
          "(4) ORDER_ONLY prior at the PRODUCTION value (1.0) does NOT prune the "
          "reliability-optimal G1 completer O (membership guard enforced)");

    /* (5) footprint: planning is read-only over BTN counters -> reliability
       evidence is byte-identical after the whole matrix. */
    for (i = 0; i < 4; ++i) {
        s_after[i] = (unsigned long)f.prod[i].output_successes;
        f_after[i] = (unsigned long)f.prod[i].output_failures;
    }
    {
        int unchanged = 1;
        for (i = 0; i < 4; ++i)
            if (s_after[i] != s_before[i] || f_after[i] != f_before[i]) unchanged = 0;
        CHECK(unchanged && f.reg.count == 4,
              "(5) BTN reliability counters + registry byte-identical after the "
              "matrix (planning is footprint-free)");
    }

    if (locked) {
        printf("  ==> SUBOPTIMAL-LOCK: an ORDER_ONLY artifact pruned the optimal "
               "completer O from the root-pair set; the worse completer W was "
               "locked in (fallback could not catch it -- W is a valid plan).\n");
    } else {
        printf("  ==> GUARD HOLDS: the prior reordered exploration but the "
               "root-pair SET stayed prior-independent; the optimal completer O "
               "was selected.\n");
    }

    ckt_free(&f);
}

int run_test_structural_pref_adversarial_circuit(void) {
    failures = 0;
    test_adversarial_circuit_prune();
    if (failures == 0) {
        printf("STRUCTURAL_PREF_ADVERSARIAL_CIRCUIT PASS\n");
        return 0;
    }
    printf("STRUCTURAL_PREF_ADVERSARIAL_CIRCUIT FAIL: %d checks failed "
           "(a (4) failure means the circuit ORDER_ONLY membership guard regressed).\n",
           failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) {
    return run_test_structural_pref_adversarial_circuit();
}
#endif
