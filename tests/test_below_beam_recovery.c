/*
 * test_below_beam_recovery.c -- the hermetic gate for CNET-D v5.1,
 * Below-Beam Recovery (SHADOW_ONLY, detect + report).
 *
 * Fixture (generalizes tests/test_dag.c's beam-limit blind-spot, but with a
 * VERIFIABLE correct producer): the mod-k residue domain delta (b=4,k=7) from
 * residue_common.h is trained + certified, then its reliability counters are
 * set LOW so it ranks BELOW two high-reliability "decoy" primitives whose
 * output ports are INCOMPATIBLE with the goal (so they cannot satisfy it). With
 * dag_beam_limit = 2 the official planner spends its whole beam on the two
 * decoys and misses delta; a beam-lifted re-plan finds delta and strict-executes
 * it to the ground-truth residue.
 *
 * A subtlety that matters: the goal residue and the start-residue source are the
 * SAME representation (ONEHOT 7), so an untagged goal would be satisfied
 * TRIVIALLY by the start source -- no producer needed, and the blind spot never
 * arises. We force a producer by giving the start residue source the tag
 * "r_state" and delta's OUTPUT (and the goal) the distinct tag "r_next": only
 * delta produces an "r_next", so the only valid plan runs delta. delta's input
 * residue slot is retagged "r_state" to consume the start source. (Retagging
 * representationally-identical ONEHOT 7 ports does not change delta's weights or
 * behavior; certification already happened against the trained table.)
 *
 * The probe DETECTS and REPORTS this; it does NOT act. The heart of the gate:
 *   - measurement: official missed it (official_matched_expected == 0) yet a
 *     valid below-beam plan existed (found_valid_below_beam == 1) with the
 *     recovered producer ranked at/below the beam cutoff;
 *   - no-authority: the registry entries AND every BTN's reliability counters
 *     are byte-identical after the probe (Delta-4), and the original registry's
 *     beam limit is unchanged (the const-copy beam-lift didn't mutate it).
 */
#include "residue_common.h"
#include "../include/proposal_sidecar.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* A minimal synthetic decoy BTN: in_port -> out_port, untrained. The output
   port is chosen INCOMPATIBLE with the goal so the decoy can never feed it. */
static int make_decoy(BinaryTransformNetwork *b, size_t in, size_t out,
                      Port input_port, Port output_port) {
    if (btn_init(b, in, out, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, input_port, output_port);
}

/* Snapshot of the authority-bearing registry state for the byte-identical
   no-authority compare (Delta-4 protects the reliability counters). */
typedef struct {
    PrimitiveState state;
    int certified;
    int queue_is_null;
    const char *shadow_of;
    unsigned long output_successes;
    unsigned long output_failures;
} EntrySnap;

#define MAX_SNAP 8

static size_t snapshot_registry(const PrimitiveRegistry *reg, EntrySnap *out) {
    size_t i;
    for (i = 0; i < reg->count && i < MAX_SNAP; ++i) {
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

static void test_below_beam_detected_no_authority(void) {
    const int b = 4, k = 7;
    double inputs[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double targets[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta = {0};
    BinaryTransformNetwork decoy_a = {0};
    BinaryTransformNetwork decoy_b = {0};
    PrimitiveRegistry reg;
    CertifyReport rep;
    CnetBelowBeamReport report;
    EntrySnap before[MAX_SNAP], after[MAX_SNAP];
    size_t n_before, n_after, samples;
    /* The task: goal = next residue ONEHOT 7 "r_next"; sources = [start residue
       onehot(0) "r_state", digit onehot(3) "digit"]; expected = (0*4 + 3) % 7 = 3.
       The goal tag "r_next" is produced ONLY by delta (see header note). */
    Port goal = {PORT_ONEHOT, (size_t)k, 1, ""};
    Port src_residue = {PORT_ONEHOT, (size_t)k, 1, ""};
    Port src_digit = {PORT_ONEHOT, (size_t)b, 1, ""};
    /* Decoy output ports, both INCOMPATIBLE with goal ONEHOT 7 "r_next":
       decoy_a has the wrong width (ONEHOT 3); decoy_b has the wrong tag
       (ONEHOT 7 "r_other" -> tag mismatch fails port_compatible). Both decoys
       consume "r_state" so they are valid candidates that spend the beam. */
    Port decoy_a_out = {PORT_ONEHOT, 3, 1, ""};
    Port decoy_b_out = {PORT_ONEHOT, (size_t)k, 1, ""};
    Port decoy_in = {PORT_ONEHOT, (size_t)k, 1, ""};
    double residue_vec[RES_MAX_K];
    double digit_vec[RES_MAX_B];
    DagSource sources[2];
    const int expected_argmax = (0 * 4 + 3) % 7;  /* == 3 */
    int rc;

    printf("below_beam_recovery: detect + report, no authority (residue fixture):\n");

    port_set_tag(&goal, "r_next");
    port_set_tag(&src_residue, "r_state");
    port_set_tag(&src_digit, "digit");
    port_set_tag(&decoy_in, "r_state");    /* decoys consume the start residue */
    port_set_tag(&decoy_b_out, "r_other"); /* tag mismatch vs goal "r_next" */

    /* ---- Build + certify the correct producer (delta) ---- */
    samples = build_residue_step_data(b, k, inputs, targets);
    train_residue_step(&delta, b, k, inputs, targets, samples, 12345u);
    if (certify_residue_step(&delta, k, inputs, targets, samples, 0.0, &rep) != 0) {
        CHECK(0, "fixture: delta certifies on its table");
        btn_free(&delta);
        return;
    }
    /* Retag delta's residue input slot + output to the distinct task tags so the
       start source cannot satisfy the goal directly and only delta produces it.
       Pure tag relabel of representationally-identical ONEHOT 7 ports. */
    port_set_tag(&delta.input_ports[0], "r_state");
    port_set_tag(&delta.output_ports[0], "r_next");

    /* ---- Build the two decoys (untrained; output port incompatible) ---- */
    if (make_decoy(&decoy_a, (size_t)k, 3, decoy_in, decoy_a_out) != 0 ||
        make_decoy(&decoy_b, (size_t)k, (size_t)k, decoy_in, decoy_b_out) != 0) {
        CHECK(0, "fixture: decoys build");
        btn_free(&delta);
        btn_free(&decoy_a);
        btn_free(&decoy_b);
        return;
    }

    /* ---- Prime reliability counters: decoys ~0.99 (rank 0,1), delta ~1/7
       (rank 2, below the beam=2 cutoff). Written directly: the ranking reads
       these on demand at plan time (recon-confirmed). ---- */
    decoy_a.output_successes = 100;
    decoy_a.output_failures = 0;
    decoy_b.output_successes = 100;
    decoy_b.output_failures = 0;
    delta.output_successes = 0;
    delta.output_failures = 5;

    registry_init(&reg);
    registry_add(&reg, &decoy_a, "decoy_a");      /* rank 0 */
    registry_add(&reg, &decoy_b, "decoy_b");      /* rank 1 */
    registry_add(&reg, &delta, "residue_step");   /* rank 2 (below beam=2) */
    registry_set_dag_beam_limit(&reg, 2);

    CHECK(reg.count == 3, "fixture has 3 producers (2 decoys + delta)");
    CHECK(reg.dag_beam_limit == 2, "beam limit set to 2");

    /* ---- The task sources ---- */
    res_onehot(residue_vec, 0, k);
    res_onehot(digit_vec, 3, b);
    sources[0].type = src_residue;
    sources[0].values = residue_vec;
    sources[1].type = src_digit;
    sources[1].values = digit_vec;

    /* 1. Snapshot the authority-bearing state (esp. the Delta-4 counters). */
    n_before = snapshot_registry(&reg, before);

    /* 2. Run the probe (read-only over reg). */
    memset(&report, 0, sizeof report);
    rc = proposal_sidecar_below_beam_probe(&reg, sources, 2, goal,
                                           expected_argmax, &report);
    CHECK(rc == 0, "proposal_sidecar_below_beam_probe returns 0");

    /* 3. Snapshot again and assert byte-identical (no footprint, Delta-4). */
    n_after = snapshot_registry(&reg, after);
    CHECK(n_before == n_after, "registry entry count unchanged");
    CHECK(snaps_identical(before, after, n_before),
          "registry state byte-identical after probe (counters/state/queue/shadow)");
    CHECK(reg.dag_beam_limit == 2,
          "original registry beam limit unchanged (const-copy beam-lift)");

    /* 4. No-authority invariants advertised by the report. */
    CHECK(report.advisory_only == 1, "report.advisory_only == 1");
    CHECK(report.authority == 0, "report.authority == 0");
    CHECK(report.planner_influence == 0, "report.planner_influence == 0");
    CHECK(report.registry_mutation_allowed == 0, "report.registry_mutation_allowed == 0");
    CHECK(report.proposer_name != NULL, "report names its proposer");
    CHECK(report.task_key != NULL, "report carries a task_key");
    CHECK(report.official_beam_limit == 2, "report.official_beam_limit == 2");

    /* 5. The measurement. */
    CHECK(report.official_matched_expected == 0,
          "official (beam=2) MISSED the valid plan (matched_expected == 0)");
    CHECK(report.sidecar_strict_ok == 1,
          "beam-lifted candidate strict-executed clean");
    CHECK(report.sidecar_matched_expected == 1,
          "beam-lifted candidate matched ground truth (residue == 3)");
    CHECK(report.recovered_producer_rank == 2,
          "recovered producer ranks 2 (the 2 decoys outrank delta)");
    CHECK(report.recovered_producer_rank >= (int)report.official_beam_limit,
          "recovered producer ranks at/below the beam cutoff");
    CHECK(report.found_valid_below_beam == 1,
          "found_valid_below_beam == 1 (the blind spot is exhibited)");

    /* 6. Delta-1 consistency: matched_expected implies strict_ok. */
    CHECK(!report.official_matched_expected || report.official_strict_ok,
          "Delta-1: official matched_expected implies strict_ok");
    CHECK(!report.sidecar_matched_expected || report.sidecar_strict_ok,
          "Delta-1: sidecar matched_expected implies strict_ok");

    printf("  [official(ok=%d,match=%d) sidecar(ok=%d,match=%d) "
           "rank=%d beam=%zu below_beam=%d proposer=%s key=%s]\n",
           report.official_strict_ok, report.official_matched_expected,
           report.sidecar_strict_ok, report.sidecar_matched_expected,
           report.recovered_producer_rank, report.official_beam_limit,
           report.found_valid_below_beam, report.proposer_name, report.task_key);

    btn_free(&delta);
    btn_free(&decoy_a);
    btn_free(&decoy_b);
    registry_free(&reg);
}

int run_test_below_beam_recovery(void) {
    test_below_beam_detected_no_authority();
    if (failures == 0) { printf("BELOW_BEAM_RECOVERY PASS\n"); return 0; }
    printf("BELOW_BEAM_RECOVERY FAIL: %d checks failed.\n", failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) {
    return run_test_below_beam_recovery();
}
#endif
