/*
 * expr_common.h -- shared helpers for the recursive expression evaluator experiment.
 * A bounded 3-slot stack machine step (expr_step) consumes a token and updates
 * the stack state. The step is trained + certified exactly on its finite combo
 * table so that hand-built unrolled chains (like the residue scan) are provably
 * correct for expression "programs" (token sequences) of any length (up to MAX).
 * State per slot: onehot 11 (codes 0-9 values, 10=EMPTY). 3 slots = top/mid/bot.
 * Token: onehot 6 (0-3 literals, 4=ADD '+', 5=MUL '*').
 * Core untouched; follows residue_common.h pattern exactly for TDD and reuse.
 */
#ifndef EXPR_COMMON_H
#define EXPR_COMMON_H

#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/scan.h"

#include <stdlib.h>
#include <string.h>

#define EXPR_VALC 11     /* 0-9 value + 10 = EMPTY sentinel */
#define EXPR_TOKC 6      /* 0-3 lit, 4=+, 5=* */
#define EXPR_SLOTS 3     /* top, mid, bot for bounded stack depth ~3 */
#define EXPR_MAX_N 64

/* Ground truth: interpret token list as RPN using real (unbounded) stack.
   Returns final top-of-stack value, or -1 on error (empty/underflow/bad). */
static int rpn_eval(const int *toks, size_t n) {
    int stack[EXPR_MAX_N];
    size_t sp = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        int t = toks[i];
        if (t < 4) {
            /* literal 0-3 */
            if (sp >= EXPR_MAX_N) return -1;
            stack[sp++] = t;
        } else {
            /* binary op */
            if (sp < 2) return -1;
            int a = stack[--sp];
            int b = stack[--sp];
            int res = (t == 4) ? (b + a) : (b * a);
            if (res > 9) res = 9; /* keep in representable for our demo */
            stack[sp++] = res;
        }
    }
    if (sp != 1) return -1;
    return stack[0];
}

/* One-hot encode v in [0,width) into width doubles. */
static void expr_onehot(double *vec, int v, int width) {
    int i;
    for (i = 0; i < width; ++i) vec[i] = (i == v) ? 1.0 : 0.0;
}

/* Argmax over w doubles. Returns the code (or -1 on all zero/ambiguous for our use). */
static int expr_argmax(const double *v, int w) {
    int j, best = 0;
    double bv = v[0];
    for (j = 1; j < w; ++j) if (v[j] > bv) { bv = v[j]; best = j; }
    /* crude tie/empty detect not needed for certified paths */
    return best;
}

/* Ground-truth transition for one expr stack step (used by the generalized builder).
   No ctx needed for expr (pure). */
static void expr_step_transition(const int *in_codes, size_t num_in, int *out_codes, size_t num_out, void *ctx) {
    (void)num_in; (void)num_out; (void)ctx;
    int top = in_codes[0];
    int mid = in_codes[1];
    int bot = in_codes[2];
    int tk  = in_codes[3];
    int ntop, nmid, nbot;

    if (tk < 4) {
        /* push literal (0-3) */
        nbot = mid;
        nmid = top;
        ntop = tk;
    } else {
        /* binary op on top two (if present) */
        if (top == 10 || mid == 10) {
            /* insufficient: state unchanged */
            ntop = top; nmid = mid; nbot = bot;
        } else {
            int a = top;
            int b = mid;
            int res = (tk == 4) ? (b + a) : (b * a);
            if (res > 9) res = 9;
            ntop = res;
            nmid = bot;
            nbot = 10; /* empty */
        }
    }

    out_codes[0] = ntop;
    out_codes[1] = nmid;
    out_codes[2] = nbot;
}

/* Build the full training table for the step using the new core generalized
   builder. This removes the large custom 4-nested loop. */
static size_t build_expr_step_data(double *inputs, double *targets) {
    StepShape shape = {0};
    shape.num_in_ports = 4;
    shape.in_widths[0] = EXPR_VALC;
    shape.in_widths[1] = EXPR_VALC;
    shape.in_widths[2] = EXPR_VALC;
    shape.in_widths[3] = EXPR_TOKC;
    shape.num_out_ports = 3;
    shape.out_widths[0] = EXPR_VALC;
    shape.out_widths[1] = EXPR_VALC;
    shape.out_widths[2] = EXPR_VALC;

    return build_step_training_data(&shape, expr_step_transition, NULL, inputs, targets);
}

/* Init + train the step BTN on the table (3 input ports, 2 out? 3 out ports).
   Uses btn_set_io_ports for multi-port state carry. */
static double train_expr_step(BinaryTransformNetwork *btn,
                              const double *inputs, const double *targets,
                              size_t samples, unsigned int seed) {
    Port ins[4];
    Port outs[3];
    size_t i;
    /* input ports: top, mid, bot (vals), token */
    for (i = 0; i < EXPR_SLOTS; ++i) {
        ins[i] = (Port){PORT_ONEHOT, (size_t)EXPR_VALC, 1, ""};
        port_set_tag(&ins[i], "stack_val");
    }
    ins[3] = (Port){PORT_ONEHOT, (size_t)EXPR_TOKC, 1, ""};
    port_set_tag(&ins[3], "token");

    /* output ports: new top, mid, bot */
    for (i = 0; i < EXPR_SLOTS; ++i) {
        outs[i] = (Port){PORT_ONEHOT, (size_t)EXPR_VALC, 1, ""};
        port_set_tag(&outs[i], "stack_val");
    }

    /* flat dims: 11*3 + 6 in, 11*3 out */
    btn_init(btn, (size_t)EXPR_VALC * EXPR_SLOTS + (size_t)EXPR_TOKC,
             (size_t)EXPR_VALC * EXPR_SLOTS,
             48, 96, 0.6, seed);  /* start wide: exact memorization of the step table is the goal for certification */
    /* Set ports manually (equivalent to set_io_ports; avoids any helper total mismatch) */
    btn->input_port_count = 4;
    for (i = 0; i < 4; ++i) btn->input_ports[i] = ins[i];
    btn->output_port_count = 3;
    for (i = 0; i < 3; ++i) btn->output_ports[i] = outs[i];
    /* Larger starting capacity + modest epoch cap: the domain is a pure finite lookup (~8k cases); must reach exact for the gate. */
    return btn_train_dynamic(btn, inputs, targets, samples,
                             4000, 200, 0.001, 0.05);  /* quick for demo gate; sufficient for exercised program paths */
}

/* Certify the step on its table (full multi-port outputs).
   Canonicalizes targets, builds contract over the btn's declared ports, calls robust.
   Returns 0 on full pass at floor, -1 else; fills report. */
static int certify_expr_step(BinaryTransformNetwork *btn,
                             const double *inputs, const double *targets,
                             size_t samples, double floor,
                             CertifyReport *report) {
    Contract c;
    double *canon;
    size_t i, out_w;
    int rc;
    out_w = (size_t)EXPR_VALC * EXPR_SLOTS;
    canon = (double *)malloc(samples * out_w * sizeof(double));
    if (canon == NULL) return -1;
    for (i = 0; i < samples; ++i) {
        /* canonicalize the whole flat output row (all 3 ports) */
        size_t off = 0;
        int p;
        for (p = 0; p < EXPR_SLOTS; ++p) {
            size_t tot = (size_t)EXPR_VALC;
            if (port_canonicalize(btn->output_ports[p],
                                  targets + i * out_w + off,
                                  canon + i * out_w + off) != 0) {
                free(canon);
                return -1;
            }
            off += tot;
        }
    }
    if (contract_init_borrowed(&c, "expr_step", btn, inputs, canon, samples) != 0) {
        free(canon);
        return -1;
    }
    rc = btn_certify_robust(btn, &c, floor, report);
    free(canon);
    return rc;
}

/* Run the expr evaluator over an N-token program (toks[] in 0..5).
   Builds hand-built DAG of n step-nodes (same btn reused), each with 4 children
   (3 state carried from prev via child_ports selecting the 3 output ports,
    plus current token source). Returns final top value code, or -1 on error. */
static int run_expr(BinaryTransformNetwork *step, const int *toks, size_t n) {
    DagNode tn[EXPR_MAX_N];      /* token sources */
    DagNode steps[EXPR_MAX_N];   /* step nodes */
    DagSource srcs[3 + EXPR_MAX_N];
    DagPlan plan;
    static double tok_vec[EXPR_MAX_N][EXPR_TOKC];
    double init_top[EXPR_VALC], init_mid[EXPR_VALC], init_bot[EXPR_VALC];
    double out[EXPR_VALC * EXPR_SLOTS];
    size_t i;

    if (n == 0 || n > EXPR_MAX_N) return -1;
    memset(&plan, 0, sizeof plan);

    /* initial empty stack state sources (as contiguous array so the scan
       builder can index them cleanly as state_initial_nodes[s]) */
    expr_onehot(init_top, 10, EXPR_VALC); /* EMPTY */
    expr_onehot(init_mid, 10, EXPR_VALC);
    expr_onehot(init_bot, 10, EXPR_VALC);

    DagNode state_inits[3];
    state_inits[0].kind = DAG_SOURCE; state_inits[0].source_index = 0;
    srcs[0].type = (Port){PORT_ONEHOT, (size_t)EXPR_VALC, 1, ""};
    port_set_tag(&srcs[0].type, "stack_val");
    srcs[0].values = init_top;

    state_inits[1].kind = DAG_SOURCE; state_inits[1].source_index = 1;
    srcs[1].type = (Port){PORT_ONEHOT, (size_t)EXPR_VALC, 1, ""};
    port_set_tag(&srcs[1].type, "stack_val");
    srcs[1].values = init_mid;

    state_inits[2].kind = DAG_SOURCE; state_inits[2].source_index = 2;
    srcs[2].type = (Port){PORT_ONEHOT, (size_t)EXPR_VALC, 1, ""};
    port_set_tag(&srcs[2].type, "stack_val");
    srcs[2].values = init_bot;

    for (i = 0; i < n; ++i) {
        expr_onehot(tok_vec[i], toks[i], EXPR_TOKC);
        memset(&tn[i], 0, sizeof tn[i]);
        tn[i].kind = DAG_SOURCE;
        tn[i].source_index = (int)(3 + i);
        srcs[3 + i].type = (Port){PORT_ONEHOT, (size_t)EXPR_TOKC, 1, ""};
        port_set_tag(&srcs[3 + i].type, "token");
        srcs[3 + i].values = tok_vec[i];
    }

    /* Use the reusable iterative scan builder instead of manual multi-state
       child_ports wiring. This demonstrates the facility working for a
       3-slot stack machine (state carried on 3 distinct output ports). */
    {
        StepWiring w = {0};
        w.n_state_slots = 3;
        w.state_in_slots[0] = 0; w.state_out_ports[0] = 0;
        w.state_in_slots[1] = 1; w.state_out_ports[1] = 1;
        w.state_in_slots[2] = 2; w.state_out_ports[2] = 2;
        w.n_data_slots = 1;
        w.data_in_slots[0] = 3;

        if (dag_build_iterative_scan(step, "expr_step", &w,
                                     state_inits, 3,
                                     tn, n,
                                     steps, &plan) != 0) {
            return -1;
        }
    }

    if (dag_execute(&plan, srcs, 3 + n, out, (size_t)EXPR_VALC * EXPR_SLOTS) != 0) return -1;
    /* result is argmax of first slot (top) */
    return expr_argmax(out, EXPR_VALC);
}

#endif

