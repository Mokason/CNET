#ifndef PLAN_TABLE_H
#define PLAN_TABLE_H

#include <stddef.h>

#include "nn.h"
#include "router.h"

/* Shared machinery for treating a proven plan as a STRICT teacher over its
   enumerated canonical input domain. Used by consolidation (distillation)
   and by contract emission. */

size_t plan_port_total(Port p);

/* The enumerated canonical domain of a port list, flattened to per-field
   units so the cartesian product is one loop. Later fields advance
   fastest. Build refuses RAW fields and size_t overflow. */
typedef struct {
    Port *field_port;     /* owning port, one entry per field */
    size_t *field_offset; /* start of the field within the input vector */
    size_t *cardinality;
    size_t n_fields;
    size_t combos;        /* product of cardinalities */
    size_t in_total;
} PlanDomain;

/* Build the enumeration of a port list. Returns 0 and fills *d (caller
   releases with plan_domain_free), or -1 when any field is not
   enumerable (RAW) or the cardinality product overflows. */
int  plan_domain_build(const Port *ports, size_t n_ports, PlanDomain *d);

/* Write the sample'th canonical member (sample in [0, combos)) into vec,
   which must hold in_total values. Later fields advance fastest. */
void plan_domain_write(const PlanDomain *d, size_t sample, double *vec);

void plan_domain_free(PlanDomain *d);

/* Labels one enumerated input. Returns 0 (label written) or -1 (abort). */
typedef int (*PlanTeacherFn)(void *ctx, const double *in, size_t in_total,
                             double *out, size_t out_total);

/* ctx = a RoutePlan whose strict flag is already set. */
int plan_route_teacher(void *ctx, const double *in, size_t in_total,
                       double *out, size_t out_total);

typedef struct {
    DagPlan teacher;          /* borrowed root, strict = 1 */
    const DagSource *declared;
    size_t n_sources;
    const size_t *offsets;    /* slice of the flat input vector per source */
} PlanDagTeacherCtx;

int plan_dag_teacher(void *ctx, const double *in, size_t in_total,
                     double *out, size_t out_total);

/* ctx for a multi-root circuit teacher (borrowed roots, strict = 1). */
typedef struct {
    CircuitPlan teacher;
    const DagSource *declared;
    size_t n_sources;
    const size_t *offsets;
} PlanCircuitTeacherCtx;

int plan_circuit_teacher(void *ctx, const double *in, size_t in_total,
                         double *out, size_t out_total);

/* Sharing-aware walkers: a node referenced by several consumers is visited
   ONCE (pointer set), so shared executions are not double-counted. */

/* Collect every DISTINCT primitive execution across the roots. Returns the
   count, writing up to cap entries. */
size_t plan_circuit_collect_members(const CircuitPlan *p,
                                    BinaryTransformNetwork **members,
                                    size_t cap);

/* Count how many DISTINCT leaf nodes reference each source index. Returns
   0, or -1 on an index outside [0, n_sources). */
int plan_circuit_count_sources(const CircuitPlan *p, size_t *counts,
                               size_t n_sources);

/* Collect every DAG_PRIMITIVE in the tree (duplicates included). Returns the
   count, writing up to cap entries. */
size_t plan_dag_collect_members(const DagNode *node,
                                BinaryTransformNetwork **members,
                                size_t cap, size_t n);

/* Count how often each source index feeds the tree. Returns 0, or -1 on an
   index outside [0, n_sources). */
int plan_dag_count_sources(const DagNode *node, size_t *counts,
                           size_t n_sources);

/* The teacher-labeled table over a plan's full canonical input domain. */
typedef struct {
    double *inputs;    /* kept x in_total */
    double *targets;   /* kept x out_total */
    size_t kept;       /* inputs the strict teacher labeled */
    size_t aborts;     /* inputs the strict teacher refused (excluded) */
    size_t in_total;
    size_t out_total;
} PlanTable;

/* Enumerate the canonical domain of in_ports (deterministic order: ports
   left to right, later fields fastest), refuse RAW fields, overflow, or
   more than max_samples combos; label every input with the strict teacher;
   snapshot and restore the distinct members' reliability counters around
   the sweep (a teacher sweep is not deployment experience). Returns 0 with
   *out filled (kept may be 0 -- caller decides), or -1 on refusal or
   allocation failure (*out zeroed). */
int plan_table_build(
    const Port *in_ports, size_t n_in,
    size_t out_total,
    PlanTeacherFn teacher, void *ctx,
    BinaryTransformNetwork *const *members, size_t n_members,
    size_t max_samples,
    PlanTable *out);

void plan_table_free(PlanTable *t);

#endif
