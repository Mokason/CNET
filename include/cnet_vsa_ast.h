#ifndef CNET_VSA_AST_H
#define CNET_VSA_AST_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "cnet_vsa.h"
#include "cnet_vsa_bsc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_AST_MAX_NODES 256
#define CNET_VSA_AST_MAX_EDGES 512

typedef enum {
    CNET_VSA_EDGE_CALLS = 0,
    CNET_VSA_EDGE_CONSTRAINS = 1,
    CNET_VSA_EDGE_VERIFIES = 2,
    CNET_VSA_EDGE_ALLOCATES = 3,
    CNET_VSA_EDGE_FREES = 4,
    CNET_VSA_EDGE_COUNT = 5
} CnetVsaAstRelation;

typedef struct {
    int node_id;
    char name[CNET_VSA_NAME_MAX];
    char kind[32]; /* "function", "struct", "contract" */
    float vector[CNET_VSA_DEFAULT_DIM];
} CnetVsaAstNode;

typedef struct {
    int src_node_id;
    int dst_node_id;
    CnetVsaAstRelation rel;
} CnetVsaAstEdge;

typedef struct {
    int dim;
    size_t node_count;
    size_t edge_count;
    CnetVsaAstNode nodes[CNET_VSA_AST_MAX_NODES];
    CnetVsaAstEdge edges[CNET_VSA_AST_MAX_EDGES];
    float rel_vectors[CNET_VSA_EDGE_COUNT][CNET_VSA_DEFAULT_DIM];
    float graph_bundle[CNET_VSA_DEFAULT_DIM]; /* Unified graph hypervector */
    CnetVsaCodebook symbol_vocab;
} CnetVsaAstGraph;

/* Initialize AST Graph Reasoner */
int  cnet_vsa_ast_init(CnetVsaAstGraph *g, int dim);
void cnet_vsa_ast_free(CnetVsaAstGraph *g);

/* Add code entity node */
int  cnet_vsa_ast_add_node(CnetVsaAstGraph *g, const char *name, const char *kind);

/* Add directed relational edge: src_name -[rel]-> dst_name */
int  cnet_vsa_ast_add_edge(CnetVsaAstGraph *g, const char *src_name, const char *dst_name, CnetVsaAstRelation rel);

/* Compile all edges into unified graph superposition hypervector */
int  cnet_vsa_ast_compile(CnetVsaAstGraph *g);

/* Query: "What does src_name [rel]?"
   probe = G * V_src * R_rel -> clean_up -> dst_name */
int  cnet_vsa_ast_query_callee(const CnetVsaAstGraph *g, const char *src_name, CnetVsaAstRelation rel,
                               char *out_dst_name, size_t max_len, float *out_sim);

/* Query: "Who [rel] dst_name?"
   probe = G * R_rel * V_dst -> clean_up -> src_name */
int  cnet_vsa_ast_query_caller(const CnetVsaAstGraph *g, const char *dst_name, CnetVsaAstRelation rel,
                               char *out_src_name, size_t max_len, float *out_sim);

/* Detect Invariant Violation:
   Returns 1 if a function allocates resources without a verified path to free/contract verification */
int  cnet_vsa_ast_check_leak_invariants(const CnetVsaAstGraph *g, const char *fn_name);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_AST_H */
