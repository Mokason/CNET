#ifndef CNET_VSA_MEMORY_H
#define CNET_VSA_MEMORY_H

#include "cnet_vsa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_STM_MAX_STACK 32
#define CNET_VSA_GRAPH_MAX_NODES 256
#define CNET_VSA_TEXT_SPAN_MAX 256

/* --- 1. Short-Term Working Memory (STM / Scratchpad) --- */

typedef struct {
    int dim;
    float *state;                               /* [dim] active working memory vector */
    float stack[CNET_VSA_STM_MAX_STACK][CNET_VSA_DEFAULT_DIM]; /* backtracking stack */
    int stack_top;
} CnetVsaStm;

/* --- 2. Session Context (Graft-Style Document Graph) --- */

typedef struct {
    int id;
    int parent_id;
    char label[CNET_VSA_NAME_MAX];
    char text_span[CNET_VSA_TEXT_SPAN_MAX];
    float vector[CNET_VSA_DEFAULT_DIM];
} CnetVsaGraphNode;

typedef struct {
    int dim;
    size_t count;
    size_t capacity;
    CnetVsaGraphNode *nodes;
} CnetVsaDocGraph;

/* --- 3. 3-Way Memory Container --- */

typedef struct {
    int dim;
    CnetVsaStm stm;
    CnetVsaDocGraph graph;
    CnetVsaCodebook ltm; /* Long-term permanent memory codebook */
} CnetVsaMemory3Way;

/* STM operations */
int  cnet_vsa_stm_init(CnetVsaStm *stm, int dim);
void cnet_vsa_stm_free(CnetVsaStm *stm);
void cnet_vsa_stm_reset(CnetVsaStm *stm);
/* Update state: state = normalize(decay * state + input_weight * input) */
void cnet_vsa_stm_step(CnetVsaStm *stm, const float *input, float decay, float input_weight);
/* Push current state to scratchpad stack (for hypothetical search / branches) */
int  cnet_vsa_stm_push(CnetVsaStm *stm);
/* Pop and restore previous state (backtrack on conflict / dead end) */
int  cnet_vsa_stm_pop(CnetVsaStm *stm);

/* Document Graph operations */
int  cnet_vsa_graph_init(CnetVsaDocGraph *graph, int dim, size_t capacity);
void cnet_vsa_graph_free(CnetVsaDocGraph *graph);
int  cnet_vsa_graph_add_node(CnetVsaDocGraph *graph, int parent_id,
                             const char *label, const char *text_span,
                             const float *vector);
/* Query the session graph for top-k closest nodes to probe vector */
int  cnet_vsa_graph_query(const CnetVsaDocGraph *graph, const float *query_vec,
                          size_t top_k, const CnetVsaGraphNode **out_nodes,
                          float *out_scores, size_t *out_count);

/* 3-Way Memory unified container */
int  cnet_vsa_memory_init(CnetVsaMemory3Way *mem, int dim,
                          size_t graph_cap, size_t ltm_cap);
void cnet_vsa_memory_free(CnetVsaMemory3Way *mem);

/* Sleep consolidation: extracts prominent session graph vectors and consolidates
   them into LTM if they exceed similarity threshold against prior clusters. */
int  cnet_vsa_memory_consolidate(CnetVsaMemory3Way *mem, float merge_threshold,
                                 size_t *promoted_count);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_MEMORY_H */
