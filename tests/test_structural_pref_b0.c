/*
 * test_structural_pref_b0.c -- the gate-probe ANCHOR for Structural Preference
 * B0: an opt-in ORDER_ONLY rank-artifact traversal bias.
 * (spec: docs/superpowers/specs/2026-06-19-structural-preference-B0-gate-probe-design.md)
 *
 * A (SHADOW_PLUS_RECOMMEND, planner_influence=0) is done: the sidecar RANKS
 * already-valid structures, planner untouched. B0 probes whether A's ranking can
 * be FROZEN into a CircuitRankArtifact and consumed through the EXISTING v2.2
 * ORDER_ONLY path as an opt-in ADDITIVE traversal bias that flips the lure's
 * selected producer alt_a -> alt_b (toward the attractor A recommends) -- with:
 *   - the full candidate set preserved (alt_a deprioritized, NOT pruned),
 *   - zero authority (no prune/cert/registry authority),
 *   - default runtime unchanged (attention_mode stays OFF; the probe sets
 *     ORDER_ONLY only on a local non-const COPY, never the production reg),
 *   - the production registry + BTN counters byte-identical before/after.
 *
 * It is a FALSIFIABLE probe. If ORDER_ONLY+artifact does NOT flip the pick, the
 * anchor records the honest finding (the existing channel can't bias single-root
 * selection as-is -> B1 must do planner work) rather than asserting a falsehood.
 *
 * Keying limitation (reported, not hidden): the planner's artifact lookup passes
 * task_key="" / output_port=0 and IGNORES root_goal/output_sig
 * (src/router.c:949,1720-1728), so the bias is per-producer-name GLOBAL, not
 * per-task. B1 is where per-task keying gets solved.
 *
 * TEST-ONLY: no src/ edits. B0 only SETS public registry knobs
 * (attention_mode, rank_artifact) on a copy and populates the public
 * CircuitRankArtifact struct.
 */
#include "structural_pref_common.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* Snapshot of the authority-bearing registry state for the byte-identical
   no-footprint compare (mirrors test_structural_pref.c's RegSnap pattern;
   captures the borrowed pointers + BTN counters too). */
typedef struct {
    const char *name;
    const BinaryTransformNetwork *btn;
    PrimitiveState state;
    int certified;
    int queue_is_null;
    const char *shadow_of;
    unsigned long output_successes;
    unsigned long output_failures;
} EntrySnap;

#define B0_MAX_SNAP 8

typedef struct {
    size_t count;
    size_t dag_beam_limit;
    int disable_plan_memo;
    CNETAttentionMode attention_mode;
    const CircuitRankArtifact *rank_artifact;
    EntrySnap entries[B0_MAX_SNAP];
} RegSnap;

static void snapshot_registry(const PrimitiveRegistry *reg, RegSnap *out) {
    size_t i;
    out->count = reg->count;
    out->dag_beam_limit = reg->dag_beam_limit;
    out->disable_plan_memo = reg->disable_plan_memo;
    out->attention_mode = reg->attention_mode;
    out->rank_artifact = reg->rank_artifact;
    for (i = 0; i < reg->count && i < B0_MAX_SNAP; ++i) {
        const RegistryEntry *e = &reg->entries[i];
        out->entries[i].name = e->name;
        out->entries[i].btn = e->btn;
        out->entries[i].state = e->state;
        out->entries[i].certified = e->certified;
        out->entries[i].queue_is_null = (e->queue == NULL);
        out->entries[i].shadow_of = e->shadow_of;
        out->entries[i].output_successes = (unsigned long)e->btn->output_successes;
        out->entries[i].output_failures = (unsigned long)e->btn->output_failures;
    }
}

static int reg_snaps_identical(const RegSnap *a, const RegSnap *b) {
    size_t i;
    if (a->count != b->count) return 0;
    if (a->dag_beam_limit != b->dag_beam_limit) return 0;
    if (a->disable_plan_memo != b->disable_plan_memo) return 0;
    if (a->attention_mode != b->attention_mode) return 0;
    if (a->rank_artifact != b->rank_artifact) return 0;
    for (i = 0; i < a->count && i < B0_MAX_SNAP; ++i) {
        if (a->entries[i].name != b->entries[i].name) return 0;
        if (a->entries[i].btn != b->entries[i].btn) return 0;
        if (a->entries[i].state != b->entries[i].state) return 0;
        if (a->entries[i].certified != b->entries[i].certified) return 0;
        if (a->entries[i].queue_is_null != b->entries[i].queue_is_null) return 0;
        if (a->entries[i].shadow_of != b->entries[i].shadow_of) return 0;
        if (a->entries[i].output_successes != b->entries[i].output_successes) return 0;
        if (a->entries[i].output_failures != b->entries[i].output_failures) return 0;
    }
    return 1;
}

/* Build a one-producer "alt_a" registry over the same goal/sources as the lure,
   to prove alt_a still PLANS in isolation (deprioritized, never pruned). The
   caller frees via spc_fixture_free. Returns 0 on success, -1 on a build fail. */
static int build_alt_a_only(SpcFixture *f) {
    memset(f, 0, sizeof *f);
    if (spc_build_producer(&f->producers[0], "gl") != 0) {
        return -1;
    }
    f->producer_count = 1;
    spc_set_task(f, "gl");
    registry_init(&f->reg);
    registry_add(&f->reg, &f->producers[0], "alt_a");
    f->built = 1;
    return 0;
}

static void test_b0_gate_probe(void) {
    SpcFixture lure, alt_a_only;
    SpcRanked r;
    CircuitRankArtifact artifact;
    RegSnap before, after;
    char name0[64] = "";       /* OFF default pick */
    char nameP0[64] = "";      /* ORDER_ONLY, NO artifact pick */
    char nameON[64] = "";      /* ORDER_ONLY + artifact pick */
    char nameRestore[64] = ""; /* final OFF/NULL pick */
    char rank0_name[64] = "";  /* the artifact's preferred (rank-0) producer */
    int rc, flipped = 0;

    printf("structural_pref_b0: opt-in ORDER_ONLY gate probe (LURE alt_a/alt_b):\n");

    if (spc_make_lure(&lure) != 0) {
        CHECK(0, "fixture: lure builds");
        spc_fixture_free(&lure);
        return;
    }
    CHECK(lure.reg.count == 2, "lure registry has 2 distinct-named producers");
    CHECK(lure.reg.attention_mode == CNET_ATTENTION_OFF,
          "lure registry default attention_mode is OFF (registry_init)");
    CHECK(lure.reg.rank_artifact == NULL,
          "lure registry default rank_artifact is NULL (registry_init)");

    /* Footprint baseline: snapshot the production reg BEFORE the whole matrix. */
    snapshot_registry(&lure.reg, &before);

    /* (1) OFF default: the registry-default tiebreak pick (expect alt_a). */
    rc = spc_selected_producer(&lure.reg, lure.sources, 1, lure.goal,
                               CNET_ATTENTION_OFF, NULL, name0, sizeof name0);
    CHECK(rc == 1, "(1) OFF: dag_plan selects a DAG_PRIMITIVE root");
    CHECK(strcmp(name0, "alt_a") == 0,
          "(1) OFF: default pick is alt_a (registry-default tiebreak)");
    CHECK(lure.reg.attention_mode == CNET_ATTENTION_OFF,
          "(1) production reg attention_mode still OFF (copy-only, default unchanged)");

    /* (2) Build the artifact from A's ranking (rank-0 -> producer name). */
    rc = structural_pref_rank(&lure.reg, lure.sources, 1, lure.goal, &r);
    CHECK(rc == 0, "(2) structural_pref_rank returns 0");
    rc = structural_pref_build_artifact(&lure.reg, lure.sources, 1, lure.goal,
                                        &r, &artifact);
    CHECK(rc == 0, "(2) structural_pref_build_artifact returns 0");
    CHECK(artifact.row_count == 1, "(2) artifact has exactly one row");
    CHECK(artifact.order_only == 1, "(2) artifact order_only flag set");
    CHECK(artifact.frozen == 1, "(2) artifact frozen flag set");
    CHECK(artifact.rows[0].task_key[0] == '\0',
          "(2) artifact row task_key == \"\" (planner lookup key)");
    CHECK(artifact.rows[0].producer_output_port == 0,
          "(2) artifact row producer_output_port == 0 (planner lookup key)");
    CHECK(artifact.rows[0].no_prune_authority == 1 &&
          artifact.rows[0].no_cert_authority == 1 &&
          artifact.rows[0].no_registry_authority == 1,
          "(2) artifact row carries NO prune/cert/registry authority");
    CHECK(artifact.rows[0].rank_prior > 0.0,
          "(2) artifact row rank_prior is a positive (additive) bias");
    strncpy(rank0_name, artifact.rows[0].producer_name, sizeof rank0_name - 1);
    rank0_name[sizeof rank0_name - 1] = '\0';
    CHECK(strcmp(rank0_name, "alt_b") == 0,
          "(2) artifact prefers rank-0 producer alt_b (the attractor)");

    /* (3) ORDER_ONLY + NO artifact: isolate mode-on from the artifact's effect. */
    rc = spc_selected_producer(&lure.reg, lure.sources, 1, lure.goal,
                               CNET_ATTENTION_ORDER_ONLY, NULL,
                               nameP0, sizeof nameP0);
    CHECK(rc == 1, "(3) ORDER_ONLY+no-artifact: selects a DAG_PRIMITIVE root");

    /* (4) ORDER_ONLY + artifact: the flip. */
    rc = spc_selected_producer(&lure.reg, lure.sources, 1, lure.goal,
                               CNET_ATTENTION_ORDER_ONLY, &artifact,
                               nameON, sizeof nameON);
    CHECK(rc == 1, "(4) ORDER_ONLY+artifact: selects a DAG_PRIMITIVE root");
    flipped = (strcmp(nameON, rank0_name) == 0 && strcmp(nameON, name0) != 0);
    if (flipped) {
        CHECK(strcmp(nameON, rank0_name) == 0,
              "(4) ORDER_ONLY+artifact: pick == artifact's preferred producer (alt_b)");
        CHECK(strcmp(nameON, name0) != 0,
              "(4) ORDER_ONLY+artifact: pick FLIPPED away from the OFF default (alt_a)");
    } else {
        /* FALSIFIABLE finding: do NOT fake the check; report what actually happened. */
        printf("  DIAGNOSTIC (no flip): OFF default=%s | ORDER_ONLY+artifact actually=%s | "
               "artifact preferred=%s\n", name0, nameON, rank0_name);
        printf("  DIAGNOSTIC: the existing v2.2 ORDER_ONLY channel did NOT bias the "
               "single-root selection toward the artifact -> B1 (planner work) needed.\n");
        CHECK(0, "(4) ORDER_ONLY+artifact: pick flips to the artifact's preferred producer "
                 "[FALSIFIABLE -- did not flip; see DIAGNOSTIC]");
    }

    /* (5) Reachability: alt_a still plans from an alt_a-only registry (proves it
       was deprioritized, not pruned); the lure reg still has both producers. */
    if (build_alt_a_only(&alt_a_only) != 0) {
        CHECK(0, "(5) fixture: alt_a-only registry builds");
    } else {
        char solo[64] = "";
        int prc = spc_selected_producer(&alt_a_only.reg, alt_a_only.sources, 1,
                                        alt_a_only.goal, CNET_ATTENTION_OFF, NULL,
                                        solo, sizeof solo);
        CHECK(prc == 1 && strcmp(solo, "alt_a") == 0,
              "(5) alt_a still plans from an alt_a-only registry (deprioritized, not pruned)");
        spc_fixture_free(&alt_a_only);
    }
    CHECK(lure.reg.count == 2,
          "(5) lure registry still has both producers (count == 2, no prune)");

    /* (7) Restore: a final OFF/NULL selection reproduces the old default. */
    rc = spc_selected_producer(&lure.reg, lure.sources, 1, lure.goal,
                               CNET_ATTENTION_OFF, NULL,
                               nameRestore, sizeof nameRestore);
    CHECK(rc == 1, "(7) restore: OFF/NULL selects a DAG_PRIMITIVE root");
    CHECK(strcmp(nameRestore, name0) == 0,
          "(7) restore: OFF/NULL reproduces the old default (artifact-off == name0)");

    /* (6) Footprint: snapshot the production reg AFTER the whole matrix; assert
       byte-identical (no mutation, no self-write during planning). */
    snapshot_registry(&lure.reg, &after);
    CHECK(reg_snaps_identical(&before, &after),
          "(6) production registry + BTN counters byte-identical after the matrix "
          "(no mutation, no self-write, attention_mode/rank_artifact restored)");

    printf("  [B0 default=%s order_only_noart=%s order_only_artifact=%s flipped=%d]\n",
           name0, nameP0, nameON, flipped);
    printf("  [B0 keying limitation: bias keyed (task_key=\"\", output_port=0) is "
           "per-producer-name GLOBAL, not per-task -- reported, B1 solves it]\n");
    printf("  [B0 readme gate NOT claimed reopened: probe only; default runtime stays "
           "attention_mode=OFF]\n");

    spc_fixture_free(&lure);
}

int run_test_structural_pref_b0(void) {
    failures = 0;
    test_b0_gate_probe();
    if (failures == 0) {
        printf("STRUCTURAL_PREF_B0 PASS\n");
        return 0;
    }
    printf("STRUCTURAL_PREF_B0 FAIL: %d checks failed.\n", failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) {
    return run_test_structural_pref_b0();
}
#endif
