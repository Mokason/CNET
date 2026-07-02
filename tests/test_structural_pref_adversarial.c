/*
 * test_structural_pref_adversarial.c -- the SATURATED-BEAM PRUNE probe.
 *
 * Claim under test (the audit prediction): the v2.2 ORDER_ONLY rank-artifact
 * channel is advertised as "order-only / no_prune_authority", but that property
 * is NOT enforced -- it only holds because the test fixtures live in an
 * UNSATURATED beam regime (#competitors <= beam_limit). Under a SATURATED beam
 * (#competitors > beam_limit) the additive prior reorders order[], and the beam
 * cutoff (src/router.c:2164) keeps only the first beam_limit of that reordered
 * list. Reorder and membership are then the SAME operation: a prior that lifts
 * beam_limit non-completers ahead of the sole completer EVICTS the completer
 * from the considered window -> a SOLVABLE single-root task becomes UNSOLVABLE.
 * That is an advisory ordering bias weaponized into a structural deletion.
 *
 * Prior magnitude (a prediction this test FALSIFIED): the audit predicted that
 * the reach-aware multihead source-sat head (src/router.c:2317-2324) would
 * front-load genuine completers, so that ONLY a large adversarial prior could
 * evict one. The run disproved that: even rank_prior=1.0 -- the EXACT value
 * structural_pref_build_artifact bakes into real B0 artifacts
 * (tests/structural_pref_common.h:582) -- prunes the sole completer under a
 * saturated beam. The head's advisory gap between a source-satisfiable completer
 * and a non-completer is SMALLER than the additive prior (advisory_score is
 * O(0.1-0.3) here; the prior adds rank_prior*0.3), so a production-magnitude
 * prior already overpowers it. The control step (ORDER_ONLY + NULL artifact ->
 * R survives) proves the PRIOR, not the mode-switch, does the eviction. So this
 * is not a theoretical large-prior edge: it triggers at the prior the live B0
 * path already emits, gated only by beam saturation. rank_prior is also
 * UNBOUNDED frozen JSON (circuit_rank_artifact_load_json, no clamp), so a stale
 * or adversarial artifact makes it strictly worse.
 *
 * TEST-ONLY: no src/ edits. Builds public structs and sets public registry
 * knobs on COPIES (via spc_selected_producer); the production reg is untouched.
 *
 * HISTORY: step (4)'s invariant ("ORDER_ONLY does NOT prune the sole completer")
 * was RED until the membership guard landed -- enforce_order_only_no_prune in
 * src/router.c admits the beam window on the UNBIASED order, then lets the prior
 * re-rank only WITHIN it. It is now a GREEN regression guard: a (4) failure means
 * the guard has regressed and an advisory ORDER_ONLY prior can again prune.
 */
#include "structural_pref_common.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

#define ADV_N 10        /* producers: 1 completer (R) + 9 non-completing decoys */
#define ADV_BEAM 4      /* saturated: 10 type-compatible competitors > beam 4 */

/* Stable (program-lifetime) names: registry_add BORROWS the name pointer
   (see the byte-identical pointer compare in test_structural_pref_b0.c), so a
   local buffer would dangle. Index 0 = R, indices 1..9 = d1..d9. */
static const char *const ADV_NAMES[ADV_N] = {
    "R", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9"
};

/* The four decoys the adversarial artifact boosts (= beam width). Lifting
   exactly beam_limit non-completers ahead of R pushes R to order-position 4,
   one past the beam boundary -> evicted. */
static const char *const ADV_BOOSTED[ADV_BEAM] = { "d4", "d5", "d6", "d7" };

typedef struct {
    BinaryTransformNetwork producers[ADV_N];
    size_t producer_count;
    PrimitiveRegistry reg;
    Port goal;
    Port src_type;
    double src_values[SPC_X_WIDTH];
    DagSource sources[1];
    int built;
} AdvFixture;

/* A non-completing decoy: output ONEHOT 4 "gl" (matches the goal -> selectable
   as a plan root) but input ONEHOT 4 "y" -- a tag NO source provides and NO
   producer outputs, so its single slot is unreachable (reach == REACH_INF) and
   the decoy can never complete. It still consumes a beam slot when chosen as a
   root, then dead-ends in the child obligation. Returns 0 / -1. */
static int adv_build_decoy(BinaryTransformNetwork *b) {
    Port in  = { PORT_ONEHOT, SPC_X_WIDTH, 1, "" };
    Port out = { PORT_ONEHOT, SPC_X_WIDTH, 1, "" };
    port_set_tag(&in, "y");
    port_set_tag(&out, "gl");
    memset(b, 0, sizeof *b);
    if (btn_init(b, SPC_X_WIDTH, SPC_X_WIDTH, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, in, out);
}

/* Fill the shared single-root task: source X = ONEHOT 4 "x" (one-hot at 0),
   goal G = ONEHOT 4 "gl". Source tag != goal tag, so a producer is required. */
static void adv_set_task(AdvFixture *f) {
    size_t i;
    f->goal     = (Port){ PORT_ONEHOT, SPC_X_WIDTH, 1, "" };
    f->src_type = (Port){ PORT_ONEHOT, SPC_X_WIDTH, 1, "" };
    port_set_tag(&f->goal, "gl");
    port_set_tag(&f->src_type, "x");
    for (i = 0; i < SPC_X_WIDTH; ++i) {
        f->src_values[i] = (i == 0) ? 1.0 : 0.0;
    }
    f->sources[0].type   = f->src_type;
    f->sources[0].values = f->src_values;
}

static void adv_fixture_free(AdvFixture *f) {
    size_t i;
    if (f->built) {
        registry_free(&f->reg);
    }
    for (i = 0; i < f->producer_count; ++i) {
        btn_free(&f->producers[i]);
    }
}

/* Build the full fixture: index 0 = R (X->G, the SOLE completer, input "x" is
   satisfied directly by the source); indices 1..9 = non-completing decoys.
   Beam set to 4 (saturated). Returns 0 / -1 (frees partial builds on failure). */
static int adv_build(AdvFixture *f) {
    size_t i;
    memset(f, 0, sizeof *f);

    /* R: input "x" (source-satisfiable), output "gl" (matches goal). */
    if (spc_build_producer(&f->producers[0], "gl") != 0) {
        return -1;
    }
    f->producer_count = 1;
    for (i = 1; i < ADV_N; ++i) {
        if (adv_build_decoy(&f->producers[i]) != 0) {
            adv_fixture_free(f);
            return -1;
        }
        f->producer_count = i + 1;
    }

    adv_set_task(f);

    registry_init(&f->reg);
    for (i = 0; i < ADV_N; ++i) {
        registry_add(&f->reg, &f->producers[i], ADV_NAMES[i]);
    }
    f->reg.dag_beam_limit = ADV_BEAM;   /* public field; saturated beam */
    f->built = 1;
    return 0;
}

/* An R-only registry over the same task: proves R is intrinsically buildable, so
   any later failure-to-find-R is an EVICTION, not an unbuildable goal. 0 / -1. */
static int adv_build_r_only(AdvFixture *f) {
    memset(f, 0, sizeof *f);
    if (spc_build_producer(&f->producers[0], "gl") != 0) {
        return -1;
    }
    f->producer_count = 1;
    adv_set_task(f);
    registry_init(&f->reg);
    registry_add(&f->reg, &f->producers[0], ADV_NAMES[0]);
    f->reg.dag_beam_limit = ADV_BEAM;
    f->built = 1;
    return 0;
}

/* A frozen ORDER_ONLY artifact that additively boosts the four ADV_BOOSTED
   decoys by `prior`. Keyed (task_key="", output_port=0) to match the planner's
   lookup (src/router.c:949). Declares no_*_authority=1 -- the label whose
   enforcement this test probes. */
static void adv_build_artifact(CircuitRankArtifact *a, double prior) {
    size_t i;
    circuit_rank_artifact_init(a);
    for (i = 0; i < ADV_BEAM; ++i) {
        CircuitRankArtifactRow *row = &a->rows[i];
        memset(row, 0, sizeof *row);
        row->task_key[0] = '\0';                  /* planner lookup key */
        snprintf(row->producer_name, sizeof row->producer_name, "%s",
                 ADV_BOOSTED[i]);
        row->producer_output_port = 0;            /* planner lookup key */
        row->rank_prior = prior;                  /* additive bias */
        row->order_only = 1;
        row->no_prune_authority = 1;
        row->no_cert_authority = 1;
        row->no_registry_authority = 1;
    }
    a->row_count = ADV_BEAM;
    a->order_only = 1;
    a->frozen = 1;
}

static void test_adversarial_prune(void) {
    AdvFixture f, r_only;
    CircuitRankArtifact art_ctrl_unused, art_prod;
    char name_off[64] = "", name_ctrl[64] = "", name_prod[64] = "", name_solo[64] = "";
    int rc, pruned;
    size_t si;
    static const double SWEEP[] = { 0.1, 0.3, 1.0, 1000.0 };

    (void)art_ctrl_unused;

    printf("structural_pref_adversarial: saturated-beam prune probe "
           "(N=%d producers, beam=%d):\n", ADV_N, ADV_BEAM);

    if (adv_build(&f) != 0) {
        CHECK(0, "fixture: 10-producer saturated-beam registry builds");
        adv_fixture_free(&f);
        return;
    }
    CHECK(f.reg.count == (size_t)ADV_N,
          "registry has 10 producers (1 completer R + 9 non-completing decoys)");
    CHECK(f.reg.dag_beam_limit == (size_t)ADV_BEAM,
          "beam limit is 4 -> SATURATED (10 type-compatible competitors > 4)");

    /* (1) OFF baseline: order[] = registry order, window = {R,d1,d2,d3}; R is
       the only one that completes -> the task is SOLVABLE and R is selected. */
    rc = spc_selected_producer(&f.reg, f.sources, 1, f.goal,
                               CNET_ATTENTION_OFF, NULL, name_off, sizeof name_off);
    CHECK(rc == 1 && strcmp(name_off, "R") == 0,
          "(1) OFF: task is SOLVABLE -- planner selects the sole completer R");

    /* (2) CONTROL -- ORDER_ONLY mode ON, but NO artifact. Isolates the
       mode-switch (rank_by_reliability -> rank_by_multihead) from the prior.
       Multihead ranks the source-satisfiable completer R above the source-
       unsatisfiable decoys, so R stays in the beam window and survives. If THIS
       fails, the eviction would be the mode's fault, not the prior's -- so this
       passing is what makes (4) a clean attribution to the artifact. */
    rc = spc_selected_producer(&f.reg, f.sources, 1, f.goal,
                               CNET_ATTENTION_ORDER_ONLY, NULL,
                               name_ctrl, sizeof name_ctrl);
    CHECK(rc == 1 && strcmp(name_ctrl, "R") == 0,
          "(2) CONTROL ORDER_ONLY + no artifact: R survives "
          "(mode-switch alone does NOT prune -- so any prune below is the PRIOR)");

    /* (3) Prior-threshold sweep (informational): how benign a frozen prior still
       evicts the sole completer. Each entry boosts the four decoys by that prior
       and reports the resulting pick. Shows the head-protection floor. */
    printf("  threshold sweep (4 decoys boosted; pick under ORDER_ONLY+artifact):\n");
    for (si = 0; si < sizeof(SWEEP) / sizeof(SWEEP[0]); ++si) {
        CircuitRankArtifact a;
        char nm[64] = "";
        adv_build_artifact(&a, SWEEP[si]);
        rc = spc_selected_producer(&f.reg, f.sources, 1, f.goal,
                                   CNET_ATTENTION_ORDER_ONLY, &a, nm, sizeof nm);
        printf("    rank_prior=%-8.4g -> pick=%-6s [%s]\n", SWEEP[si],
               (rc == 1 ? nm : "<NONE>"),
               (rc == 1 && strcmp(nm, "R") == 0) ? "R kept" : "R PRUNED");
    }

    /* (4) THE MEMBERSHIP-GUARD INVARIANT at the PRODUCTION prior. rank_prior=1.0
       is exactly what structural_pref_build_artifact emits
       (structural_pref_common.h:582). Without the guard the four boosted decoys
       fill the beam-4 window and evict R (solvable -> unsolvable); WITH
       enforce_order_only_no_prune the window is admitted on the unbiased order,
       so R stays admitted and the prior only re-ranks within it. GREEN now;
       a failure here = the guard regressed. */
    adv_build_artifact(&art_prod, 1.0);
    rc = spc_selected_producer(&f.reg, f.sources, 1, f.goal,
                               CNET_ATTENTION_ORDER_ONLY, &art_prod,
                               name_prod, sizeof name_prod);
    pruned = !(rc == 1 && strcmp(name_prod, "R") == 0);
    printf("  [OFF pick=%s | CONTROL(no-art) pick=%s | "
           "ORDER_ONLY+artifact@1.0 rc=%d pick=%s]\n",
           name_off, name_ctrl, rc, (name_prod[0] ? name_prod : "<NONE>"));
    CHECK(rc == 1 && strcmp(name_prod, "R") == 0,
          "(4) ORDER_ONLY prior at the PRODUCTION value (1.0) does NOT prune the "
          "sole completer (membership guard enforced)");

    /* (4) R is intrinsically buildable (plans in an R-only registry), so (3) was
       an EVICTION by the beam, not an unbuildable goal -- a true prune. And the
       production registry still holds all 10 producers: the deletion is in the
       SEARCH, not the registry (so the byte-identical-footprint claim is true
       AND beside the point). */
    if (adv_build_r_only(&r_only) != 0) {
        CHECK(0, "(5) fixture: R-only registry builds");
    } else {
        rc = spc_selected_producer(&r_only.reg, r_only.sources, 1, r_only.goal,
                                   CNET_ATTENTION_OFF, NULL,
                                   name_solo, sizeof name_solo);
        CHECK(rc == 1 && strcmp(name_solo, "R") == 0,
              "(5) R plans in isolation -> (4) was an EVICTION/prune, not an "
              "unbuildable goal");
        adv_fixture_free(&r_only);
    }
    CHECK(f.reg.count == (size_t)ADV_N,
          "(5) registry still has all 10 producers -> the prune is in the SEARCH, "
          "not the registry (footprint-free yet still a deletion)");

    if (pruned) {
        printf("  ==> REGRESSED: an advisory ORDER_ONLY artifact evicted the sole "
               "completer from the saturated beam -- the membership guard is "
               "broken.\n");
    } else {
        printf("  ==> GUARD HOLDS: the prior re-ranked within the admitted window "
               "but could not evict the sole completer (membership preserved).\n");
    }

    adv_fixture_free(&f);
}

int run_test_structural_pref_adversarial(void) {
    failures = 0;
    test_adversarial_prune();
    if (failures == 0) {
        printf("STRUCTURAL_PREF_ADVERSARIAL PASS\n");
        return 0;
    }
    printf("STRUCTURAL_PREF_ADVERSARIAL FAIL: %d checks failed "
           "(a (4) failure means the ORDER_ONLY membership guard regressed).\n",
           failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) {
    return run_test_structural_pref_adversarial();
}
#endif
