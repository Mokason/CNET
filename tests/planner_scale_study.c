/*
 * planner_scale_study.c -- the collision-factor sweep.
 *
 * Pushes on the central scaling claim ("scale knowledge by adding primitives,
 * not changing the core"). Drives the REAL planner (dag_plan_circuit) over
 * synthetic registries and reads the real search-expansion counter
 * (CircuitPlan.attention.nodes_expanded) plus wall-clock and whether a VALID
 * plan was still found.
 *
 * Synthetic only -- no training, no weights. The planner reads contracts/ports.
 *
 * Two sweeps:
 *   A. BREADTH (irrelevant decoys): does adding distinct-typed primitives that
 *      cannot reach the goal stay cheap? (tests reachability pruning)
 *   B. COLLISION (interchangeable producers): the real combinatorial knob. An
 *      arity-2 combiner C:(M,M)->G whose two M inputs each come from one of k
 *      interchangeable producers S->M. Vary k under three policies:
 *        - default beam (top-8 per slot)
 *        - exhaustive (beam disabled)
 *        - exhaustive + memo off (reachability pruning disabled)
 *      Measures expansions, ms/plan, and found (the incompleteness check:
 *      does the beam ever drop a valid plan once collisions exceed it?).
 *
 * NOT part of `make test`. Budgeted study. Writes artifacts/planner_scale/*.csv.
 */
#include "../include/nn.h"
#include "../include/router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* All "types" are PORT_BINARY_MSB, count 1, distinguished by field width.
 * Distinct width => distinct type => distinct contract. */
static Port P(size_t width) {
    Port p;
    p.family = PORT_BINARY_MSB;
    p.field_width = width;
    p.field_count = 1;
    p.tag[0] = '\0';   /* untagged = wildcard on the tag dimension */
    return p;
}

#define MAXP 2048
static BinaryTransformNetwork pool[MAXP];
static size_t pool_used = 0;

static BinaryTransformNetwork *palloc(void) {
    if (pool_used >= MAXP) { fprintf(stderr, "pool exhausted\n"); exit(2); }
    return &pool[pool_used++];
}
static void pool_reset(void) {
    for (size_t i = 0; i < pool_used; ++i) btn_free(&pool[i]);
    pool_used = 0;
}

/* arity-1 primitive: in_width -> out_width */
static BinaryTransformNetwork *prim1(size_t in_w, size_t out_w) {
    BinaryTransformNetwork *b = palloc();
    if (btn_init(b, in_w, out_w, 1, 4, 0.5, 1u) != 0) { fprintf(stderr, "btn_init\n"); exit(2); }
    if (btn_set_ports(b, P(in_w), P(out_w)) != 0) { fprintf(stderr, "set_ports\n"); exit(2); }
    return b;
}

/* arity-2 primitive: (a_width, b_width) -> out_width */
static BinaryTransformNetwork *prim2(size_t a_w, size_t b_w, size_t out_w) {
    BinaryTransformNetwork *b = palloc();
    Port ins[2] = { P(a_w), P(b_w) };
    if (btn_init(b, a_w + b_w, out_w, 1, 4, 0.5, 1u) != 0) { fprintf(stderr, "btn_init2\n"); exit(2); }
    if (btn_set_input_ports(b, ins, 2, P(out_w)) != 0) { fprintf(stderr, "set_input_ports\n"); exit(2); }
    return b;
}

/* Plan once for telemetry (expansions, found), then time an adaptive rep loop.
 * Mode is OFF (registry_init default): the expansion counter is armed/captured
 * unconditionally inside dag_plan_circuit regardless of attention_mode, so OFF
 * gives the true search cost with no attention overhead. (Do NOT enable SHADOW
 * here: attention_retrieve_top_k stack-overflows for reg->count > 64 -- a real
 * router bug, separate from this study.) */
static void measure(PrimitiveRegistry *reg, const DagSource *sources, size_t n_sources,
                    const Port *goals, size_t n_goals,
                    size_t *expansions, int *found, double *ms_per) {
    CircuitPlan plan;
    int rc = dag_plan_circuit(reg, sources, n_sources, goals, n_goals, &plan);
    *found = (rc == 0);
    *expansions = (rc == 0) ? plan.attention.nodes_expanded : 0;
    if (rc == 0) circuit_free(&plan);

    clock_t t0 = clock();
    size_t reps = 0;
    do {
        CircuitPlan p;
        int r = dag_plan_circuit(reg, sources, n_sources, goals, n_goals, &p);
        if (r == 0) circuit_free(&p);
        ++reps;
    } while ((clock() - t0) < (clock_t)(0.05 * CLOCKS_PER_SEC) && reps < 5000);
    double total_ms = (double)(clock() - t0) / CLOCKS_PER_SEC * 1000.0;
    *ms_per = total_ms / (double)reps;
}

/* ---- Sweep A: breadth / irrelevant decoys ---- */
static void sweep_breadth(FILE *csv) {
    const size_t S = 4, M = 5, G = 6;  /* solution: source S -> M -> G */
    const size_t decoys[] = { 0, 8, 32, 128, 512 };
    const size_t n_cfg = sizeof(decoys) / sizeof(decoys[0]);

    printf("\n=== Sweep A: BREADTH (irrelevant distinct-typed decoys; one S->M->G solution) ===\n");
    printf("decoys | registry_size | found | expansions | ms/plan\n");

    for (size_t ci = 0; ci < n_cfg; ++ci) {
        size_t D = decoys[ci];
        pool_reset();

        BinaryTransformNetwork *p_mid  = prim1(S, M);  /* S -> M */
        BinaryTransformNetwork *p_goal = prim1(M, G);  /* M -> G */

        PrimitiveRegistry reg;
        registry_init(&reg);
        registry_add(&reg, p_mid,  "mid");
        registry_add(&reg, p_goal, "goal");
        for (size_t i = 0; i < D; ++i) {
            /* unrelated types, unreachable from S and never producing M or G */
            BinaryTransformNetwork *d = prim1(10 + 2 * i, 11 + 2 * i);
            registry_add(&reg, d, "decoy");
        }

        DagSource src = { P(S), NULL };
        Port goal = P(G);
        size_t exp; int found; double ms;
        measure(&reg, &src, 1, &goal, 1, &exp, &found, &ms);

        printf("%6zu | %13zu | %5d | %10zu | %.5f\n", D, reg.count, found, exp, ms);
        if (csv) fprintf(csv, "breadth,%zu,%zu,default,1,%d,%zu,%.6f\n", D, reg.count, found, exp, ms);

        registry_free(&reg);
    }
}

/* ---- Sweep B: collision factor ---- */
static void run_collision_cfg(FILE *csv, size_t k, const char *mode,
                              size_t beam, int memo_off) {
    const size_t S = 4, M = 5, G = 6;
    pool_reset();

    BinaryTransformNetwork *comb = prim2(M, M, G);  /* the only G producer: (M,M)->G */

    PrimitiveRegistry reg;
    registry_init(&reg);
    registry_add(&reg, comb, "combine");
    for (size_t j = 0; j < k; ++j) {
        BinaryTransformNetwork *p = prim1(S, M);    /* k interchangeable S->M */
        registry_add(&reg, p, "producer");
    }
    reg.dag_beam_limit = beam;      /* registry_init default is 8; beam==0 means UNLIMITED (router.c:2150) */
    reg.disable_plan_memo = memo_off;

    DagSource sources[2] = { { P(S), NULL }, { P(S), NULL } };
    Port goal = P(G);
    size_t exp; int found; double ms;
    measure(&reg, sources, 2, &goal, 1, &exp, &found, &ms);

    printf("%4zu | %-18s | %5d | %10zu | %.5f\n", k, mode, found, exp, ms);
    if (csv) fprintf(csv, "collision,%zu,%zu,%s,%zu,%d,%zu,%.6f\n",
                     k, reg.count, mode, beam, found, exp, ms);

    registry_free(&reg);
}

static void sweep_collision(FILE *csv) {
    const size_t ks[] = { 1, 2, 4, 8, 16, 32, 64, 128 };
    const size_t n_k = sizeof(ks) / sizeof(ks[0]);

    /* registry_init default beam = 8; beam==0 means UNLIMITED (router.c:2150). */
    printf("\n=== Sweep B: COLLISION (k interchangeable producers per needed type; arity-2 target) ===\n");
    printf("   k | mode               | found | expansions | ms/plan\n");
    for (size_t i = 0; i < n_k; ++i) run_collision_cfg(csv, ks[i], "beam8_default", 8, 0);
    printf("   -----\n");
    for (size_t i = 0; i < n_k; ++i) run_collision_cfg(csv, ks[i], "unlimited", 0, 0);
    printf("   -----\n");
    for (size_t i = 0; i < n_k; ++i) run_collision_cfg(csv, ks[i], "unlimited_memo_off", 0, 1);
}

/* ---- Sweep C: depth (left-deep combiner chain, like ripple-carry / scan) ----
 * A chain of unique arity-2 combiners; the only collision is k interchangeable
 * leaf producers S->M. Combiner-depth d has (d+1) leaf slots, so the exponent
 * should be (d+1): expansions ~ k^(d+1). DAG depth used = d+1 (<= DAG_MAX_DEPTH). */
static void run_depth_cfg(FILE *csv, size_t d, size_t k) {
    const size_t S = 4, M = 5;
    pool_reset();

    PrimitiveRegistry reg;
    registry_init(&reg);

    /* level 1: (M,M)->R1 (width 6); level i: (M, R_{i-1})->R_i (width 5+i) */
    BinaryTransformNetwork *c1 = prim2(M, M, 5 + 1);
    registry_add(&reg, c1, "comb1");
    size_t r_prev = 5 + 1;
    for (size_t i = 2; i <= d; ++i) {
        BinaryTransformNetwork *ci = prim2(M, r_prev, 5 + i);  /* fresh M + running result */
        registry_add(&reg, ci, "comb");
        r_prev = 5 + i;
    }
    for (size_t j = 0; j < k; ++j) registry_add(&reg, prim1(S, M), "producer");

    reg.dag_beam_limit = 0;              /* UNLIMITED: expose the true k^(d+1) growth.
                                            (Under the default beam=8 this saturates at
                                            k>8, exactly as the collision sweep shows.) */
    size_t nsrc = d + 1;                 /* one fresh leaf per combiner, +1 */
    DagSource sources[16];
    for (size_t s = 0; s < nsrc; ++s) { sources[s].type = P(S); sources[s].values = NULL; }
    Port goal = P(5 + d);                /* R_d */

    size_t exp; int found; double ms;
    measure(&reg, sources, nsrc, &goal, 1, &exp, &found, &ms);

    printf("d=%zu (leaves=%zu) k=%3zu | found=%d | expansions=%8zu | %.5f ms\n",
           d, d + 1, k, found, exp, ms);
    if (csv) fprintf(csv, "depth,%zu,%zu,d%zu,0,%d,%zu,%.6f\n", k, reg.count, d, found, exp, ms);

    registry_free(&reg);
}

static void sweep_depth(FILE *csv) {
    const size_t ds[] = { 1, 2, 3 };
    const size_t ks[] = { 2, 4, 8, 16 };
    printf("\n=== Sweep C: DEPTH (left-deep chain; exponent should track #leaves = d+1) ===\n");
    for (size_t di = 0; di < sizeof(ds)/sizeof(ds[0]); ++di) {
        for (size_t ki = 0; ki < sizeof(ks)/sizeof(ks[0]); ++ki)
            run_depth_cfg(csv, ds[di], ks[ki]);
        printf("   -----\n");
    }
}

int main(void) {
    printf("=== Planner scale study (real dag_plan_circuit; real expansion counter) ===\n");
    printf("DAG_MAX_DEPTH=%d DAG_MAX_SLOTS=%d default_beam=8\n", DAG_MAX_DEPTH, DAG_MAX_SLOTS);

    FILE *csv = fopen("artifacts/planner_scale/scale_study.csv", "w");
    if (csv) fprintf(csv, "sweep,k_or_decoys,registry_size,mode,beam,found,expansions,ms_per_plan\n");

    sweep_breadth(csv);
    sweep_collision(csv);
    sweep_depth(csv);

    if (csv) { fclose(csv); printf("\nWrote artifacts/planner_scale/scale_study.csv\n"); }

    pool_reset();
    return 0;
}
