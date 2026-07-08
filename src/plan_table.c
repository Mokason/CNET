/*
 * Plan table: shared machinery for treating a proven route/DAG plan as a
 * STRICT teacher over its enumerated canonical input domain. The plan is
 * never trained or mutated -- the domain is enumerated (canonical members
 * only), each input is labeled by executing the plan, and the labeled table
 * is handed back. Used by consolidation (distillation) and by contract
 * emission. Member reliability counters are snapshotted before a teacher
 * sweep and restored after it: a teacher sweep is not deployment experience.
 */
#include "../include/plan_table.h"

#include "../include/arena.h"

#include <stdlib.h>
#include <string.h>

static int size_t_mul_overflow(size_t a, size_t b, size_t *out) {
    if (a != 0 && b > (size_t)-1 / a) {
        return -1;
    }
    *out = a * b;
    return 0;
}

size_t plan_port_total(Port p) {
    return p.field_width * p.field_count;
}

/* Canonical members of ONE field; 0 = not enumerable (RAW, or a binary
   field too wide to count in a size_t). */
static size_t field_cardinality(Port p) {
    switch (p.family) {
    case PORT_ONEHOT:
        return p.field_width;
    case PORT_BINARY_MSB:
    case PORT_BINARY_LSB:
        if (p.field_width >= sizeof(size_t) * 8 - 1) {
            return 0;
        }
        return (size_t)1 << p.field_width;
    default:
        return 0;
    }
}

/* Write one field's value'th canonical member at dst. */
static void field_write(Port p, size_t value, double *dst) {
    size_t w = p.field_width;
    size_t j;

    if (p.family == PORT_ONEHOT) {
        for (j = 0; j < w; ++j) {
            dst[j] = 0.0;
        }
        dst[value] = 1.0;
        return;
    }
    for (j = 0; j < w; ++j) {
        size_t bit = (p.family == PORT_BINARY_MSB) ? (w - 1 - j) : j;
        dst[j] = (double)((value >> bit) & 1u);
    }
}

void plan_domain_free(PlanDomain *d) {
    free(d->field_port);
    free(d->field_offset);
    free(d->cardinality);
    memset(d, 0, sizeof *d);
}

/* Returns 0 and fills *d, or -1 when any field is not enumerable or the
   product overflows. */
int plan_domain_build(const Port *ports, size_t n_ports, PlanDomain *d) {
    size_t n_fields = 0;
    size_t i, f, k;
    size_t offset = 0;
    size_t field_port_bytes = 0;
    size_t field_offset_bytes = 0;
    size_t cardinality_bytes = 0;

    memset(d, 0, sizeof *d);
    for (i = 0; i < n_ports; ++i) {
        if (n_fields > (size_t)-1 - ports[i].field_count) {
            return -1;
        }
        n_fields += ports[i].field_count;
    }
    if (n_fields == 0) {
        return -1;
    }
    if (size_t_mul_overflow(n_fields, sizeof *d->field_port, &field_port_bytes) != 0 ||
        size_t_mul_overflow(n_fields, sizeof *d->field_offset, &field_offset_bytes) != 0 ||
        size_t_mul_overflow(n_fields, sizeof *d->cardinality, &cardinality_bytes) != 0) {
        return -1;
    }
    d->field_port = malloc(field_port_bytes);
    d->field_offset = malloc(field_offset_bytes);
    d->cardinality = malloc(cardinality_bytes);
    if (d->field_port == NULL || d->field_offset == NULL ||
        d->cardinality == NULL) {
        plan_domain_free(d);
        return -1;
    }

    d->n_fields = n_fields;
    d->combos = 1;
    k = 0;
    for (i = 0; i < n_ports; ++i) {
        size_t card = field_cardinality(ports[i]);
        if (card == 0) {
            plan_domain_free(d);
            return -1;
        }
        for (f = 0; f < ports[i].field_count; ++f) {
            if (offset > (size_t)-1 - ports[i].field_width) {
                plan_domain_free(d);
                return -1;
            }
            d->field_port[k] = ports[i];
            d->field_offset[k] = offset;
            d->cardinality[k] = card;
            offset += ports[i].field_width;
            if (d->combos > (size_t)-1 / card) {
                plan_domain_free(d);
                return -1;
            }
            d->combos *= card;
            ++k;
        }
    }
    d->in_total = offset;
    return 0;
}

/* Write the sample'th member of the domain into vec (later fields fastest). */
void plan_domain_write(const PlanDomain *d, size_t sample, double *vec) {
    size_t f = d->n_fields;

    while (f-- > 0) {
        size_t v = sample % d->cardinality[f];
        sample /= d->cardinality[f];
        field_write(d->field_port[f], v, vec + d->field_offset[f]);
    }
}

/* ---- member evidence hygiene -------------------------------------------
   Distillation sweeps are not deployment experience: snapshot every distinct
   member's counters before the teacher pass and restore them after it. */

typedef struct {
    BinaryTransformNetwork *btn;
    unsigned long successes;
    unsigned long failures;
} StatSnap;

static size_t snap_members(BinaryTransformNetwork *const *members,
                           size_t n_members, StatSnap *snaps) {
    size_t n = 0;
    size_t i, j;

    for (i = 0; i < n_members; ++i) {
        int seen = 0;
        for (j = 0; j < n; ++j) {
            if (snaps[j].btn == members[i]) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            snaps[n].btn = members[i];
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
            snaps[n].successes = atomic_load_explicit(&members[i]->output_successes, memory_order_relaxed);
            snaps[n].failures = atomic_load_explicit(&members[i]->output_failures, memory_order_relaxed);
#else
            snaps[n].successes = members[i]->output_successes;
            snaps[n].failures = members[i]->output_failures;
#endif
            ++n;
        }
    }
    return n;
}

static void restore_members(const StatSnap *snaps, size_t n) {
    size_t i;

    for (i = 0; i < n; ++i) {
#if defined(_WIN32) || defined(__WIN32__) || defined(__MINGW32__)
        atomic_store_explicit(&snaps[i].btn->output_successes, snaps[i].successes, memory_order_relaxed);
        atomic_store_explicit(&snaps[i].btn->output_failures, snaps[i].failures, memory_order_relaxed);
#else
        snaps[i].btn->output_successes = snaps[i].successes;
        snaps[i].btn->output_failures = snaps[i].failures;
#endif
    }
}

/* ---- route plans -------------------------------------------------------- */

int plan_route_teacher(void *ctx, const double *in, size_t in_total,
                       double *out, size_t out_total) {
    const RoutePlan *plan = ctx;

    return route_execute(plan, in, in_total, out, out_total);
}

/* ---- DAG plans ----------------------------------------------------------- */

int plan_dag_teacher(void *ctx, const double *in, size_t in_total,
                     double *out, size_t out_total) {
    PlanDagTeacherCtx *c = ctx;
    DagSource live[DAG_MAX_SLOTS];
    size_t i;

    (void)in_total;
    for (i = 0; i < c->n_sources; ++i) {
        live[i].type = c->declared[i].type;
        live[i].values = in + c->offsets[i];
    }
    return dag_execute(&c->teacher, live, c->n_sources, out, out_total);
}

/* Count how often each source index feeds the tree. Returns 0, or -1 on an
   index outside [0, n_sources). */
int plan_dag_count_sources(const DagNode *node, size_t *counts,
                           size_t n_sources) {
    size_t i;

    if (node == NULL) {
        return 0;
    }
    if (node->kind == DAG_SOURCE) {
        if (node->source_index < 0 ||
            (size_t)node->source_index >= n_sources) {
            return -1;
        }
        ++counts[node->source_index];
        return 0;
    }
    for (i = 0; i < node->child_count; ++i) {
        if (plan_dag_count_sources(node->children[i], counts, n_sources) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Collect every DAG_PRIMITIVE in the tree (duplicates included; the snapshot
   dedupes). Returns the count, writing up to cap entries. */
size_t plan_dag_collect_members(const DagNode *node,
                                BinaryTransformNetwork **members,
                                size_t cap, size_t n) {
    size_t i;

    if (node == NULL) {
        return n;
    }
    if (node->kind == DAG_PRIMITIVE) {
        if (n < cap) {
            members[n] = (BinaryTransformNetwork *)node->btn;
        }
        ++n;
        for (i = 0; i < node->child_count; ++i) {
            n = plan_dag_collect_members(node->children[i], members, cap, n);
        }
    }
    return n;
}

/* ---- circuit plans ------------------------------------------------------- */

int plan_circuit_teacher(void *ctx, const double *in, size_t in_total,
                         double *out, size_t out_total) {
    PlanCircuitTeacherCtx *c = ctx;
    DagSource live[DAG_MAX_SLOTS];
    size_t i;

    (void)in_total;
    for (i = 0; i < c->n_sources; ++i) {
        live[i].type = c->declared[i].type;
        live[i].values = in + c->offsets[i];
    }
    return dag_execute_circuit(&c->teacher, live, c->n_sources, out,
                               out_total, NULL);
}

/* Pointer set for sharing-aware traversal: each node visits once. */
typedef struct {
    const DagNode **items;
    size_t count;
    size_t cap;
} NodeSet;

/* Returns 1 when newly added, 0 when already present, -1 on OOM. */
static int node_set_add(NodeSet *set, const DagNode *node, Arena *arena) {
    size_t i;
    size_t ncap;
    size_t bytes;
    const DagNode **grown;

    for (i = 0; i < set->count; ++i) {
        if (set->items[i] == node) {
            return 0;
        }
    }
    if (set->count == set->cap) {
        ncap = set->cap == 0 ? 16 : set->cap * 2;
        if (size_t_mul_overflow(ncap, sizeof(*grown), &bytes) != 0) {
            return -1;
        }
        grown = arena_alloc(arena, bytes);
        if (grown == NULL) {
            return -1;
        }
        if (set->items != NULL && set->count > 0) {
            memcpy((void *)grown, set->items, set->count * sizeof(*set->items));
        }

        set->items = grown;
        set->cap = ncap;
    }
    set->items[set->count++] = node;
    return 1;
}

/* Visit each distinct node once: collect members and/or count source
   references. Returns 0, or -1 on a bad source index or OOM. */
static int circuit_visit(const DagNode *node, NodeSet *seen,
                         BinaryTransformNetwork **members, size_t cap,
                         size_t *n_members, Arena *arena,
                         size_t *counts, size_t n_sources) {
    size_t i;
    int added;

    if (node == NULL) {
        return -1;
    }
    added = node_set_add(seen, node, arena);
    if (added < 0) {
        return -1;
    }
    if (added == 0) {
        return 0; /* shared node: already visited */
    }
    if (node->kind == DAG_SOURCE) {
        if (node->source_index < 0 ||
            (size_t)node->source_index >= n_sources) {
            return -1;
        }
        if (counts != NULL) {
            ++counts[node->source_index];
        }
        return 0;
    }
    if (n_members != NULL) {
        if (members != NULL && *n_members < cap) {
            members[*n_members] = (BinaryTransformNetwork *)node->btn;
        }
        ++*n_members;
    }
    for (i = 0; i < node->child_count; ++i) {
        if (circuit_visit(node->children[i], seen, members, cap, n_members,
                          arena,
                          counts, n_sources) != 0) {
            return -1;
        }
    }
    return 0;
}

size_t plan_circuit_collect_members(const CircuitPlan *p,
                                    BinaryTransformNetwork **members,
                                    size_t cap) {
    NodeSet seen = {NULL, 0, 0};
    Arena arena;
    size_t n = 0;
    size_t g;
    int rc = 0;

    arena_init(&arena);
    for (g = 0; rc == 0 && g < p->root_count; ++g) {
        rc = circuit_visit(p->roots[g], &seen, members, cap, &n, &arena,
                           NULL, (size_t)-1);
    }
    arena_reset(&arena);
    return rc == 0 ? n : 0;
}

int plan_circuit_count_sources(const CircuitPlan *p, size_t *counts,
                               size_t n_sources) {
    NodeSet seen = {NULL, 0, 0};
    Arena arena;
    size_t g;
    int rc = 0;

    arena_init(&arena);
    for (g = 0; rc == 0 && g < p->root_count; ++g) {
        rc = circuit_visit(p->roots[g], &seen, NULL, 0, NULL, &arena,
                           counts, n_sources);
    }
    arena_reset(&arena);
    return rc;
}

/* ---- teacher-labeled table over a plan's full canonical input domain ---- */

int plan_table_build(
    const Port *in_ports, size_t n_in,
    size_t out_total,
    PlanTeacherFn teacher, void *ctx,
    BinaryTransformNetwork *const *members, size_t n_members,
    size_t max_samples,
    PlanTable *out
) {
    PlanDomain dom;
    Arena arena;
    StatSnap *snaps = NULL;
    size_t n_snaps = 0;
    size_t s;
    size_t input_cells;
    size_t target_cells;
    size_t in_bytes;
    size_t out_bytes;
    size_t snaps_bytes;

    if (out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof *out);
    if (in_ports == NULL || n_in == 0 || out_total == 0 || teacher == NULL) {
        return -1;
    }
    if (plan_domain_build(in_ports, n_in, &dom) != 0) {
        return -1;
    }
    if (dom.combos > max_samples) {
        plan_domain_free(&dom);
        return -1;
    }

    if (size_t_mul_overflow(n_members > 0 ? n_members : 1, sizeof *snaps, &snaps_bytes) != 0 ||
        size_t_mul_overflow(dom.combos, dom.in_total, &input_cells) != 0 ||
        size_t_mul_overflow(dom.combos, out_total, &target_cells) != 0 ||
        size_t_mul_overflow(input_cells, sizeof *out->inputs, &in_bytes) != 0 ||
        size_t_mul_overflow(target_cells, sizeof *out->targets, &out_bytes) != 0) {
        plan_domain_free(&dom);
        return -1;
    }

    arena_init(&arena);
    snaps = arena_alloc(&arena, snaps_bytes);
    out->inputs = malloc(in_bytes);
    out->targets = malloc(out_bytes);
    if (snaps == NULL || out->inputs == NULL || out->targets == NULL) {
        plan_table_free(out);
        plan_domain_free(&dom);
        arena_reset(&arena);
        return -1;
    }

    n_snaps = snap_members(members, n_members, snaps);
    for (s = 0; s < dom.combos; ++s) {
        double *in_vec = out->inputs + out->kept * dom.in_total;
        double *out_vec = out->targets + out->kept * out_total;

        plan_domain_write(&dom, s, in_vec);
        if (teacher(ctx, in_vec, dom.in_total, out_vec, out_total) == 0) {
            ++out->kept;
        } else {
            ++out->aborts;
        }
    }
    restore_members(snaps, n_snaps);

    out->in_total = dom.in_total;
    out->out_total = out_total;
    arena_reset(&arena);
    plan_domain_free(&dom);
    return 0;
}

void plan_table_free(PlanTable *t) {
    if (t == NULL) {
        return;
    }
    free(t->inputs);
    free(t->targets);
    memset(t, 0, sizeof *t);
}
