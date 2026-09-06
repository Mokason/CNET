#ifndef CNET_CORE_SELECTOR_H
#define CNET_CORE_SELECTOR_H
#include <stdint.h>
#include "cnet_core_cell.h"
#ifdef __cplusplus
extern "C" {
#endif
#define CNET_SELECTOR_MAX_NODES 64
#define CNET_SELECTOR_FEATURE_VERSION 1
typedef struct {
    uint64_t identity,version;
    uint32_t input_type,output_type;
    unsigned available;
} CnetSelectorNode;
/* Caller constructs this immutable view from one pinned, verified inventory.
 * Type IDs denote complete equal port signatures within that view, not hashes
 * used in place of authoritative port validation. Edges describe availability.
 * The selector never certifies an edge or an execution result. */
typedef struct {
    unsigned feature_version,n,start,goal;
    uint64_t generation;
    CnetSelectorNode node[CNET_SELECTOR_MAX_NODES];
    uint64_t edge[CNET_SELECTOR_MAX_NODES];
} CnetSelectorGraph;
typedef struct {
    uint64_t generation;
    unsigned count,iterations,ranked[CNET_SELECTOR_MAX_NODES];
    unsigned hops,path[CNET_SELECTOR_MAX_NODES];
    int distance;
    float score;
} CnetSelectorProposal;
int cnet_core_selector_validate(const CnetSelectorGraph *graph);
/* Zero count means abstention (or already at goal, distance=0). Invalid input
 * returns -1 with a cleared proposal. Bounded CPU inference; no allocation or
 * mutation. Path excludes start and includes goal; a complete structural path
 * is still only a proposal and must pass actual capsule execution/coverage.
 * Threshold .9 is fixed by the version-1 evaluation contract. */
int cnet_core_selector_propose(const CnetCoreCell *cell,const CnetSelectorGraph *graph,
                              unsigned iterations,CnetSelectorProposal *proposal);
#ifdef __cplusplus
}
#endif
#endif
