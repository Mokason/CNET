/*
 * probe_overhead_bench.c -- CNET-D v5.1 probe overhead vs the planner's
 * route+execute cycle (deployment-shaped denominator, "option 2"). NOT in make test.
 *
 * The v5.1 below-beam probe is SHADOW-ONLY and OPT-IN: nothing calls it in v5.1's
 * design. This number is for a FUTURE deployer deciding whether to ACTIVATE the
 * probe, not for v5.1 correctness. It is reported as a multiple of one full
 * route+execute cycle (dag_plan + dag_execute) -- the layer the probe would sit
 * beside in production -- measured on a NORMAL task (no blind spot: beam=8 finds
 * delta), since that is the deployment-shaped case the probe is paid on per call.
 *
 * Per call the probe does: official dag_plan + strict validate, beam-lifted
 * dag_plan + strict validate, reliability-rank reconstruction, and the Delta-4
 * counter snapshot/restore. The baseline does one dag_plan + one dag_execute.
 * The ratio is the probe's overhead.
 */
#include "residue_common.h"
#include "../include/proposal_sidecar.h"

#include <stdio.h>
#include <time.h>

/* A minimal synthetic decoy: in -> out, untrained, output port chosen so it is
   INCOMPATIBLE with the goal (a realistic "irrelevant high-reliability producer"
   that the planner must rank and skip). */
static int make_decoy(BinaryTransformNetwork *b, size_t in, size_t out,
                      Port input_port, Port output_port) {
    if (btn_init(b, in, out, 1, 4, 0.5, 1u) != 0) {
        return -1;
    }
    return btn_set_ports(b, input_port, output_port);
}

int main(void) {
    const int b = 4, k = 7;
    const size_t ITERS = 200000;
    double inputs[RES_MAX_K * RES_MAX_B * (RES_MAX_K + RES_MAX_B)];
    double targets[RES_MAX_K * RES_MAX_B * RES_MAX_K];
    BinaryTransformNetwork delta = {0}, decoy_a = {0}, decoy_b = {0};
    PrimitiveRegistry reg;
    CertifyReport rep;
    CnetBelowBeamReport report;
    Port goal = {PORT_ONEHOT, (size_t)k, 1, ""};
    Port src_residue = {PORT_ONEHOT, (size_t)k, 1, ""};
    Port src_digit = {PORT_ONEHOT, (size_t)b, 1, ""};
    Port decoy_a_out = {PORT_ONEHOT, 3, 1, ""};
    Port decoy_b_out = {PORT_ONEHOT, (size_t)k, 1, ""};
    Port decoy_in = {PORT_ONEHOT, (size_t)k, 1, ""};
    double residue_vec[RES_MAX_K], digit_vec[RES_MAX_B], out[RES_MAX_K];
    DagSource sources[2];
    const int expected = (0 * 4 + 3) % 7;  /* == 3 */
    size_t i, samples;
    clock_t t0, t1;
    double base_ns, probe_ns;
    volatile long sink = 0;

    /* Distinct task tags so a producer is required (goal "r_next" is produced only
       by delta; the start residue source is "r_state"). Same relabel as the v5.1 gate. */
    port_set_tag(&goal, "r_next");
    port_set_tag(&src_residue, "r_state");
    port_set_tag(&src_digit, "digit");
    port_set_tag(&decoy_in, "r_state");
    port_set_tag(&decoy_b_out, "r_other");

    /* Correct producer: residue delta, trained + certified. */
    samples = build_residue_step_data(b, k, inputs, targets);
    train_residue_step(&delta, b, k, inputs, targets, samples, 12345u);
    if (certify_residue_step(&delta, k, inputs, targets, samples, 0.0, &rep) != 0) {
        printf("delta did not certify; aborting benchmark\n");
        btn_free(&delta);
        return 1;
    }
    port_set_tag(&delta.input_ports[0], "r_state");
    port_set_tag(&delta.output_ports[0], "r_next");

    /* Two high-reliability, output-incompatible decoys (realistic registry noise). */
    if (make_decoy(&decoy_a, (size_t)k, 3, decoy_in, decoy_a_out) != 0 ||
        make_decoy(&decoy_b, (size_t)k, (size_t)k, decoy_in, decoy_b_out) != 0) {
        printf("decoy build failed; aborting\n");
        btn_free(&delta); btn_free(&decoy_a); btn_free(&decoy_b);
        return 1;
    }
    decoy_a.output_successes = 100;
    decoy_b.output_successes = 100;
    delta.output_failures = 5;

    /* registry_init default beam = 8: the official planner FINDS delta (rank 2 < 8),
       i.e. the NORMAL no-blind-spot deployment case the probe is paid on per call. */
    registry_init(&reg);
    registry_add(&reg, &decoy_a, "decoy_a");
    registry_add(&reg, &decoy_b, "decoy_b");
    registry_add(&reg, &delta, "residue_step");

    res_onehot(residue_vec, 0, k);
    res_onehot(digit_vec, 3, b);
    sources[0].type = src_residue; sources[0].values = residue_vec;
    sources[1].type = src_digit;   sources[1].values = digit_vec;

    /* ---- baseline: one full route+execute cycle (what the planner normally does) ---- */
    t0 = clock();
    for (i = 0; i < ITERS; ++i) {
        DagPlan plan;
        memset(&plan, 0, sizeof plan);
        if (dag_plan(&reg, sources, 2, goal, &plan) == 0) {
            plan.strict = 1;
            sink += dag_execute(&plan, sources, 2, out, (size_t)k);
            dag_free(&plan);
        }
    }
    t1 = clock();
    base_ns = (double)(t1 - t0) / (double)CLOCKS_PER_SEC * 1e9 / (double)ITERS;

    /* ---- probe: the full v5.1 below-beam probe (official + beam-lifted + 2 strict
       validates + rank reconstruction + Delta-4 snapshot/restore) ---- */
    t0 = clock();
    for (i = 0; i < ITERS; ++i) {
        sink += proposal_sidecar_below_beam_probe(&reg, sources, 2, goal, expected, &report);
    }
    t1 = clock();
    probe_ns = (double)(t1 - t0) / (double)CLOCKS_PER_SEC * 1e9 / (double)ITERS;

    (void)sink;
    printf("=== v5.1 probe-overhead benchmark (ns/call, %zu iters) ===\n", ITERS);
    printf("denominator: one route+execute cycle (dag_plan + dag_execute);\n");
    printf("3-producer registry, beam=8 (delta found, no blind spot -- normal case)\n\n");
    printf("  baseline route+execute : %10.1f ns/call\n", base_ns);
    printf("  v5.1 below-beam probe  : %10.1f ns/call\n", probe_ns);
    printf("  probe overhead         : %.2fx the planner's route+execute cost\n",
           base_ns > 0.0 ? probe_ns / base_ns : 0.0);
    printf("\nShadow-only & opt-in: paid ONLY when proposal_sidecar_below_beam_probe\n");
    printf("is called; nothing calls it in v5.1. This informs a FUTURE activation decision.\n");

    btn_free(&delta); btn_free(&decoy_a); btn_free(&decoy_b);
    registry_free(&reg);
    return 0;
}
