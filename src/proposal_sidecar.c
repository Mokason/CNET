/*
 * proposal_sidecar.c -- CNET-D v5.0 Proposal Sidecar (SHADOW_ONLY).
 *
 * A scripted, deterministic proposer + a strict-validation harness that reuses
 * the EXISTING executor (dag_execute with plan.strict = 1) and emits telemetry.
 * It is read-only over the registry and -- crucially -- leaves the borrowed
 * BTNs' runtime reliability counters bit-for-bit unchanged (Delta-4), so
 * strict-validating imagined candidates can never indirectly steer the planner.
 *
 * No new validation semantics: "valid" means exactly "CNET's strict executor
 * accepted it." The sidecar never registers, certifies, re-orders, prunes, or
 * re-plans. It has zero authority by construction.
 */
#include "../include/proposal_sidecar.h"
#include "../include/nn.h"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
#include <stdatomic.h>
#endif

/* Fixture facts for the v5.0 residue proposer (must match residue_common.h's
   delta: input slots [residue ONEHOT k, digit ONEHOT b] -> ONEHOT k). */
#define PSC_B 4
#define PSC_K 7
#define PSC_VEC_CAP 16   /* >= max(PSC_B, PSC_K); source buffers are this wide */

/* One-hot encode v in [0,width) into width doubles (vec must hold width). */
static void psc_onehot(double *vec, int v, int width) {
    int i;
    for (i = 0; i < width; ++i) vec[i] = (i == v) ? 1.0 : 0.0;
}

/* Argmax over the first k doubles. */
static int psc_argmax(const double *v, int k) {
    int j, best = 0;
    double bv = v[0];
    for (j = 1; j < k; ++j) if (v[j] > bv) { bv = v[j]; best = j; }
    return best;
}

/* ---- Delta-4: snapshot / restore the borrowed BTNs' reliability counters ---- */

typedef struct {
    unsigned long successes;
    unsigned long failures;
} CounterSnap;

static unsigned long psc_load_succ(const BinaryTransformNetwork *btn) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    return atomic_load_explicit(&((BinaryTransformNetwork *)btn)->output_successes,
                                memory_order_relaxed);
#else
    return btn->output_successes;
#endif
}

static unsigned long psc_load_fail(const BinaryTransformNetwork *btn) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    return atomic_load_explicit(&((BinaryTransformNetwork *)btn)->output_failures,
                                memory_order_relaxed);
#else
    return btn->output_failures;
#endif
}

static void psc_store_succ(BinaryTransformNetwork *btn, unsigned long v) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    atomic_store_explicit(&btn->output_successes, v, memory_order_relaxed);
#else
    btn->output_successes = v;
#endif
}

static void psc_store_fail(BinaryTransformNetwork *btn, unsigned long v) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
    atomic_store_explicit(&btn->output_failures, v, memory_order_relaxed);
#else
    btn->output_failures = v;
#endif
}

/* Capture every registry BTN's runtime counters. snaps must hold reg->count.
   Distinct entries may share a btn pointer; we snapshot per entry, which is
   safe because restore writes the same captured value back regardless. */
static void psc_snapshot_counters(const PrimitiveRegistry *reg, CounterSnap *snaps) {
    size_t i;
    for (i = 0; i < reg->count; ++i) {
        const BinaryTransformNetwork *btn = reg->entries[i].btn;
        snaps[i].successes = psc_load_succ(btn);
        snaps[i].failures = psc_load_fail(btn);
    }
}

static void psc_restore_counters(const PrimitiveRegistry *reg, const CounterSnap *snaps) {
    size_t i;
    for (i = 0; i < reg->count; ++i) {
        BinaryTransformNetwork *btn = reg->entries[i].btn;
        psc_store_succ(btn, snaps[i].successes);
        psc_store_fail(btn, snaps[i].failures);
    }
}

/* ---- The validator: reuse the existing strict executor verbatim ---- */

/* Strict-validate ONE hand-built single-delta candidate.
   srcs[0] feeds slot 0 (residue), srcs[1] feeds slot 1 (digit).
   On a clean run returns 0 and writes argmax(out) to *predicted; on a strict
   abort (out-of-domain / type-inconsistent handoff) returns -1. */
static int psc_validate_one_digit(const BinaryTransformNetwork *delta,
                                  const char *delta_name,
                                  const DagSource *srcs,
                                  int *predicted) {
    DagNode step;
    DagNode s_residue, s_digit;
    DagPlan plan;
    double out[PSC_VEC_CAP];
    int rc;

    memset(&plan, 0, sizeof plan);
    memset(&step, 0, sizeof step);
    memset(&s_residue, 0, sizeof s_residue);
    memset(&s_digit, 0, sizeof s_digit);

    s_residue.kind = DAG_SOURCE;
    s_residue.source_index = 0;
    s_digit.kind = DAG_SOURCE;
    s_digit.source_index = 1;

    step.kind = DAG_PRIMITIVE;
    step.btn = delta;
    step.name = delta_name;
    step.output_index = 0;
    step.children[0] = &s_residue;   /* slot 0: residue (ONEHOT k) */
    step.children[1] = &s_digit;     /* slot 1: digit   (ONEHOT b) */
    step.child_count = 2;

    plan.root = &step;
    plan.owned = NULL;               /* hand-built: caller owns the nodes */
    plan.strict = 1;                 /* abort on any out-of-domain handoff */

    rc = dag_execute(&plan, srcs, 2, out, (size_t)PSC_K);
    if (rc != 0) {
        return -1;
    }
    if (predicted != NULL) {
        *predicted = psc_argmax(out, PSC_K);
    }
    return 0;
}

int proposal_sidecar_run_demo(const PrimitiveRegistry *reg,
                              CnetPlanProposalReport *report) {
    const BinaryTransformNetwork *delta;
    const char *delta_name;
    CounterSnap *snaps = NULL;
    DagSource srcs[2];
    double residue_vec[PSC_VEC_CAP];
    double digit_vec[PSC_VEC_CAP];
    Port residue_port, digit_port;
    const int valid_digit = 3;       /* ground truth for a 1-digit scan = d % k */
    int predicted;
    int rc;

    if (reg == NULL || report == NULL || reg->count == 0) {
        return -1;
    }
    delta = reg->entries[0].btn;
    delta_name = reg->entries[0].name;
    if (delta == NULL) {
        return -1;
    }

    /* No-authority invariants are constants for v5.0 (asserted in the gate). */
    memset(report, 0, sizeof *report);
    report->kind = CNET_PROPOSAL_PLAN_DAG;
    report->advisory_only = 1;
    report->authority = 0;
    report->planner_influence = 0;
    report->registry_mutation_allowed = 0;
    report->strict_validator_required = 1;
    report->proposer_name = "scripted_residue_v50";
    report->task_key = "residue_step:onehot7+onehot4->onehot7";

    /* INVARIANT (gate-enforced by test_proposal_sidecar, "Delta-4"):
       a sidecar path that strict-executes candidates MUST leave every borrowed
       BTN's reliability counters byte-identical. Those counters feed the
       planner's ranking/beam, so any net mutation would be backdoor
       planner_influence -- the exact authority we forbid. The mechanism:
       snapshot the counters here, restore them verbatim below. ANY new
       strict-executing path added in later milestones (v5.1+) must repeat this
       snapshot/restore -- the v5.0 gate only proves it for the v5.0 path. */
    snaps = (CounterSnap *)malloc(reg->count * sizeof(*snaps));
    if (snaps == NULL) {
        return -1;
    }
    psc_snapshot_counters(reg, snaps);

    /* Typed source ports shared by the candidates (residue=ONEHOT k, digit=ONEHOT b). */
    residue_port = (Port){PORT_ONEHOT, (size_t)PSC_K, 1, ""};
    digit_port = (Port){PORT_ONEHOT, (size_t)PSC_B, 1, ""};
    port_set_tag(&residue_port, "residue");
    port_set_tag(&digit_port, "digit");

    /* ---- Candidate 1: KNOWN-VALID 1-digit scan ----
       start residue = 0 (ONEHOT k), digit = valid_digit (ONEHOT b). A correct,
       type-consistent plan: strict-executes clean AND matches ground truth
       (start residue 0 => result = valid_digit % k). */
    memset(residue_vec, 0, sizeof residue_vec);
    memset(digit_vec, 0, sizeof digit_vec);
    psc_onehot(residue_vec, 0, PSC_K);
    psc_onehot(digit_vec, valid_digit, PSC_B);

    srcs[0].type = residue_port;
    srcs[0].values = residue_vec;
    srcs[1].type = digit_port;
    srcs[1].values = digit_vec;

    report->proposed_count++;
    predicted = -1;
    rc = psc_validate_one_digit(delta, delta_name, srcs, &predicted);
    if (rc == 0) {
        report->strict_executed_ok_count++;
        if (predicted == (valid_digit % PSC_K)) {
            report->matched_expected_count++;
        }
    } else {
        report->strict_rejected_count++;
    }

    /* ---- Candidate 2: KNOWN-INVALID (type-broken / out-of-domain) ----
       The digit source declares its proper type (ONEHOT b) but carries an
       out-of-domain value: an all-zeros vector (no hot bit). The executor
       validates external input against its source port on entry and strict-
       aborts (rc=-1). This is the anti-creep centerpiece: a deliberately
       broken candidate is REJECTED and counted -- telemetry only, no registry
       change, no stats promotion, no planner fallback. */
    memset(residue_vec, 0, sizeof residue_vec);
    memset(digit_vec, 0, sizeof digit_vec);
    psc_onehot(residue_vec, 0, PSC_K);
    /* digit_vec deliberately left all-zeros: fails ONEHOT validation. */

    srcs[0].type = residue_port;
    srcs[0].values = residue_vec;
    srcs[1].type = digit_port;
    srcs[1].values = digit_vec;

    report->proposed_count++;
    rc = psc_validate_one_digit(delta, delta_name, srcs, NULL);
    if (rc == 0) {
        report->strict_executed_ok_count++;
    } else {
        report->strict_rejected_count++;
    }

    /* Delta-4: restore the reliability counters verbatim -- the strict runs
       above are now invisible to reality's evidence. */
    psc_restore_counters(reg, snaps);
    free(snaps);

    return 0;
}

/* ============================================================================
 * CNET-D v5.1 -- Below-Beam Recovery (SHADOW_ONLY, detect + report).
 * ==========================================================================*/

/* Total width of a port (field_width * field_count); used to bound argmax. */
static size_t psc_port_total(Port p) {
    return p.field_width * p.field_count;
}

/* Strict-validate a planner-built DagPlan: set plan->strict, execute, and
   (on a clean run) compare argmax(out) over the goal width to expected_argmax.
   *strict_ok = (rc == 0); *matched = strict_ok && (argmax == expected). out is
   a scratch buffer of >= goal_total doubles. */
static void psc_strict_validate_plan(DagPlan *plan,
                                     const DagSource *sources, size_t n_sources,
                                     size_t goal_total, int expected_argmax,
                                     double *out, size_t out_cap,
                                     int *strict_ok, int *matched) {
    int rc;
    int predicted;

    *strict_ok = 0;
    *matched = 0;
    if (goal_total == 0 || out_cap < goal_total) {
        return;
    }
    plan->strict = 1;
    rc = dag_execute(plan, sources, n_sources, out, out_cap);
    if (rc != 0) {
        return;
    }
    *strict_ok = 1;
    predicted = psc_argmax(out, (int)goal_total);
    if (predicted == expected_argmax) {
        *matched = 1;
    }
}

/* 0-based reliability rank of `target` among ALL registry entries: the count of
   entries whose btn_reliability is STRICTLY greater than target's. Matched to a
   registry entry by btn pointer first, then by name. Returns -1 if the target
   is not found in the registry. */
static int psc_rank_of_root(const PrimitiveRegistry *reg,
                            const BinaryTransformNetwork *target_btn,
                            const char *target_name) {
    size_t i;
    int found = 0;
    double target_rel = 0.0;
    int rank = 0;

    for (i = 0; i < reg->count; ++i) {
        const RegistryEntry *e = &reg->entries[i];
        int is_match = 0;
        if (target_btn != NULL && e->btn == target_btn) {
            is_match = 1;
        } else if (!found && target_name != NULL && e->name != NULL &&
                   strcmp(e->name, target_name) == 0) {
            is_match = 1;
        }
        if (is_match) {
            target_rel = btn_reliability(e->btn);
            found = 1;
            break;
        }
    }
    if (!found) {
        return -1;
    }
    for (i = 0; i < reg->count; ++i) {
        if (btn_reliability(reg->entries[i].btn) > target_rel) {
            ++rank;
        }
    }
    return rank;
}

int proposal_sidecar_below_beam_probe(const PrimitiveRegistry *reg,
                                      const DagSource *sources, size_t n_sources,
                                      Port goal, int expected_argmax,
                                      CnetBelowBeamReport *report) {
    CounterSnap *snaps = NULL;
    PrimitiveRegistry lifted;
    DagPlan official;
    DagPlan sidecar;
    double *out = NULL;
    size_t goal_total;
    int rc;

    if (reg == NULL || report == NULL || sources == NULL || reg->count == 0) {
        return -1;
    }

    /* Constant no-authority invariants + task framing. */
    memset(report, 0, sizeof *report);
    report->advisory_only = 1;
    report->authority = 0;
    report->planner_influence = 0;
    report->registry_mutation_allowed = 0;
    report->recovered_producer_rank = -1;
    report->official_beam_limit = reg->dag_beam_limit;
    report->proposer_name = "beam_lifted_replan_v51";
    report->task_key = "below_beam_recovery:official_vs_beam_lifted";

    goal_total = psc_port_total(goal);
    if (goal_total == 0) {
        return -1;
    }
    out = (double *)malloc(goal_total * sizeof(*out));
    if (out == NULL) {
        return -1;
    }

    /* INVARIANT (Delta-4, gate-enforced): a sidecar path that strict-executes
       candidates MUST leave every borrowed BTN's reliability counters
       byte-identical. dag_execute records evidence into the borrowed BTNs, and
       that evidence feeds the NEXT official dag_plan's ranking/beam -- any net
       mutation would be backdoor planner_influence, the exact authority we
       forbid. Snapshot here, restore before every return below. */
    snaps = (CounterSnap *)malloc(reg->count * sizeof(*snaps));
    if (snaps == NULL) {
        free(out);
        return -1;
    }
    psc_snapshot_counters(reg, snaps);

    /* ---- Official plan: the beam-limited planner exactly as production runs.
       If it builds, strict-validate it; if not, both official_* stay 0. ---- */
    memset(&official, 0, sizeof official);
    rc = dag_plan(reg, sources, n_sources, goal, &official);
    if (rc == 0) {
        psc_strict_validate_plan(&official, sources, n_sources, goal_total,
                                 expected_argmax, out, goal_total,
                                 &report->official_strict_ok,
                                 &report->official_matched_expected);
        dag_free(&official);
    }

    /* ---- Beam-lifted re-plan (const-safe): a SHALLOW COPY of the registry
       sharing the borrowed entries/BTNs, with the beam disabled and memo
       pruning off. The passed registry is never mutated -> const respected. -- */
    lifted = *reg;
    lifted.dag_beam_limit = 0;     /* 0 = no beam limit (full candidate set) */
    lifted.disable_plan_memo = 1;  /* expose the full set, no reachability prune */

    memset(&sidecar, 0, sizeof sidecar);
    rc = dag_plan(&lifted, sources, n_sources, goal, &sidecar);
    if (rc == 0) {
        psc_strict_validate_plan(&sidecar, sources, n_sources, goal_total,
                                 expected_argmax, out, goal_total,
                                 &report->sidecar_strict_ok,
                                 &report->sidecar_matched_expected);
        /* Reconstruct the recovered root producer's reliability rank among ALL
           reg entries (the recovered root is a DAG_PRIMITIVE with ->btn/->name). */
        if (sidecar.root != NULL && sidecar.root->kind == DAG_PRIMITIVE) {
            report->recovered_producer_rank =
                psc_rank_of_root(reg, sidecar.root->btn, sidecar.root->name);
        }
        dag_free(&sidecar);
    }

    report->found_valid_below_beam =
        report->sidecar_matched_expected &&
        report->recovered_producer_rank >= 0 &&
        report->recovered_producer_rank >= (int)report->official_beam_limit;

    /* Delta-4: restore the counters verbatim -- both strict runs above are now
       invisible to reality's evidence. */
    psc_restore_counters(reg, snaps);
    free(snaps);
    free(out);

    return 0;
}
