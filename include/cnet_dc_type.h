#ifndef CNET_DC_TYPE_H
#define CNET_DC_TYPE_H

#include <stddef.h>

#include "nn.h"
#include "router.h"
#include "contract/contract.h"

#ifdef __cplusplus
extern "C" {
#endif

/* DreamCoder Core type logic (Ellis et al., type.py / type.ml):
   TypeConstructor | TypeVariable, Context, instantiate, unify, apply,
   occurs check. Native C. Not residual speech. Not an ASI-5 unit. */

#define CNET_DC_MAX_NODES 8192
#define CNET_DC_MAX_ARGS 4
#define CNET_DC_NAME_MAX 32
#define CNET_DC_MAX_SUBST 96
#define CNET_DC_MAX_BINDS 64
#define CNET_DC_ARROW_NAME "->"

typedef enum {
    CNET_DC_VAR = 0,
    CNET_DC_CTOR = 1
} CnetDcKind;

typedef struct {
    CnetDcKind kind;
    int var;
    char name[CNET_DC_NAME_MAX];
    int n_args;
    int args[CNET_DC_MAX_ARGS];
} CnetDcNode;

typedef struct {
    CnetDcNode nodes[CNET_DC_MAX_NODES];
    int count;
} CnetDcArena;

typedef struct {
    int next_var;
    int subst_var[CNET_DC_MAX_SUBST];
    int subst_type[CNET_DC_MAX_SUBST];
    int subst_count;
} CnetDcContext;

void cnet_dc_arena_init(CnetDcArena *arena);
void cnet_dc_ctx_init(CnetDcContext *ctx);

/* Allocate. Return node index, or -1 on overflow / bad args. */
int cnet_dc_var(CnetDcArena *arena, int index);
int cnet_dc_ctor(CnetDcArena *arena, const char *name, const int *args,
                 int n_args);
int cnet_dc_base(CnetDcArena *arena, const char *name);
int cnet_dc_arrow(CnetDcArena *arena, int domain, int range);
int cnet_dc_list(CnetDcArena *arena, int elem);
int cnet_dc_pair(CnetDcArena *arena, int left, int right);

int cnet_dc_is_arrow(const CnetDcArena *arena, int type);
int cnet_dc_equal(const CnetDcArena *arena, int a, int b);
int cnet_dc_show(const CnetDcArena *arena, int type, char *out, size_t cap);

/* instantiate / apply / unify: 0 ok, 1 failure, <0 execution error. */
int cnet_dc_instantiate(CnetDcArena *arena, CnetDcContext *ctx, int type,
                        int *out);
int cnet_dc_apply(CnetDcArena *arena, const CnetDcContext *ctx, int type,
                  int *out);
int cnet_dc_unify(CnetDcArena *arena, CnetDcContext *ctx, int t1, int t2);
int cnet_dc_can_unify(CnetDcArena *arena, int t1, int t2);

/* Apply fn : a -> b to arg : a, write b. 0 ok, 1 ill-typed, <0 error. */
int cnet_dc_apply_fn(CnetDcArena *arena, int fn, int arg, int *result);

/* Map a Core Port onto a DreamCoder type. */
int cnet_dc_from_port(CnetDcArena *arena, Port port);

/* Arrow of input ports to a single output or pair of outputs. */
int cnet_dc_from_contract(CnetDcArena *arena, const Contract *contract);

/* Same meaning as port_compatible, via DC types + RAW/tag wildcard rules. */
int cnet_dc_ports_unify(Port producer, Port consumer);

/* Fail-closed type check of a Core route plan. Linear routes are unary:
   a step with extra unsatisfied inputs is ill-typed. 0 well-typed, 1 not. */
int cnet_dc_route_well_typed(const RoutePlan *plan, Port source);

/* Fail-closed type check of every DAG edge (all input slots, not port 0). */
int cnet_dc_dag_well_typed(const DagPlan *plan, const DagSource *sources,
                           size_t n_sources, Port goal);

/* Fail-closed type check of every circuit root and its used ports. */
int cnet_dc_circuit_well_typed(const CircuitPlan *plan,
                               const DagSource *sources, size_t n_sources,
                               const Port *goals, size_t n_goals);

#ifdef __cplusplus
}
#endif

#endif
