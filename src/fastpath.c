/*
 * fastpath.c -- a separate, batched route executor. It repacks frozen BTN
 * weights into a chosen numeric kind (f64/f32/int) ONCE, reuses scratch across
 * the whole stream, and snaps every handoff exactly like route_execute. The
 * double core (nn.c/router.c) is untouched and remains the oracle this is
 * checked against. Reduction order matches btn_forward so the f64 path is
 * bit-identical to the oracle, per sample.
 */
#include "../include/fastpath.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static double fp_sigmoid(double x) { return 1.0 / (1.0 + exp(-x)); }

typedef struct {
    size_t in;     /* input_count */
    size_t out;    /* output_count */
    size_t hid;    /* live hidden_count */
    Port in_port;
    Port out_port;
    /* Dense, hid-strided packs (representation depends on cr->prec.kind). */
    double *w_ih;  /* [hid * in]  row-major by hidden */
    double *w_ho;  /* [out * hid] row-major by output */
    double *b_h;   /* [hid] */
    double *b_o;   /* [out] */
    float  *w_ih_f; float *w_ho_f; float *b_h_f; float *b_o_f;
    int8_t *w_ih_q; int8_t *w_ho_q;
    double w_ih_scale; double w_ho_scale;
    int int_bits;
} FpStep;

struct CompiledRoute {
    FastPrecision prec;
    size_t batch;
    FpStep *steps;
    size_t length;
    size_t in_total;
    size_t out_total;
    /* scratch, reused across samples AND steps */
    double *cur;   /* batch * max_width */
    double *nxt;   /* batch * max_width */
    double *hidv;  /* batch * max_hidden */
    double min_margin;
};

double fp_route_min_margin(const CompiledRoute *cr) {
    return cr ? cr->min_margin : 0.0;
}
void fp_route_reset_margin(CompiledRoute *cr) {
    if (cr) cr->min_margin = 0.5;
}
size_t fp_route_in_total(const CompiledRoute *cr) { return cr ? cr->in_total : 0; }
size_t fp_route_out_total(const CompiledRoute *cr) { return cr ? cr->out_total : 0; }

static void fp_step_free_buffers(FpStep *st) {
    free(st->w_ih); free(st->w_ho); free(st->b_h); free(st->b_o);
    free(st->w_ih_f); free(st->w_ho_f); free(st->b_h_f); free(st->b_o_f);
    free(st->w_ih_q); free(st->w_ho_q);
}

void fp_route_free(CompiledRoute *cr) {
    size_t s;
    if (cr == NULL) return;
    if (cr->steps) {
        for (s = 0; s < cr->length; ++s)
            fp_step_free_buffers(&cr->steps[s]);
        free(cr->steps);
    }
    free(cr->cur); free(cr->nxt); free(cr->hidv);
    free(cr);
}

/* Pack one step's weights as dense f64 (hid-strided), reading the canonical
   btn layout described in the plan header. */
static int pack_step_f64(FpStep *st, const BinaryTransformNetwork *btn) {
    size_t i, h, o;
    st->w_ih = malloc(st->hid * st->in * sizeof(double));
    st->w_ho = malloc(st->out * st->hid * sizeof(double));
    st->b_h  = malloc(st->hid * sizeof(double));
    st->b_o  = malloc(st->out * sizeof(double));
    if (!st->w_ih || !st->w_ho || !st->b_h || !st->b_o) return -1;
    for (h = 0; h < st->hid; ++h) {
        st->b_h[h] = btn->hidden_bias[h];
        for (i = 0; i < st->in; ++i)
            st->w_ih[h * st->in + i] = btn->input_hidden[h * btn->input_count + i];
    }
    for (o = 0; o < st->out; ++o) {
        st->b_o[o] = btn->output_bias[o];
        for (h = 0; h < st->hid; ++h)
            st->w_ho[o * st->hid + h] =
                btn->hidden_output_weights[o * btn->max_hidden_count + h];
    }
    return 0;
}

static int pack_step_f32(FpStep *st) {
    size_t i;
    st->w_ih_f = malloc(st->hid * st->in * sizeof(float));
    st->w_ho_f = malloc(st->out * st->hid * sizeof(float));
    st->b_h_f  = malloc(st->hid * sizeof(float));
    st->b_o_f  = malloc(st->out * sizeof(float));
    if (!st->w_ih_f || !st->w_ho_f || !st->b_h_f || !st->b_o_f) return -1;
    for (i = 0; i < st->hid * st->in; ++i)  st->w_ih_f[i] = (float)st->w_ih[i];
    for (i = 0; i < st->out * st->hid; ++i) st->w_ho_f[i] = (float)st->w_ho[i];
    for (i = 0; i < st->hid; ++i) st->b_h_f[i] = (float)st->b_h[i];
    for (i = 0; i < st->out; ++i) st->b_o_f[i] = (float)st->b_o[i];
    return 0;
}

/* Symmetric per-matrix quantization of an f64 source into int8 codes. Returns
   the scale (max|w| / qmax); writes codes into q. */
static double quantize_matrix(const double *w, size_t n, int bits, int8_t *q) {
    size_t i;
    double amax = 0.0, scale;
    int qmax = (1 << (bits - 1)) - 1;   /* bits=8 -> 127, bits=2 -> 1 */
    if (qmax < 1) qmax = 1;
    for (i = 0; i < n; ++i) { double a = fabs(w[i]); if (a > amax) amax = a; }
    scale = (amax > 0.0) ? amax / (double)qmax : 1.0;
    for (i = 0; i < n; ++i) {
        long c = lround(w[i] / scale);
        if (c > qmax) c = qmax;
        if (c < -qmax) c = -qmax;
        q[i] = (int8_t)c;
    }
    return scale;
}

static int pack_step_int(FpStep *st, int bits) {
    st->w_ih_q = malloc(st->hid * st->in * sizeof(int8_t));
    st->w_ho_q = malloc(st->out * st->hid * sizeof(int8_t));
    if (!st->w_ih_q || !st->w_ho_q) return -1;
    st->w_ih_scale = quantize_matrix(st->w_ih, st->hid * st->in, bits, st->w_ih_q);
    st->w_ho_scale = quantize_matrix(st->w_ho, st->out * st->hid, bits, st->w_ho_q);
    return 0;
}

CompiledRoute *fp_route_compile(const RoutePlan *plan, FastPrecision prec,
                                size_t batch) {
    CompiledRoute *cr;
    size_t s, max_width = 0, max_hidden = 0;

    if (plan == NULL || plan->length == 0 || batch == 0) return NULL;

    cr = calloc(1, sizeof *cr);
    if (cr == NULL) return NULL;
    cr->prec = prec;
    cr->batch = batch;
    cr->length = plan->length;
    cr->min_margin = 0.5;
    cr->steps = calloc(plan->length, sizeof(FpStep));
    if (cr->steps == NULL) { fp_route_free(cr); return NULL; }

    for (s = 0; s < plan->length; ++s) {
        const BinaryTransformNetwork *btn = plan->steps[s];
        FpStep *st = &cr->steps[s];
        if (btn->input_port_count != 1 || btn->output_port_count != 1) {
            fp_route_free(cr); return NULL;   /* route lane is single-port */
        }
        st->in = btn->input_count;
        st->out = btn->output_count;
        st->hid = btn->hidden_count;
        st->in_port = btn->input_ports[0];
        st->out_port = btn->output_ports[0];
        st->int_bits = prec.int_bits;
        if (st->in > max_width) max_width = st->in;
        if (st->out > max_width) max_width = st->out;
        if (st->hid > max_hidden) max_hidden = st->hid;
        if (pack_step_f64(st, btn) != 0) { fp_route_free(cr); return NULL; }
        if (prec.kind == FP_F32 && pack_step_f32(st) != 0) {
            fp_route_free(cr); return NULL;
        }
        if (prec.kind == FP_INT && pack_step_int(st, prec.int_bits) != 0) {
            fp_route_free(cr); return NULL;
        }
    }
    cr->in_total = cr->steps[0].in;
    cr->out_total = cr->steps[cr->length - 1].out;

    cr->cur  = malloc(batch * max_width * sizeof(double));
    cr->nxt  = malloc(batch * max_width * sizeof(double));
    cr->hidv = malloc(batch * max_hidden * sizeof(double));
    if (!cr->cur || !cr->nxt || !cr->hidv) { fp_route_free(cr); return NULL; }
    return cr;
}

/* Forward one step over n samples in f64. Reads samples from `src` (n x st->in,
   row-major), writes raw outputs to `dst` (n x st->out). Per-sample reduction
   order matches btn_forward. */
static void step_forward_f64(const FpStep *st, const double *src, size_t n,
                             double *hidv, double *dst) {
    size_t k, i, h, o;
    for (k = 0; k < n; ++k) {
        const double *in = src + k * st->in;
        double *hv = hidv + k * st->hid;
        double *ov = dst + k * st->out;
        for (h = 0; h < st->hid; ++h) {
            double sum = st->b_h[h];
            const double *row = st->w_ih + h * st->in;
            for (i = 0; i < st->in; ++i) sum += in[i] * row[i];
            hv[h] = fp_sigmoid(sum);
        }
        for (o = 0; o < st->out; ++o) {
            double sum = st->b_o[o];
            const double *row = st->w_ho + o * st->hid;
            for (h = 0; h < st->hid; ++h) sum += hv[h] * row[h];
            ov[o] = fp_sigmoid(sum);
        }
    }
}

static void step_forward_f32(const FpStep *st, const double *src, size_t n,
                             double *hidv, double *dst) {
    size_t k, i, h, o;
    for (k = 0; k < n; ++k) {
        const double *in = src + k * st->in;
        double *hv = hidv + k * st->hid;
        double *ov = dst + k * st->out;
        for (h = 0; h < st->hid; ++h) {
            float sum = st->b_h_f[h];
            const float *row = st->w_ih_f + h * st->in;
            for (i = 0; i < st->in; ++i) sum += (float)in[i] * row[i];
            hv[h] = fp_sigmoid((double)sum);
        }
        for (o = 0; o < st->out; ++o) {
            float sum = st->b_o_f[o];
            const float *row = st->w_ho_f + o * st->hid;
            for (h = 0; h < st->hid; ++h) sum += (float)hv[h] * row[h];
            ov[o] = fp_sigmoid((double)sum);
        }
    }
}

static void step_forward_int(const FpStep *st, const double *src, size_t n,
                             double *hidv, double *dst) {
    size_t k, i, h, o;
    int qmax = (1 << (st->int_bits - 1)) - 1;
    double aq = (qmax >= 1) ? (double)qmax : 1.0;   /* activation quant levels */
    for (k = 0; k < n; ++k) {
        const double *in = src + k * st->in;
        double *hv = hidv + k * st->hid;
        double *ov = dst + k * st->out;
        for (h = 0; h < st->hid; ++h) {
            int64_t acc = 0;
            const int8_t *row = st->w_ih_q + h * st->in;
            for (i = 0; i < st->in; ++i) {
                /* in[i] in [0,1], so (long)(x+0.5) == lround(x) without libm. */
                long qa = (long)(in[i] * aq + 0.5);
                acc += (int64_t)row[i] * qa;
            }
            hv[h] = fp_sigmoid((double)acc * st->w_ih_scale / aq + st->b_h[h]);
        }
        for (o = 0; o < st->out; ++o) {
            int64_t acc = 0;
            const int8_t *row = st->w_ho_q + o * st->hid;
            for (h = 0; h < st->hid; ++h) {
                /* hv[h] in (0,1), so (long)(x+0.5) == lround(x) without libm. */
                long qa = (long)(hv[h] * aq + 0.5);
                acc += (int64_t)row[h] * qa;
            }
            ov[o] = fp_sigmoid((double)acc * st->w_ho_scale / aq + st->b_o[o]);
        }
    }
}

int fp_route_run(CompiledRoute *cr, const double *in, size_t n,
                 double *out, size_t out_cap_total) {
    size_t s, k;
    double *cur, *nxt;

    if (cr == NULL || in == NULL || out == NULL) return -1;
    if (n == 0 || n > cr->batch) return -1;
    if (out_cap_total < n * cr->out_total) return -1;

    cur = cr->cur;
    nxt = cr->nxt;

    /* Load + validate + canonicalize the external input, per sample, against
       the first step's input port (mirrors route_execute's entry snap). */
    for (k = 0; k < n; ++k) {
        const double *src = in + k * cr->in_total;
        double *dstrow = cur + k * cr->steps[0].in;
        if (!port_validate(cr->steps[0].in_port, src)) return -1;
        if (port_canonicalize(cr->steps[0].in_port, src, dstrow) != 0) return -1;
    }

    for (s = 0; s < cr->length; ++s) {
        FpStep *st = &cr->steps[s];
        /* cur holds n x st->in canonical inputs; forward into nxt (raw). */
        switch (cr->prec.kind) {
            case FP_F32: step_forward_f32(st, cur, n, cr->hidv, nxt); break;
            case FP_INT: step_forward_int(st, cur, n, cr->hidv, nxt); break;
            default:     step_forward_f64(st, cur, n, cr->hidv, nxt); break;
        }
        /* Snap each sample's raw output against the output port; fold margin. */
        for (k = 0; k < n; ++k) {
            double *raw = nxt + k * st->out;
            double *dst = cur + k * st->out;   /* next step reads from cur */
            double mm;
            if (port_margin(st->out_port, raw, &mm) == 0 && mm < cr->min_margin)
                cr->min_margin = mm;
            if (port_canonicalize(st->out_port, raw, dst) != 0) return -1;
        }
    }

    for (k = 0; k < n; ++k)
        memcpy(out + k * cr->out_total, cur + k * cr->out_total,
               cr->out_total * sizeof(double));
    return 0;
}

/* ===================== batched DAG lane ===================== */

typedef struct {
    int is_source;
    /* source node */
    int source_index;
    Port src_type;
    /* primitive node (is_source == 0) */
    FpStep step;                          /* packed weights; reuses route kernels */
    size_t child_count;
    size_t child_node[DAG_MAX_SLOTS];     /* compiled index feeding slot s */
    size_t child_seg_off[DAG_MAX_SLOTS];  /* offset of the edge's segment in child full */
    Port   slot_port[DAG_MAX_SLOTS];      /* consuming slot's port */
    size_t slot_total[DAG_MAX_SLOTS];
    Port   out_port[BTN_MAX_OUTPUT_PORTS];/* output segments (snap + margin) */
    size_t out_port_count;
    size_t full_out;                      /* node full output total (per sample) */
    double *nodebuf;                      /* batch * full_out (cached node output) */
} FpDagNode;

struct CompiledDag {
    FastPrecision prec;
    size_t batch;
    FpDagNode *nodes;
    size_t node_count;
    size_t root;
    size_t root_seg_off, root_seg_total;
    size_t n_sources;
    /* scratch reused across nodes/samples */
    double *assembled;   /* batch * max_in  */
    double *hidv;        /* batch * max_hid */
    double *raw;         /* batch * max_out */
    double min_margin;
};

double fp_dag_min_margin(const CompiledDag *cd) { return cd ? cd->min_margin : 0.0; }
void   fp_dag_reset_margin(CompiledDag *cd) { if (cd) cd->min_margin = 0.5; }
size_t fp_dag_out_total(const CompiledDag *cd) { return cd ? cd->root_seg_total : 0; }

/* fp_step_free_buffers is defined above fp_route_free, so it is in scope here. */
void fp_dag_free(CompiledDag *cd) {
    size_t i;
    if (cd == NULL) return;
    if (cd->nodes) {
        for (i = 0; i < cd->node_count; ++i) {
            if (!cd->nodes[i].is_source) fp_step_free_buffers(&cd->nodes[i].step);
            free(cd->nodes[i].nodebuf);
        }
        free(cd->nodes);
    }
    free(cd->assembled); free(cd->hidv); free(cd->raw);
    free(cd);
}

/* Offset of output port `sel` within a btn's flat output (mirrors router.c
   output_segment). */
static size_t fp_output_seg_off(const BinaryTransformNetwork *p, int sel) {
    size_t off = 0; int oj;
    for (oj = 0; oj < sel && (size_t)oj < p->output_port_count; ++oj)
        off += p->output_ports[oj].field_width * p->output_ports[oj].field_count;
    return off;
}

/* Effective output port of children[k] (mirrors router.c edge_port). */
static int fp_edge_port(const DagNode *parent, size_t k) {
    int pp = parent->child_ports[k];
    return pp != 0 ? pp : parent->children[k]->output_index;
}

/* True post-order walk with pointer-dedup: children are compiled (and indexed)
   BEFORE their parent, so every child's index is strictly less than its parent's.
   A forward sweep in fp_dag_run then evaluates children before parents for ANY
   acyclic DAG -- including a node shared by several parents (dedup keys on the
   DagNode*; the first DFS visit completes and registers the node in seen[] before
   any second visit's dedup check fires, since the graph is acyclic). `seen[i]` is
   the DagNode* compiled at index i. Returns the node's index, or (size_t)-1 on
   failure -- fp_dag_free cleans up partial state (calloc-zeroed nodes, NULL-safe
   frees). */
static size_t fp_dag_walk(CompiledDag *cd, const DagNode *node,
                          const DagNode **seen, const Port *source_types,
                          size_t n_sources, size_t *max_in, size_t *max_hid,
                          size_t *max_out) {
    size_t i, idx;
    FpDagNode *fn;

    if (node == NULL) return (size_t)-1;
    for (i = 0; i < cd->node_count; ++i)
        if (seen[i] == node) return i;          /* shared node already compiled */

    if (node->kind == DAG_SOURCE) {
        if (node->source_index < 0 || (size_t)node->source_index >= n_sources)
            return (size_t)-1;
        idx = cd->node_count++;
        seen[idx] = node;
        fn = &cd->nodes[idx];                    /* already calloc-zeroed */
        fn->is_source = 1;
        fn->source_index = node->source_index;
        fn->src_type = source_types[node->source_index];
        fn->full_out = fn->src_type.field_width * fn->src_type.field_count;
    } else {
        const BinaryTransformNetwork *p = node->btn;
        size_t s;
        size_t child_idx[DAG_MAX_SLOTS];
        if (p->input_port_count > DAG_MAX_SLOTS) return (size_t)-1;
        /* Compile children FIRST so they receive lower indices (post-order). */
        for (s = 0; s < node->child_count; ++s) {
            child_idx[s] = fp_dag_walk(cd, node->children[s], seen, source_types,
                                       n_sources, max_in, max_hid, max_out);
            if (child_idx[s] == (size_t)-1) return (size_t)-1;
        }
        idx = cd->node_count++;                   /* index claimed after children */
        seen[idx] = node;
        fn = &cd->nodes[idx];                     /* already calloc-zeroed */
        fn->step.in  = p->input_count;
        fn->step.out = p->output_count;
        fn->step.hid = p->hidden_count;
        if (pack_step_f64(&fn->step, p) != 0) return (size_t)-1;
        if (cd->prec.kind == FP_F32 && pack_step_f32(&fn->step) != 0) return (size_t)-1;
        if (cd->prec.kind == FP_INT &&
            pack_step_int(&fn->step, cd->prec.int_bits) != 0) return (size_t)-1;
        fn->step.int_bits = cd->prec.int_bits;
        fn->child_count = node->child_count;
        fn->out_port_count = p->output_port_count;
        for (s = 0; s < p->output_port_count; ++s) fn->out_port[s] = p->output_ports[s];
        fn->full_out = p->output_count;
        if (p->input_count  > *max_in)  *max_in  = p->input_count;
        if (p->hidden_count > *max_hid) *max_hid = p->hidden_count;
        if (p->output_count > *max_out) *max_out = p->output_count;
        for (s = 0; s < node->child_count; ++s) {
            const DagNode *child = node->children[s];
            fn->child_node[s] = child_idx[s];
            fn->slot_port[s] = p->input_ports[s];
            fn->slot_total[s] = p->input_ports[s].field_width *
                                p->input_ports[s].field_count;
            fn->child_seg_off[s] = (child->kind == DAG_PRIMITIVE)
                ? fp_output_seg_off(child->btn, fp_edge_port(node, s)) : 0;
        }
    }

    /* Per-node output buffer, sized now that full_out is known. */
    fn->nodebuf = malloc(cd->batch * (fn->full_out ? fn->full_out : 1) * sizeof(double));
    if (fn->nodebuf == NULL) return (size_t)-1;
    return idx;
}

/* Pre-count unique nodes (pointer dedup) to size the array; -1 if > 64. */
static int fp_dag_count(const DagNode *node, const DagNode **seen, size_t *cnt) {
    size_t i;
    if (node == NULL) return -1;
    for (i = 0; i < *cnt; ++i) if (seen[i] == node) return 0;
    if (*cnt >= 64) return -1;
    seen[(*cnt)++] = node;
    if (node->kind == DAG_PRIMITIVE) {
        size_t s;
        for (s = 0; s < node->child_count; ++s)
            if (fp_dag_count(node->children[s], seen, cnt) != 0) return -1;
    }
    return 0;
}

CompiledDag *fp_dag_compile(const DagPlan *plan, const Port *source_types,
                            size_t n_sources, FastPrecision prec, size_t batch) {
    CompiledDag *cd;
    const DagNode *count_seen[64];
    const DagNode *seen[64];
    size_t total = 0, max_in = 0, max_hid = 0, max_out = 0;
    const DagNode *r;

    if (plan == NULL || plan->root == NULL || source_types == NULL || batch == 0)
        return NULL;
    if (fp_dag_count(plan->root, count_seen, &total) != 0 || total == 0)
        return NULL;

    cd = calloc(1, sizeof *cd);
    if (cd == NULL) return NULL;
    cd->prec = prec; cd->batch = batch; cd->n_sources = n_sources;
    cd->min_margin = 0.5;
    cd->nodes = calloc(total, sizeof(FpDagNode));   /* fixed array, never realloc'd */
    if (cd->nodes == NULL) { free(cd); return NULL; }

    /* One pass: build every node and allocate its buffer; fp_dag_free cleans up
       any partial state on failure (calloc-zeroed nodes, NULL-safe frees). */
    cd->root = fp_dag_walk(cd, plan->root, seen, source_types, n_sources,
                           &max_in, &max_hid, &max_out);
    if (cd->root == (size_t)-1) { fp_dag_free(cd); return NULL; }

    r = plan->root;
    if (r->kind == DAG_SOURCE) {
        cd->root_seg_off = 0;
        cd->root_seg_total = cd->nodes[cd->root].full_out;
    } else {
        cd->root_seg_off = fp_output_seg_off(r->btn, r->output_index);
        cd->root_seg_total = r->btn->output_ports[r->output_index].field_width *
                             r->btn->output_ports[r->output_index].field_count;
    }

    cd->assembled = malloc(batch * (max_in  ? max_in  : 1) * sizeof(double));
    cd->hidv      = malloc(batch * (max_hid ? max_hid : 1) * sizeof(double));
    cd->raw       = malloc(batch * (max_out ? max_out : 1) * sizeof(double));
    if (!cd->assembled || !cd->hidv || !cd->raw) { fp_dag_free(cd); return NULL; }
    return cd;
}

/* Evaluate every node once over n samples (post-order forward sweep): sources
   validate+canonicalize their batch; primitives assemble each slot from its
   child's projected segment, forward (kind-dispatched), and snap every output
   segment, folding the worst margin into cd->min_margin. No root projection.
   Assumes arguments already validated by the caller. Returns 0, or -1 on a
   bad/ambiguous handoff. */
static int fp_dag_eval(CompiledDag *cd, const double *const *src_batches, size_t n) {
    size_t ni, k;
    for (ni = 0; ni < cd->node_count; ++ni) {
        FpDagNode *fn = &cd->nodes[ni];
        if (fn->is_source) {
            const double *sb = src_batches[fn->source_index];
            size_t tot = fn->full_out;
            for (k = 0; k < n; ++k) {
                const double *v = sb + k * tot;
                double *dst = fn->nodebuf + k * tot;
                if (!port_validate(fn->src_type, v) ||
                    port_canonicalize(fn->src_type, v, dst) != 0) return -1;
            }
            continue;
        }
        /* primitive: assemble -> forward -> snap */
        for (k = 0; k < n; ++k) {
            size_t s, off = 0;
            double *asm_row = cd->assembled + k * fn->step.in;
            for (s = 0; s < fn->child_count; ++s) {
                const FpDagNode *ch = &cd->nodes[fn->child_node[s]];
                const double *seg = ch->nodebuf + k * ch->full_out + fn->child_seg_off[s];
                if (!port_validate(fn->slot_port[s], seg) ||
                    port_canonicalize(fn->slot_port[s], seg, asm_row + off) != 0)
                    return -1;
                off += fn->slot_total[s];
            }
        }
        switch (cd->prec.kind) {
            case FP_F32: step_forward_f32(&fn->step, cd->assembled, n, cd->hidv, cd->raw); break;
            case FP_INT: step_forward_int(&fn->step, cd->assembled, n, cd->hidv, cd->raw); break;
            default:     step_forward_f64(&fn->step, cd->assembled, n, cd->hidv, cd->raw); break;
        }
        for (k = 0; k < n; ++k) {
            size_t oj, off = 0;
            double *raw_row = cd->raw + k * fn->step.out;
            double *dst = fn->nodebuf + k * fn->full_out;
            for (oj = 0; oj < fn->out_port_count; ++oj) {
                double mm;
                size_t tot = fn->out_port[oj].field_width * fn->out_port[oj].field_count;
                if (port_margin(fn->out_port[oj], raw_row + off, &mm) == 0 &&
                    mm < cd->min_margin) cd->min_margin = mm;
                if (port_canonicalize(fn->out_port[oj], raw_row + off, dst + off) != 0)
                    return -1;
                off += tot;
            }
        }
    }
    return 0;
}

int fp_dag_run(CompiledDag *cd, const double *const *src_batches, size_t n,
               double *out, size_t out_cap_total) {
    size_t k;
    if (cd == NULL || src_batches == NULL || out == NULL) return -1;
    if (n == 0 || n > cd->batch) return -1;
    if (out_cap_total < n * cd->root_seg_total) return -1;

    if (fp_dag_eval(cd, src_batches, n) != 0) return -1;

    {
        const FpDagNode *r = &cd->nodes[cd->root];
        for (k = 0; k < n; ++k)
            memcpy(out + k * cd->root_seg_total,
                   r->nodebuf + k * r->full_out + cd->root_seg_off,
                   cd->root_seg_total * sizeof(double));
    }
    return 0;
}

/* ===================== batched circuit lane ===================== */

struct CompiledCircuit {
    CompiledDag *g;                       /* the shared node graph (reuses DAG) */
    size_t roots[CIRCUIT_MAX_ROOTS];      /* compiled-node index per root */
    size_t root_off[CIRCUIT_MAX_ROOTS];   /* projected-segment offset per root */
    size_t root_total[CIRCUIT_MAX_ROOTS]; /* projected-segment width per root */
    size_t root_count;
    size_t out_total;                     /* sum of root_total */
};

double fp_circuit_min_margin(const CompiledCircuit *cc) {
    return cc ? cc->g->min_margin : 0.0;
}
void fp_circuit_reset_margin(CompiledCircuit *cc) {
    if (cc) cc->g->min_margin = 0.5;
}
size_t fp_circuit_out_total(const CompiledCircuit *cc) { return cc ? cc->out_total : 0; }
size_t fp_circuit_node_count(const CompiledCircuit *cc) {
    return cc ? cc->g->node_count : 0;
}

void fp_circuit_free(CompiledCircuit *cc) {
    if (cc == NULL) return;
    fp_dag_free(cc->g);   /* frees the graph's nodes + scratch + the CompiledDag */
    free(cc);
}

CompiledCircuit *fp_circuit_compile(const CircuitPlan *plan, const Port *source_types,
                                    size_t n_sources, FastPrecision prec, size_t batch) {
    CompiledCircuit *cc;
    CompiledDag *g;
    const DagNode *count_seen[64];
    const DagNode *seen[64];
    size_t total = 0, max_in = 0, max_hid = 0, max_out = 0, gi;

    if (plan == NULL || plan->root_count == 0 || source_types == NULL || batch == 0)
        return NULL;
    if (plan->root_count > CIRCUIT_MAX_ROOTS) return NULL;
    /* Count the UNION of nodes over all roots (shared dedup) to size the graph. */
    for (gi = 0; gi < plan->root_count; ++gi)
        if (plan->roots[gi] == NULL ||
            fp_dag_count(plan->roots[gi], count_seen, &total) != 0) return NULL;
    if (total == 0) return NULL;

    g = calloc(1, sizeof *g);
    if (g == NULL) return NULL;
    g->prec = prec; g->batch = batch; g->min_margin = 0.5;
    g->nodes = calloc(total, sizeof(FpDagNode));
    if (g->nodes == NULL) { free(g); return NULL; }

    cc = calloc(1, sizeof *cc);
    if (cc == NULL) { fp_dag_free(g); return NULL; }
    cc->g = g;
    cc->root_count = plan->root_count;

    /* Walk every root into the SHARED graph: a node read by several roots is
       compiled once (shared seen[]), so it is later evaluated once too. */
    for (gi = 0; gi < plan->root_count; ++gi) {
        const DagNode *r = plan->roots[gi];
        size_t idx = fp_dag_walk(g, r, seen, source_types, n_sources,
                                 &max_in, &max_hid, &max_out);
        if (idx == (size_t)-1) { fp_dag_free(g); free(cc); return NULL; }
        cc->roots[gi] = idx;
        if (r->kind == DAG_SOURCE) {
            cc->root_off[gi] = 0;
            cc->root_total[gi] = g->nodes[idx].full_out;
        } else {
            cc->root_off[gi] = fp_output_seg_off(r->btn, plan->root_ports[gi]);
            cc->root_total[gi] =
                r->btn->output_ports[plan->root_ports[gi]].field_width *
                r->btn->output_ports[plan->root_ports[gi]].field_count;
        }
        cc->out_total += cc->root_total[gi];
    }

    g->assembled = malloc(batch * (max_in  ? max_in  : 1) * sizeof(double));
    g->hidv      = malloc(batch * (max_hid ? max_hid : 1) * sizeof(double));
    g->raw       = malloc(batch * (max_out ? max_out : 1) * sizeof(double));
    if (!g->assembled || !g->hidv || !g->raw) { fp_dag_free(g); free(cc); return NULL; }
    return cc;
}

int fp_circuit_run(CompiledCircuit *cc, const double *const *src_batches, size_t n,
                   double *out, size_t out_cap_total) {
    size_t gi, k;
    if (cc == NULL || src_batches == NULL || out == NULL) return -1;
    if (n == 0 || n > cc->g->batch) return -1;
    if (out_cap_total < n * cc->out_total) return -1;

    if (fp_dag_eval(cc->g, src_batches, n) != 0) return -1;

    /* Project each root's segment, concatenated in goal order, per sample. */
    for (k = 0; k < n; ++k) {
        size_t concat = 0;
        for (gi = 0; gi < cc->root_count; ++gi) {
            const FpDagNode *r = &cc->g->nodes[cc->roots[gi]];
            memcpy(out + k * cc->out_total + concat,
                   r->nodebuf + k * r->full_out + cc->root_off[gi],
                   cc->root_total[gi] * sizeof(double));
            concat += cc->root_total[gi];
        }
    }
    return 0;
}
