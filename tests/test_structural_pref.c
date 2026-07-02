/*
 * test_structural_pref.c -- the hermetic ANCHOR for the Structural Preference
 * Sidecar -- Derivation Lock (SHADOW_ONLY).
 * (spec: docs/superpowers/specs/2026-06-19-structural-preference-sidecar-design.md)
 *
 * Among ALREADY-VALID single-root structures, the derivation-lock residual
 * measures how attractor-like each is: re-derive the same dag_plan task under
 * candidate-order + search-depth perturbations and check whether the SAME
 * canonical plan digest reappears. Low residual = an attractor; high residual =
 * ordering-dependent / fragile. Advisory only -- zero authority.
 *
 * The two fixtures (both fresh synthetic single-root producers, NOT the
 * planner_scale_study collision, whose same-named producers collapse to D=1):
 *   - ATTRACTOR: one producer solo: X -> G_a. Every perturbation reproduces the
 *     same single structure -> D == 1, residual == 0.
 *   - LURE: two DISTINCT-NAMED producers alt_a, alt_b: X -> G_l (same signature,
 *     equal fresh reliability). Permutations flip which equal producer wins ->
 *     D >= 2, residual > 0.
 *
 * HARD INVARIANTS (the no-footprint heart of the gate): snapshot the production
 * registry (each entry's name ptr, btn ptr, state, certified, queue-null,
 * shadow_of) AND each BTN's output_successes/output_failures AND
 * reg.count/dag_beam_limit/disable_plan_memo BEFORE the sweep; run the full
 * sweep on both fixtures; assert byte-identical AFTER. The perturbations only
 * ever touch COPIES, so the production registry/planner/counters stay
 * byte-identical (advisory, planner_influence = 0, registry_mutation = 0).
 */
#include "structural_pref_common.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* Snapshot of the authority-bearing registry state for the byte-identical
   no-footprint compare. Captures the borrowed pointers too (a shallow permuted
   copy must never rewrite the originals). */
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

#define SPC_MAX_SNAP 8

typedef struct {
    size_t count;
    size_t dag_beam_limit;
    int disable_plan_memo;
    EntrySnap entries[SPC_MAX_SNAP];
} RegSnap;

static void snapshot_registry(const PrimitiveRegistry *reg, RegSnap *out) {
    size_t i;
    out->count = reg->count;
    out->dag_beam_limit = reg->dag_beam_limit;
    out->disable_plan_memo = reg->disable_plan_memo;
    for (i = 0; i < reg->count && i < SPC_MAX_SNAP; ++i) {
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
    for (i = 0; i < a->count && i < SPC_MAX_SNAP; ++i) {
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

/* Run the sweep over one built fixture, asserting the production registry is
   byte-identical before/after, and return the filled SpcSweep via *sweep. */
static int sweep_fixture_no_footprint(SpcFixture *f, const char *label,
                                      SpcSweep *sweep) {
    RegSnap before, after;
    int rc;

    snapshot_registry(&f->reg, &before);
    rc = spc_run_sweep(&f->reg, f->sources, 1, f->goal, sweep);
    snapshot_registry(&f->reg, &after);

    CHECK(rc == 0, "spc_run_sweep returns 0");
    CHECK(reg_snaps_identical(&before, &after),
          "production registry byte-identical after sweep (entries/counters/scalars)");
    (void)label;
    return rc;
}

static void test_attractor(void) {
    SpcFixture f;
    SpcSweep s;
    uint64_t recomputed;

    printf("structural_pref: ATTRACTOR (one producer solo: X -> G_a):\n");
    if (spc_make_attractor(&f) != 0) {
        CHECK(0, "fixture: attractor builds");
        spc_fixture_free(&f);
        return;
    }
    CHECK(f.reg.count == 1, "attractor registry has 1 producer");

    if (sweep_fixture_no_footprint(&f, "attractor", &s) != 0) {
        spc_fixture_free(&f);
        return;
    }

    CHECK(s.baseline_planned == 1, "attractor baseline planned");
    CHECK(s.baseline_digest != 0u, "attractor baseline digest non-zero");
    CHECK(s.K > 0, "attractor: at least one cell planned (K>0)");
    CHECK(s.D == 1, "attractor: exactly ONE distinct structure (D==1)");
    CHECK(s.derivation_lock_residual == 0.0,
          "attractor: derivation_lock_residual == 0 (settled attractor)");
    CHECK(s.R == s.K, "attractor: every planned cell matched baseline (R==K)");

    /* The baseline digest must be reproducible (recompute -> same value). */
    recomputed = 0u;
    {
        int planned = spc_plan_under_perturbation(
            &f.reg, f.sources, 1, f.goal,
            SPC_BASELINE_PERM, SPC_BASELINE_BEAM, SPC_BASELINE_MEMO,
            &recomputed);
        CHECK(planned == 1, "attractor: baseline re-plan succeeds");
        CHECK(recomputed == s.baseline_digest,
              "attractor: baseline digest reproducible (same on recompute)");
    }

    /* ---- Step A: post-hoc candidate ranking (advisory; planner untouched) ---- */
    {
        SpcRanked r;
        RegSnap rb, ra;
        uint64_t rec = 0u;
        int prc;

        snapshot_registry(&f.reg, &rb);
        prc = structural_pref_rank(&f.reg, f.sources, 1, f.goal, &r);
        snapshot_registry(&f.reg, &ra);
        CHECK(prc == 0, "attractor: structural_pref_rank returns 0");
        CHECK(reg_snaps_identical(&rb, &ra),
              "attractor: registry byte-identical after rank (read-only, planner_influence=0)");
        CHECK(r.count == 1, "attractor: exactly ONE ranked structure");
        CHECK(r.entries[0].count == s.K, "attractor: rank-0 reproduced in every cell (count==K)");
        CHECK(r.baseline_rank == 0, "attractor: planner default is rank-0");
        prc = structural_pref_recommend_digest(&f.reg, f.sources, 1, f.goal, &r, &rec);
        CHECK(prc == 1 && rec == r.entries[0].digest,
              "attractor: rank-0 recipe re-derives to rank-0 digest (actionable)");
        CHECK(rec == s.baseline_digest, "attractor: rank-0 == baseline (the unique structure)");
    }

    printf("  [attractor K=%zu R=%zu D=%zu X=%zu residual=%.4f score=%.4f]\n",
           s.K, s.R, s.D, s.X, s.derivation_lock_residual, spc_score(&s));
    spc_fixture_free(&f);
}

static void test_lure(void) {
    SpcFixture f;
    SpcSweep s;

    printf("structural_pref: LURE (two distinct-named alt_a, alt_b: X -> G_l):\n");
    if (spc_make_lure(&f) != 0) {
        CHECK(0, "fixture: lure builds");
        spc_fixture_free(&f);
        return;
    }
    CHECK(f.reg.count == 2, "lure registry has 2 distinct-named producers");

    if (sweep_fixture_no_footprint(&f, "lure", &s) != 0) {
        spc_fixture_free(&f);
        return;
    }

    CHECK(s.baseline_planned == 1, "lure baseline planned");
    CHECK(s.baseline_digest != 0u, "lure baseline digest non-zero");
    CHECK(s.K > 0, "lure: at least one cell planned (K>0)");
    CHECK(s.D >= 2, "lure: at least TWO distinct structures (D>=2)");
    CHECK(s.derivation_lock_residual > 0.0,
          "lure: derivation_lock_residual > 0 (ordering-dependent / fragile)");
    CHECK(s.R < s.K, "lure: not every cell matched baseline (R<K)");

    /* ---- Step A: post-hoc candidate ranking (advisory; planner untouched) ---- */
    {
        SpcRanked r;
        RegSnap rb, ra;
        uint64_t rec = 0u;
        size_t i, sum = 0;
        int prc, ordered = 1;

        snapshot_registry(&f.reg, &rb);
        prc = structural_pref_rank(&f.reg, f.sources, 1, f.goal, &r);
        snapshot_registry(&f.reg, &ra);
        CHECK(prc == 0, "lure: structural_pref_rank returns 0");
        CHECK(reg_snaps_identical(&rb, &ra),
              "lure: registry byte-identical after rank (read-only, planner_influence=0)");
        CHECK(r.count >= 2, "lure: >=2 ranked structures");
        for (i = 0; i + 1 < r.count; ++i) {
            if (r.entries[i].count < r.entries[i + 1].count) ordered = 0;
        }
        CHECK(ordered, "lure: ranked by reproduction-count descending");
        for (i = 0; i < r.count; ++i) sum += r.entries[i].count;
        CHECK(sum == s.K, "lure: NO prune (sum of ranked counts == K; all D structures present)");
        CHECK(r.baseline_rank >= 0, "lure: planner default still reachable in the ranked set");
        CHECK(r.baseline_rank >= 0 &&
              r.entries[0].count >= r.entries[r.baseline_rank].count,
              "lure: rank-0 recommendation is >= the planner default in reproduction");
        prc = structural_pref_recommend_digest(&f.reg, f.sources, 1, f.goal, &r, &rec);
        CHECK(prc == 1 && rec == r.entries[0].digest,
              "lure: rank-0 recipe re-derives to rank-0 digest (actionable)");
    }

    printf("  [lure K=%zu R=%zu D=%zu X=%zu residual=%.4f score=%.4f]\n",
           s.K, s.R, s.D, s.X, s.derivation_lock_residual, spc_score(&s));
    spc_fixture_free(&f);
}

/* The two distinct producers in the lure really do hash differently (the digest
   discriminates by name) -- the heart of why the lure has D>=2. */
static void test_digest_discriminates_by_name(void) {
    SpcFixture la, lb;
    uint64_t da = 0u, db = 0u;
    int pa, pb;
    Port goal_b;

    printf("structural_pref: digest discriminates by name (not pointer):\n");

    /* Plan over a single-producer "alt_a" registry vs a single-producer
       "alt_b" registry, same goal/sources -> the digests must differ because
       the only structural difference is the producer NAME. */
    memset(&la, 0, sizeof la);
    memset(&lb, 0, sizeof lb);

    if (spc_build_producer(&la.producers[0], "gl") != 0) {
        CHECK(0, "fixture: name-test producer a builds");
        return;
    }
    la.producer_count = 1;
    spc_set_task(&la, "gl");
    registry_init(&la.reg);
    registry_add(&la.reg, &la.producers[0], "alt_a");
    la.built = 1;

    if (spc_build_producer(&lb.producers[0], "gl") != 0) {
        CHECK(0, "fixture: name-test producer b builds");
        spc_fixture_free(&la);
        return;
    }
    lb.producer_count = 1;
    spc_set_task(&lb, "gl");
    goal_b = lb.goal;
    registry_init(&lb.reg);
    registry_add(&lb.reg, &lb.producers[0], "alt_b");
    lb.built = 1;

    pa = spc_plan_under_perturbation(&la.reg, la.sources, 1, la.goal,
                                     0, 0, 1, &da);
    pb = spc_plan_under_perturbation(&lb.reg, lb.sources, 1, goal_b,
                                     0, 0, 1, &db);
    CHECK(pa == 1 && pb == 1, "both single-name plans succeed");
    CHECK(da != db, "alt_a vs alt_b plans hash to DIFFERENT digests (by name)");

    spc_fixture_free(&la);
    spc_fixture_free(&lb);
}

int run_test_structural_pref(void) {
    failures = 0;
    test_attractor();
    test_lure();
    test_digest_discriminates_by_name();
    if (failures == 0) {
        printf("STRUCTURAL_PREF PASS\n");
        return 0;
    }
    printf("STRUCTURAL_PREF FAIL: %d checks failed.\n", failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) {
    return run_test_structural_pref();
}
#endif
