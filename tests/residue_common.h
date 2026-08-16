/*
 * residue_common.h -- shared helpers for the mod-k residue-scan experiment.
 * delta(r,d) = (r*b + d) mod k. State = residue (ONEHOT k); digit = ONEHOT b;
 * output = next residue (ONEHOT k). Static functions, included by both
 * test_residue.c and residue_study.c; compile with -Wno-unused-function.
 */
#ifndef RESIDUE_COMMON_H
#define RESIDUE_COMMON_H

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/scan.h"

#include <stdlib.h>
#include <string.h>

#define RES_MAX_K 16
#define RES_MAX_B 16
#define RES_MAX_N 64

/* Ground truth: residue of an N-digit base-b number, MSB-first. */
static int string_residue(const int *digits, size_t n, int b, int k) {
    int r = 0;
    size_t i;
    for (i = 0; i < n; ++i) r = (r * b + digits[i]) % k;
    return r;
}

/* One-hot encode v in [0,width) into width doubles. */
static void res_onehot(double *vec, int v, int width) {
    int i;
    for (i = 0; i < width; ++i) vec[i] = (i == v) ? 1.0 : 0.0;
}

/* Argmax over k doubles. */
static int res_argmax(const double *v, int k) {
    int j, best = 0;
    double bv = v[0];
    for (j = 1; j < k; ++j) if (v[j] > bv) { bv = v[j]; best = j; }
    return best;
}

/* Ground-truth transition for one residue step (used by the generalized builder).
   ctx is int[2] = {b, k}. */
static void residue_step_transition(const int *in_codes, size_t num_in, int *out_codes, size_t num_out, void *ctx) {
    (void)num_in; (void)num_out;
    int *p = (int *)ctx;
    int r = in_codes[0];
    int d = in_codes[1];
    out_codes[0] = (r * p[0] + d) % p[1];
}

/* Build delta's full training table using the new core generalized builder
   with proper ctx (no globals). */
static size_t build_residue_step_data(int b, int k, double *inputs, double *targets) {
    static int p[2];
    p[0] = b;
    p[1] = k;

    StepShape shape = {0};
    shape.num_in_ports = 2;
    shape.in_widths[0] = (size_t)k;   /* residue */
    shape.in_widths[1] = (size_t)b;   /* digit */
    shape.num_out_ports = 1;
    shape.out_widths[0] = (size_t)k;

    return build_step_training_data(&shape, residue_step_transition, p, inputs, targets);
}

/* Init + train delta on its table. Caller btn_free's *btn. Returns final loss. */
static double train_residue_step(BinaryTransformNetwork *btn, int b, int k,
                                 const double *inputs, const double *targets,
                                 size_t samples, unsigned int seed) {
    Port in[2];
    Port out = {PORT_ONEHOT, (size_t)k, 1, ""};
    in[0] = (Port){PORT_ONEHOT, (size_t)k, 1, ""};   /* residue */
    in[1] = (Port){PORT_ONEHOT, (size_t)b, 1, ""};   /* digit */
    port_set_tag(&in[0], "residue");
    port_set_tag(&in[1], "digit");
    port_set_tag(&out, "residue");
    btn_init(btn, (size_t)k + (size_t)b, (size_t)k, 8, 64, 0.5, seed);
    btn_set_input_ports(btn, in, 2, out);
    return btn_train_dynamic(btn, inputs, targets, samples,
                             200000, 1000, 0.0008, 0.02);
}

/* Certify delta exactly on its table (canonicalize 0.9/0.1 -> 0/1 first).
   Returns 0 if certified at margin >= floor, -1 otherwise; fills *report. */
static int certify_residue_step(BinaryTransformNetwork *btn, int k,
                                const double *inputs, const double *targets,
                                size_t samples, double floor,
                                CertifyReport *report) {
    Contract c;
    double *canon;
    size_t i;
    int rc;
    canon = (double *)malloc(samples * (size_t)k * sizeof(double));
    if (canon == NULL) return -1;
    for (i = 0; i < samples; ++i)
        if (port_canonicalize(btn->output_ports[0],
                              targets + i * (size_t)k,
                              canon + i * (size_t)k) != 0) { free(canon); return -1; }
    if (contract_init_borrowed(&c, "residue_step", btn, inputs, canon, samples) != 0) {
        free(canon);
        return -1;
    }
    rc = btn_certify_robust(btn, &c, floor, report);
    free(canon);  /* contract borrowed the tables; nothing else to free */
    return rc;
}

/* Run the residue scan over an N-digit string (digits[i] in [0,b)).
   Builds a hand-built DAG of n delta-nodes (the same delta pointer reused),
   runs dag_execute, returns the predicted residue (argmax of the final state),
   or -1 on executor failure / bad n. */
static int run_scan(BinaryTransformNetwork *delta, int b, int k,
                    const int *digits, size_t n) {
    DagNode start;
    DagNode dn[RES_MAX_N];      /* digit sources */
    DagNode steps[RES_MAX_N];   /* delta nodes */
    DagSource srcs[1 + RES_MAX_N];
    DagPlan plan;
    double start_vec[RES_MAX_K];
    static double digit_vec[RES_MAX_N][RES_MAX_B];
    double out[RES_MAX_K];
    size_t i;

    if (n == 0 || n > RES_MAX_N) return -1;
    memset(&plan, 0, sizeof plan);
    memset(&start, 0, sizeof start);

    res_onehot(start_vec, 0, k);                 /* start residue = 0 */
    start.kind = DAG_SOURCE;
    start.source_index = 0;
    srcs[0].type = (Port){PORT_ONEHOT, (size_t)k, 1, ""};
    port_set_tag(&srcs[0].type, "residue");
    srcs[0].values = start_vec;

    for (i = 0; i < n; ++i) {
        res_onehot(digit_vec[i], digits[i], b);
        memset(&dn[i], 0, sizeof dn[i]);
        dn[i].kind = DAG_SOURCE;
        dn[i].source_index = (int)(1 + i);
        srcs[1 + i].type = (Port){PORT_ONEHOT, (size_t)b, 1, ""};
        port_set_tag(&srcs[1 + i].type, "digit");
        srcs[1 + i].values = digit_vec[i];
    }

    /* Use the reusable iterative scan builder (new core facility in router)
       instead of manual child/child_ports wiring. This directly reduces the
       hand-construction tax for new "certified step + structured scan" domains,
       making it easier to push the enumerable-boundary wall (certify the tiny
       step table once; the builder + canonicalization give you the long
       composition by induction). Layering (parent = step contract) can now be
       applied to the overall scan contract with almost no extra code. */
    {
        StepWiring w = {0};
        w.n_state_slots = 1;
        w.state_in_slots[0] = 0;
        w.state_out_ports[0] = 0;
        w.n_data_slots = 1;
        w.data_in_slots[0] = 1;

        if (dag_build_iterative_scan(delta, "residue_step", &w,
                                     &start, 1,
                                     dn, n,
                                     steps, &plan) != 0) {
            return -1;
        }
    }
    /* plan.root and wiring are now set by the builder; owned remains NULL for hand-built storage */

    if (dag_execute(&plan, srcs, 1 + n, out, (size_t)k) != 0) return -1;
    return res_argmax(out, k);
}

/* Build a flat training/eval row: an N-digit string -> [N one-hot-b inputs |
   onehot-k residue target]. inputs row is n*b wide; target row is k wide
   (0.9/0.1). digits[] are the n digits. */
static void build_flat_row(int b, int k, size_t n, const int *digits,
                           double *in_row, double *tg_row) {
    size_t i; int j, r;
    for (i = 0; i < n; ++i)
        for (j = 0; j < b; ++j) in_row[i * (size_t)b + j] = (digits[i] == j) ? 1.0 : 0.0;
    r = string_residue(digits, n, b, k);
    for (j = 0; j < k; ++j) tg_row[j] = (j == r) ? 0.9 : 0.1;
}

/* Init + train the flat monolith: whole string (n*b one-hot) -> residue (k). */
static double train_flat(BinaryTransformNetwork *btn, int b, int k, size_t n,
                         const double *inputs, const double *targets,
                         size_t samples, unsigned int seed) {
    Port in = {PORT_ONEHOT, (size_t)b, n, ""};     /* n fields of one-hot-b */
    Port out = {PORT_ONEHOT, (size_t)k, 1, ""};
    btn_init(btn, n * (size_t)b, (size_t)k, 16, 128, 0.3, seed);
    btn_set_ports(btn, in, out);
    return btn_train_dynamic(btn, inputs, targets, samples,
                             200000, 2000, 0.0005, 0.01);
}

/* Predict residue from a flat net for one string. */
static int flat_predict(BinaryTransformNetwork *btn, int b, size_t n,
                        const int *digits, int k) {
    static double in_row[RES_MAX_N * RES_MAX_B];
    const double *out;
    size_t i; int j;
    for (i = 0; i < n; ++i)
        for (j = 0; j < b; ++j) in_row[i * (size_t)b + j] = (digits[i] == j) ? 1.0 : 0.0;
    out = btn_forward(btn, in_row);
    return res_argmax(out, k);
}

#endif

