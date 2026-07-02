/*
 * test_belowbeam_chars.c -- the thin hermetic ANCHOR for the below-beam
 * failure-mode characterization study (spec: docs/superpowers/specs/
 * 2026-06-19-belowbeam-failure-mode-characterization-design.md).
 *
 * NOT a measurement -- a regression guard that the study's two cause generators
 * (cold-start, rank-poisoning) build VALID below-beam blind spots and that the
 * v5.1 probe over them is no-authority + leaves no Delta-4 footprint.
 *
 * One deterministic fixture per cause, each at a representative knob value that
 * DOES exhibit the blind spot:
 *   - cold-start:    delta n=1 evidence, M=8 spread competitors, beam=2;
 *   - rank-poisoning: delta s=8 evidence, m=3 injected-high competitors, beam=2.
 *
 * For EACH: snapshot the authority-bearing state (per entry: state / certified /
 * queue-null / shadow_of, and each BTN's output_successes/output_failures), run
 * the probe, and assert: found_valid_below_beam == 1; recovered_producer_rank >=
 * official_beam_limit; the registry + ALL BTN counters are byte-identical after
 * (Delta-4); and the no-authority report flags (authority/planner_influence/
 * registry_mutation_allowed all 0). The fixture device is reused verbatim from
 * test_below_beam_recovery.c via belowbeam_chars_common.h.
 */
#include "belowbeam_chars_common.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* Snapshot of one registry entry's authority-bearing state (Delta-4 protects
   the reliability counters). Mirrors test_below_beam_recovery.c. */
typedef struct {
    PrimitiveState state;
    int certified;
    int queue_is_null;
    const char *shadow_of;
    unsigned long output_successes;
    unsigned long output_failures;
} EntrySnap;

static size_t snapshot_registry(const PrimitiveRegistry *reg, EntrySnap *out) {
    size_t i;
    for (i = 0; i < reg->count && i < BBC_MAX_ENTRIES; ++i) {
        const RegistryEntry *e = &reg->entries[i];
        out[i].state = e->state;
        out[i].certified = e->certified;
        out[i].queue_is_null = (e->queue == NULL);
        out[i].shadow_of = e->shadow_of;
        out[i].output_successes = (unsigned long)e->btn->output_successes;
        out[i].output_failures = (unsigned long)e->btn->output_failures;
    }
    return i;
}

static int snaps_identical(const EntrySnap *a, const EntrySnap *b, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        if (a[i].state != b[i].state) return 0;
        if (a[i].certified != b[i].certified) return 0;
        if (a[i].queue_is_null != b[i].queue_is_null) return 0;
        if (a[i].shadow_of != b[i].shadow_of) return 0;
        if (a[i].output_successes != b[i].output_successes) return 0;
        if (a[i].output_failures != b[i].output_failures) return 0;
    }
    return 1;
}

/* Run the probe over one fixture and assert the per-cause invariants. */
static void check_cause(BbcFixture *f, const char *label) {
    CnetBelowBeamReport report;
    EntrySnap before[BBC_MAX_ENTRIES], after[BBC_MAX_ENTRIES];
    size_t n_before, n_after;
    int rc;
    int delta_rank;
    double margin;

    printf("belowbeam_chars: %s (representative blind-spot fixture):\n", label);

    CHECK(f->built, "fixture built (delta certified + registry populated)");
    if (!f->built) return;

    /* 1. Snapshot the authority-bearing state (esp. the Delta-4 counters). */
    n_before = snapshot_registry(&f->reg, before);

    /* 2. Run the probe (read-only over reg). */
    memset(&report, 0, sizeof report);
    rc = proposal_sidecar_below_beam_probe(&f->reg, f->sources, 2, f->goal,
                                           BBC_EXPECTED, &report);
    CHECK(rc == 0, "proposal_sidecar_below_beam_probe returns 0");

    /* 3. Snapshot again, assert byte-identical (no footprint, Delta-4). */
    n_after = snapshot_registry(&f->reg, after);
    CHECK(n_before == n_after, "registry entry count unchanged");
    CHECK(snaps_identical(before, after, n_before),
          "registry + ALL BTN counters byte-identical after probe (Delta-4)");
    CHECK(f->reg.dag_beam_limit == 2,
          "original registry beam limit unchanged (const-copy beam-lift)");

    /* 4. The blind spot is exhibited. */
    CHECK(report.found_valid_below_beam == 1,
          "found_valid_below_beam == 1 (the cause exhibits a blind spot)");
    CHECK(report.recovered_producer_rank >= (int)report.official_beam_limit,
          "recovered producer ranks at/below the beam cutoff");

    /* 5. No-authority invariants advertised by the report. */
    CHECK(report.authority == 0, "report.authority == 0");
    CHECK(report.planner_influence == 0, "report.planner_influence == 0");
    CHECK(report.registry_mutation_allowed == 0,
          "report.registry_mutation_allowed == 0");

    /* 6. Margin/rank reconstruction agrees with the probe (the study's math). */
    delta_rank = bbc_rank_of(&f->reg, "residue_step");
    margin = bbc_margin(&f->reg, "residue_step", report.official_beam_limit);
    CHECK(delta_rank == report.recovered_producer_rank,
          "reconstructed delta rank matches probe recovered_producer_rank");

    printf("  [rank=%d beam=%zu below_beam=%d margin=%.4f]\n",
           report.recovered_producer_rank, report.official_beam_limit,
           report.found_valid_below_beam, margin);
}

static void test_coldstart_blind_spot(void) {
    BbcFixture f;
    /* cold-start: delta n=1, M=8 spread competitors, beam=2 -> delta low rel,
       competitors all above it; delta sits below the beam=2 cutoff. */
    if (bbc_make_coldstart(&f, 8, 1u, 2) != 0) {
        CHECK(0, "cold-start fixture builds (delta certifies)");
        bbc_fixture_free(&f);
        return;
    }
    check_cause(&f, "cold-start");
    bbc_fixture_free(&f);
}

static void test_poisoning_blind_spot(void) {
    BbcFixture f;
    /* rank-poisoning: delta s=8 (rel ~0.9), m=3 injected-high competitors
       (rel ~0.99), beam=2 -> the 3 poisoners outrank delta, pushing it past
       the beam=2 cutoff to rank 3. */
    if (bbc_make_poisoning(&f, 3, 8u, 2) != 0) {
        CHECK(0, "rank-poisoning fixture builds (delta certifies)");
        bbc_fixture_free(&f);
        return;
    }
    check_cause(&f, "rank-poisoning");
    bbc_fixture_free(&f);
}

int run_test_belowbeam_chars(void) {
    test_coldstart_blind_spot();
    test_poisoning_blind_spot();
    if (failures == 0) { printf("BELOWBEAM_CHARS PASS\n"); return 0; }
    printf("BELOWBEAM_CHARS FAIL: %d checks failed.\n", failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) {
    return run_test_belowbeam_chars();
}
#endif
