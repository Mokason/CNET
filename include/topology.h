#ifndef TOPOLOGY_H
#define TOPOLOGY_H

/* Contract-graph topology audit (observability only, ZERO authority).
 *
 * Treats the registry as a CAPABILITY GRAPH: nodes = registered primitives,
 * an undirected edge P--Q iff P can feed Q (some output port of P is
 * port_compatible with some input port of Q) or vice-versa. From that graph
 * it reports topological invariants and library-health diagnostics:
 *
 *   - betti0 = connected components  -> "capability islands" that cannot
 *              currently interoperate. A high count means fragmented coverage.
 *   - betti1 = cycle rank (E - V + C) -> redundant composition paths; a hint
 *              for where dedup/consolidation has something to collapse.
 *   - bridge suggestions ("what to mint next") -> the missing producer->consumer
 *              type transforms that would MERGE two components (reduce betti0).
 *   - dedup candidates -> primitives with identical port signatures (by-signature
 *              interchangeable; advisory consolidation flag, NOT a behavior claim).
 *
 * This is a read-only report. It never registers, certifies, prunes, plans,
 * mutates reliability, or relaxes any contract -- it advises library growth,
 * mirroring the existing snapshot/trend/engram reports in router.h. */

#include <stddef.h>
#include "nn.h"      /* Port, PORT_TAG_MAX */
#include "router.h"  /* PrimitiveRegistry */

#define TOPO_MAX_NODES        256
#define TOPO_MAX_SUGGESTIONS   64
#define TOPO_MAX_DEDUP         64
#define TOPO_SIG_MAX           48   /* printable port-signature buffer */

/* A "what to mint next" suggestion: a primitive consuming `consume_sig` and
   producing `produce_sig` would connect component `from_component` (which
   produces that type) to `to_component` (which consumes it), reducing betti0. */
typedef struct {
    char   produce_sig[TOPO_SIG_MAX];   /* the output type the bridge would emit */
    char   consume_sig[TOPO_SIG_MAX];   /* the input type the bridge would accept */
    int    from_component;              /* component that already PRODUCES produce_sig */
    int    to_component;                /* component that already CONSUMES consume_sig */
    size_t score;                       /* sizes of the two components it would join */
} TopoBridgeSuggestion;

/* Two primitives with identical input- and output-port signatures: by signature
   they are interchangeable, so they are candidates for dedup/consolidation. This
   is a signature observation only -- their behavior may still differ. */
typedef struct {
    char name_a[64];
    char name_b[64];
} TopoDedupCandidate;

typedef struct {
    size_t node_count;     /* V: eligible primitives */
    size_t edge_count;     /* E: undirected composability edges */
    size_t betti0;         /* connected components */
    long   betti1;         /* cycle rank = E - V + C (>= 0) */
    size_t largest_component_size;

    const char *node_name[TOPO_MAX_NODES];  /* borrowed from the registry */
    int         node_component[TOPO_MAX_NODES];

    size_t suggestion_count;
    TopoBridgeSuggestion suggestions[TOPO_MAX_SUGGESTIONS];

    size_t dedup_count;
    TopoDedupCandidate dedup[TOPO_MAX_DEDUP];
} TopoReport;

/* Analyze the capability graph of `reg`. If certified_only != 0, only certified
   entries are treated as nodes (the graph the planner would use under
   require_certified). Returns 0 on success (report filled), -1 on bad args or if
   the registry has more than TOPO_MAX_NODES eligible entries. */
int topology_analyze(const PrimitiveRegistry *reg, int certified_only, TopoReport *out);

/* Human-readable dump of a report (for the demo / CLI audit). Safe on NULL. */
void topology_print_report(const TopoReport *report);

/* Write the report as JSON to `path`, creating parent directories. The
   mint/dedup entries are written as advisory "candidates" -- observations, not
   automatic evolution. Returns 0 on success, -1 on failure. */
int topology_write_json(const TopoReport *report, const char *path);

/* Render a port's signature into buf ("F<fam>w<fw>c<fc>[/tag]"). Exposed for
   tests. Returns buf. */
const char *topology_port_sig(Port p, char *buf, size_t n);

#endif
