/*
 * test_proposal_sidecar.c -- the hermetic no-authority gate for CNET-D v5.0,
 * the Proposal Sidecar (SHADOW_ONLY).
 *
 * Fixture: the mod-k residue domain (delta(r,d) = (r*b+d) mod k), reused from
 * residue_common.h. delta is trained + certified exactly on its k*b table, then
 * registered as a certified primitive. The sidecar proposes a small fixed set
 * of candidate plans (>=1 known-valid, >=1 known-invalid), strict-validates each
 * through the EXISTING executor, and reports telemetry only.
 *
 * The heart of the test: snapshot the registry's authority-bearing state (entry
 * lifecycle/certified/queue/shadow + every BTN's reliability counters) BEFORE
 * the sidecar runs and assert it is byte-identical AFTER -- imagination must
 * leave no footprint on reality's evidence (Delta-4).
 */
#include "residue_common.h"
#include "../include/proposal_sidecar.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* Train + certify delta and register it certified. Returns 0 on success. */
static int build_fixture(PrimitiveRegistry *reg, BinaryTransformNetwork *delta,
                         double *inputs, double *targets, int b, int k) {
    CertifyReport rep;
    Contract c;
    double *canon;
    size_t samples, i;

    memset(delta, 0, sizeof *delta);
    registry_init(reg);

    samples = build_residue_step_data(b, k, inputs, targets);
    train_residue_step(delta, b, k, inputs, targets, samples, 12345u);
    if (certify_residue_step(delta, k, inputs, targets, samples, 0.0, &rep) != 0) {
        return -1;
    }

    /* Build the contract delta certifies against (canonicalize targets first),
       then register certified -- mirrors test_residue + the certify gate. */
    canon = (double *)malloc(samples * (size_t)k * sizeof(double));
    if (canon == NULL) {
        return -1;
    }
    for (i = 0; i < samples; ++i) {
        if (port_canonicalize(delta->output_ports[0],
                              targets + i * (size_t)k,
                              canon + i * (size_t)k) != 0) {
            free(canon);
            return -1;
        }
    }
    if (contract_init_borrowed(&c, "residue_step", delta, inputs, canon,
                               samples) != 0) {
        free(canon);
        return -1;
    }
    if (registry_add_certified(reg, delta, "residue_step", &c) != 0) {
        free(canon);
        return -1;
    }
    free(canon);  /* the contract borrowed the tables; the entry keeps the btn */
    return 0;
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

static void test_sidecar_has_no_authority(void) {
    const int b = 4, k = 7;
    double inputs[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double targets[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta;
    PrimitiveRegistry reg;
    CnetPlanProposalReport report;
    EntrySnap before[MAX_SNAP], after[MAX_SNAP];
    size_t n_before, n_after;
    int rc;

    printf("proposal_sidecar: no-authority gate (residue fixture):\n");

    if (build_fixture(&reg, &delta, inputs, targets, b, k) != 0) {
        CHECK(0, "fixture: delta certifies + registers certified");
        return;
    }
    CHECK(reg.count == 1, "fixture has the single certified residue_step");
    CHECK(reg.entries[0].certified == 1, "residue_step registered certified");
    CHECK(reg.entries[0].state == PRIM_FROZEN, "residue_step is FROZEN on certify");

    /* 1. Snapshot the authority-bearing state (esp. the Delta-4 counters). */
    n_before = snapshot_registry(&reg, before);

    /* 2. Run the sidecar (proposer + strict validation + telemetry). */
    memset(&report, 0, sizeof report);
    rc = proposal_sidecar_run_demo(&reg, &report);
    CHECK(rc == 0, "proposal_sidecar_run_demo returns 0");

    /* 3. Snapshot again and assert byte-identical (no footprint on evidence). */
    n_after = snapshot_registry(&reg, after);
    CHECK(n_before == n_after, "registry entry count unchanged");
    CHECK(snaps_identical(before, after, n_before),
          "registry state byte-identical after sidecar (counters/state/queue/shadow)");

    /* The single most load-bearing fact: the reliability counters reality reads
       are bit-for-bit unchanged despite strict-executing candidates (Delta-4). */
    CHECK(before[0].output_successes == after[0].output_successes &&
          before[0].output_failures == after[0].output_failures,
          "Delta-4: BTN reliability counters unchanged by strict validation");

    /* 4. No-authority invariants must be advertised by the report. */
    CHECK(report.advisory_only == 1, "report.advisory_only == 1");
    CHECK(report.authority == 0, "report.authority == 0");
    CHECK(report.planner_influence == 0, "report.planner_influence == 0");
    CHECK(report.registry_mutation_allowed == 0, "report.registry_mutation_allowed == 0");
    CHECK(report.strict_validator_required == 1, "report.strict_validator_required == 1");
    CHECK(report.kind == CNET_PROPOSAL_PLAN_DAG, "report.kind == DAG (v5.0 fixture)");
    CHECK(report.proposer_name != NULL, "report names its proposer");
    CHECK(report.task_key != NULL, "report carries a task_key");

    /* 5. Outcomes: both gate paths exercised, the anti-creep reject present. */
    CHECK(report.proposed_count >= 2, "proposed >= 2 candidates");
    CHECK(report.strict_executed_ok_count >= 1,
          "at least one candidate strict-executed clean");
    CHECK(report.strict_rejected_count >= 1,
          "anti-creep: at least one candidate strict-REJECTED (telemetry only)");
    CHECK(report.matched_expected_count >= 1,
          "at least one candidate matched ground truth");
    /* Accounting sanity: ok + rejected accounts for every proposal, and a match
       is always a clean execution. */
    CHECK(report.strict_executed_ok_count + report.strict_rejected_count
              == report.proposed_count,
          "every proposal is either strict-ok or strict-rejected");
    CHECK(report.matched_expected_count <= report.strict_executed_ok_count,
          "matches are a subset of clean executions");

    printf("  [proposed=%d ok=%d rejected=%d matched=%d proposer=%s key=%s]\n",
           report.proposed_count, report.strict_executed_ok_count,
           report.strict_rejected_count, report.matched_expected_count,
           report.proposer_name, report.task_key);

    btn_free(&delta);
    registry_free(&reg);
}

int run_test_proposal_sidecar(void) {
    test_sidecar_has_no_authority();
    if (failures == 0) { printf("PROPOSAL_SIDECAR PASS\n"); return 0; }
    printf("PROPOSAL_SIDECAR FAIL: %d checks failed.\n", failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) {
    return run_test_proposal_sidecar();
}
#endif
