#ifndef ROUTER_INTERNAL_H
#define ROUTER_INTERNAL_H

/* router_internal.h
 *
 * Private implementation details shared across the split router_*.c files.
 * NOT part of the public API. Do not include from tests or other modules
 * unless they are implementation peers.
 *
 * This enables clean SRP split of the former monolithic src/router.c (~5700 LOC)
 * into focused modules while preserving exact original behavior.
 */

#include "../router.h"
#include <stdint.h>

/* === Shared mutable study / pruning state (was file-scope static) === */

/* v1.0 circuit pair pruning support (active only during a top-level circuit PRUNE call) */
extern size_t g_circuit_prune_allowed_prim[128];
extern int    g_circuit_prune_allowed_oj[128];
extern size_t g_circuit_prune_num_allowed;
extern int    g_circuit_prune_active;

/* v2.3: real counter for search expansions. Used only for measurement in studies. */
extern size_t *g_nodes_expanded_counter;

/* Forward decl for plan cache ownership (defined in router_dag_plan.c) */
struct PlanCacheEntry;
typedef struct PlanCacheEntry PlanCacheEntry;

/* Internal helper that had duplicate definitions */
int router_same_port_type(Port a, Port b);

/* Cross-module visible from registry */
int entry_usable(const PrimitiveRegistry *reg, size_t i);

/* === DAG internal structures (moved from monolithic router.c) ===
   These are not part of the public router.h API.
*/
typedef struct {
    const DagNode *node;
    unsigned ports_used;   /* bitmask of output ports already used by consumers in the built plan */
} BuiltEntry;

typedef struct {
    BuiltEntry *items;
    size_t count;
    size_t cap;
} BuiltStack;

typedef struct {
    const DagNode **orig;
    DagNode **copy;
    size_t count;
    size_t cap;
} CloneMap;

typedef struct {
    /* reachability: min depth to produce each port type from the sources */
    /* For simplicity in this split we keep a simple table keyed by port signature */
    /* (In full impl it was more sophisticated with hash or sorted types) */
    size_t n_types;
    Port *types;
    int *min_depth;   /* -1 unreachable */
} ReachTable;

typedef struct {
    int goal_index;
    Port type;
} DagGoal;

typedef struct {
    double score;
    DagNode *roots[CIRCUIT_MAX_ROOTS];
    int ports[CIRCUIT_MAX_ROOTS];
    size_t n_roots;
} DagBest;

/* Plan cache entry (owned by dag_plan module) */
struct PlanCacheEntry {
    int used;
    unsigned long long signature;  /* portable */
    size_t n_sources;
    size_t n_roots;
    DagNode *roots[CIRCUIT_MAX_ROOTS];
    int ports[CIRCUIT_MAX_ROOTS];
    DagNode **owned;
    size_t owned_count;
};

void reach_free(ReachTable *r);
void dag_free_node(DagNode *node);

#endif /* ROUTER_INTERNAL_H */
