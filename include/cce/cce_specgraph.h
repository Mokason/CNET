#ifndef CCE_SPECGRAPH_H
#define CCE_SPECGRAPH_H

/* Specialist knowledge graph: the "codebase index" of a decomposed model.
 *
 * A loaded model is already a forest of NAMED specialists (qwen2.blk.0.q_proj,
 * mamba.blk.N.in_proj, ...). This module gives each specialist an IDENTITY and
 * the model a queryable structure, the same move codebase-memory-mcp makes on
 * source code (typed nodes with qualified names + typed edges), so that reuse
 * becomes checkable instead of impossible:
 *
 *  - content digest: FNV-1a over block structure + weight/bias bytes.
 *    Two specialists with equal digests hold identical parameters
 *    (byte-verified at the store layer, never trusted on hash alone).
 *  - behavioral fingerprint: K seeded probe vectors run through the cascade,
 *    hash of the outputs. Deterministic; a checkable invariant is
 *    digest-equal => fingerprint-equal. Plus a coarse 8-float signature
 *    for future SIMILAR_TO search (near-duplicates across models).
 *  - DATA_FLOWS edges: the forward wiring, materialized per family
 *    (transformer / ssm) instead of living implicitly in runner code.
 *
 * Everything here is deterministic structure — no learned scores — so the
 * graph's claims stay provable (contract-style, not vibes).
 */

#include "cce_defs.h"
#include "cce_cascade.h"
#include "cce_detect.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CCE_SPEC_NAME_MAX 96
#define CCE_SPEC_SIG_DIM  8

typedef struct {
    char     name[CCE_SPEC_NAME_MAX]; /* qualified branch name */
    char     role[24];                /* suffix after last '.', e.g. "q_proj" */
    int      in_dim, out_dim;
    int      n_blocks;
    uint64_t digest;                  /* content identity (weights+bias bytes) */
    uint64_t fingerprint;             /* behavioral probe hash */
    float    sig[CCE_SPEC_SIG_DIM];   /* coarse behavior signature (probe 0) */
} cce_spec_node;

typedef struct {
    int  src, dst;                    /* node indices */
    char kind[16];                    /* "DATA_FLOWS" (more kinds later) */
} cce_spec_edge;

typedef struct {
    char model_name[64];
    cce_spec_node* nodes; int n_nodes;
    cce_spec_edge* edges; int n_edges;
} cce_spec_graph;

/* Identity primitives (shared with the weight store). */
uint64_t cce_spec_digest(const cce_cascade* cas);
uint64_t cce_spec_fingerprint(const cce_cascade* cas, float sig_out[CCE_SPEC_SIG_DIM]);

/* Build the graph for a loaded model. Nodes for every forest specialist;
 * DATA_FLOWS wiring emitted for transformer and ssm families (other
 * families get nodes only). */
cce_result cce_spec_graph_build(cce_spec_graph** out, const cce_anymodel* m,
                                const char* model_name);

/* Nodes-only build from a bare forest (no wiring knowledge). */
cce_result cce_spec_graph_from_forest(cce_spec_graph** out, cce_forest* f,
                                      const char* model_name);

void cce_spec_graph_free(cce_spec_graph* g);

int cce_spec_graph_find(const cce_spec_graph* g, const char* name); /* -1 if absent */

/* Plain-text sidecar persistence (flat file, CNET style). */
cce_result cce_spec_graph_save(const cce_spec_graph* g, const char* path);
cce_result cce_spec_graph_load(cce_spec_graph** out, const char* path);

#ifdef __cplusplus
}
#endif

#endif /* CCE_SPECGRAPH_H */
