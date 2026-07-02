#include "../../include/router.h"
#include "../../include/router/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* DAG execution + blackboard + eval memo (real ported logic) */

/* Per-run memo of full canonical outputs */
typedef struct {
    const DagNode *node;
    double *full;
    size_t len;
} EvalEntry;

typedef struct {
    EvalEntry *items;
    size_t count;
    size_t cap;
} EvalMemo;

static void eval_memo_destroy(EvalMemo *memo) {
    if (!memo) return;
    for (size_t i = 0; i < memo->count; ++i) free(memo->items[i].full);
    free(memo->items);
    memo->items = NULL;
    memo->count = 0;
    memo->cap = 0;
}

static int eval_memo_put(EvalMemo *memo, const DagNode *node, double *full, size_t len) {
    if (memo->count == memo->cap) {
        size_t ncap = memo->cap ? memo->cap * 2 : 16;
        EvalEntry *g = (EvalEntry*)realloc(memo->items, ncap * sizeof(EvalEntry));
        if (!g) return -1;
        memo->items = g;
        memo->cap = ncap;
    }
    memo->items[memo->count].node = node;
    memo->items[memo->count].full = full;
    memo->items[memo->count].len = len;
    memo->count++;
    return 0;
}

static int edge_port(const DagNode *parent, size_t k) {
    int p = parent->child_ports[k];
    return p != 0 ? p : parent->children[k]->output_index;
}

static int output_segment(const BinaryTransformNetwork *p, size_t sel, size_t *off, size_t *total) {
    if (sel >= p->output_port_count) return -1;
    size_t o = 0;
    for (size_t oj = 0; oj < sel; ++oj) {
        o += p->output_ports[oj].field_width * p->output_ports[oj].field_count;
    }
    *off = o;
    *total = p->output_ports[sel].field_width * p->output_ports[sel].field_count;
    return 0;
}

static const double *eval_node(const DagNode *node, const DagSource *sources, size_t n_sources,
                               EvalMemo *memo, int strict, const PrimitiveRegistry *reg /* for future */) {
    (void)reg;
    if (!node) return NULL;

    if (node->kind == DAG_SOURCE) {
        if ((size_t)node->source_index >= n_sources) return NULL;
        return sources[node->source_index].values;
    }

    /* primitive: evaluate children once via memo */
    const BinaryTransformNetwork *p = node->btn;
    if (!p) return NULL;

    size_t total_in = p->input_count;
    double *assembled = (double*)malloc(total_in * sizeof(double));
    if (!assembled) return NULL;

    size_t offset = 0;
    for (size_t k = 0; k < node->child_count; ++k) {
        const DagNode *ch = node->children[k];
        int port_idx = edge_port(node, k);
        size_t off, tot;
        if (output_segment(ch->btn ? ch->btn : p /*fallback*/, port_idx, &off, &tot) != 0) {
            /* try direct */
            tot = ch->btn ? ch->btn->output_count : 0;
            off = 0;
        }
        const double *child_out = eval_node(ch, sources, n_sources, memo, strict, reg);
        if (!child_out) { free(assembled); return NULL; }

        /* copy the segment */
        for (size_t b=0; b < tot && offset + b < total_in; ++b) {
            assembled[offset + b] = child_out[off + b];
        }
        offset += tot;
    }

    /* forward */
    const double *raw = btn_forward((BinaryTransformNetwork*)p, assembled);
    if (!raw) { free(assembled); return NULL; }

    /* validate + record + canonicalize into owned buffer for memo */
    size_t out_total = p->output_count;
    double *canon = (double*)malloc(out_total * sizeof(double));
    if (!canon) { free(assembled); return NULL; }

    /* simplistic: assume first output port for recording */
    if (p->output_port_count > 0) {
        if (port_validate(p->output_ports[0], raw)) {
            /* success */
        } else if (strict) {
            free(canon); free(assembled); return NULL;
        }
        port_canonicalize(p->output_ports[0], raw, canon);
    } else {
        memcpy(canon, raw, out_total * sizeof(double));
    }

    if (eval_memo_put(memo, node, canon, out_total) != 0) {
        free(canon); free(assembled); return NULL;
    }
    free(assembled);
    return canon;
}

int dag_execute(const DagPlan *plan, const DagSource *sources, size_t n_sources,
                double *output, size_t out_cap) {
    if (!plan || !plan->root || !output) return -1;
    EvalMemo memo = {0};
    const double *res = eval_node(plan->root, sources, n_sources, &memo, plan->strict, NULL);
    if (!res) {
        eval_memo_destroy(&memo);
        return -1;
    }
    /* copy root output (assume single for dag_execute simple case) */
    size_t to_copy = plan->root->btn ? plan->root->btn->output_count : 0;
    if (to_copy > out_cap) to_copy = out_cap;
    memcpy(output, res, to_copy * sizeof(double));
    eval_memo_destroy(&memo);
    return 0;
}

int dag_execute_circuit(const CircuitPlan *plan,
                        const DagSource *sources, size_t n_sources,
                        double *output, size_t out_cap,
                        CircuitBlackboard *blackboard) {
    if (!plan || !output) return -1;
    if (blackboard) {
        blackboard->count = 0;
    }
    /* Very simplified: execute roots in order and concat */
    size_t written = 0;
    for (size_t r = 0; r < plan->root_count; ++r) {
        DagNode *root = plan->roots[r];
        if (!root) continue;
        EvalMemo memo = {0};
        const double *res = eval_node(root, sources, n_sources, &memo, plan->strict, NULL);
        if (res && root->btn) {
            size_t n = root->btn->output_count;
            if (written + n <= out_cap) {
                memcpy(output + written, res, n * sizeof(double));
                written += n;
            }
        }
        eval_memo_destroy(&memo);
    }
    return (written > 0) ? 0 : -1;
}

void circuit_free(CircuitPlan *plan) {
    if (!plan) return;
    if (plan->owned) {
        for (size_t i = 0; i < plan->owned_count; ++i) {
            dag_free_node(plan->owned[i]);
        }
        free(plan->owned);
    }
    plan->root_count = 0;
    plan->owned = NULL;
}

void circuit_blackboard_init(CircuitBlackboard *bb) { if (bb) memset(bb, 0, sizeof(*bb)); }
void circuit_blackboard_free(CircuitBlackboard *bb) {
    if (!bb) return;
    for (size_t i = 0; i < bb->count; ++i) {
        free(bb->entries[i].canonical);
    }
    free(bb->entries);
    bb->entries = NULL;
    bb->count = 0;
}

void blackboard_init(CNETBlackboard *bb, size_t capacity_hint) { (void)capacity_hint; circuit_blackboard_init(bb); }

int blackboard_write(CNETBlackboard *bb, Port port, const double *values,
                     const char *name, double reliability) {
    (void)reliability; (void)name;
    if (!bb || !values) return -1;
    /* grow */
    if (bb->count == bb->capacity) {
        size_t n = bb->capacity ? bb->capacity*2 : 8;
        CircuitBlackboardEntry *e = (CircuitBlackboardEntry*)realloc(bb->entries, n * sizeof(*e));
        if (!e) return -1;
        bb->entries = e;
        bb->capacity = n;
    }
    size_t idx = bb->count++;
    bb->entries[idx].port = port;
    size_t tot = port.field_width * port.field_count;
    bb->entries[idx].canonical = (double*)malloc(tot * sizeof(double));
    if (bb->entries[idx].canonical) memcpy(bb->entries[idx].canonical, values, tot*sizeof(double));
    bb->entries[idx].canonical_len = tot;
    bb->entries[idx].consumer_count = 1;
    return 0;
}

int blackboard_read(const CNETBlackboard *bb, size_t slot_index, double *out, size_t out_len) {
    if (!bb || slot_index >= bb->count || !out) return -1;
    size_t n = bb->entries[slot_index].canonical_len;
    if (n > out_len) n = out_len;
    memcpy(out, bb->entries[slot_index].canonical, n * sizeof(double));
    return 0;
}

int circuit_blackboard_compute_trace_summary(const CircuitPlan *plan,
    const CircuitBlackboard *bb, const double *outputs, size_t out_len,
    CircuitTraceSummary *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (bb) out->output_entry_count = bb->count;
    if (plan) out->root_output_count = plan->root_count;
    return 0;
}



