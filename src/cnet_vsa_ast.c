#include "../include/cnet_vsa_ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static uint64_t hash_str(const char *s) {
    uint64_t h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

int cnet_vsa_ast_init(CnetVsaAstGraph *g, int dim) {
    if (!g || dim <= 0) return -1;
    memset(g, 0, sizeof(*g));
    g->dim = dim;

    int rc = cnet_vsa_codebook_init(&g->symbol_vocab, dim, CNET_VSA_AST_MAX_NODES);
    if (rc != 0) return -1;

    /* Initialize relational basis vectors */
    const char *rel_names[CNET_VSA_EDGE_COUNT] = {
        "REL_CALLS", "REL_CONSTRAINS", "REL_VERIFIES", "REL_ALLOCATES", "REL_FREES"
    };

    for (int r = 0; r < CNET_VSA_EDGE_COUNT; ++r) {
        uint64_t seed = hash_str(rel_names[r]);
        cnet_vsa_random(g->rel_vectors[r], dim, &seed);
    }

    return 0;
}

void cnet_vsa_ast_free(CnetVsaAstGraph *g) {
    if (!g) return;
    cnet_vsa_codebook_free(&g->symbol_vocab);
    memset(g, 0, sizeof(*g));
}

int cnet_vsa_ast_add_node(CnetVsaAstGraph *g, const char *name, const char *kind) {
    if (!g || !name || !kind || g->node_count >= CNET_VSA_AST_MAX_NODES) return -1;

    for (size_t i = 0; i < g->node_count; ++i) {
        if (strcmp(g->nodes[i].name, name) == 0) {
            return (int)i + 1;
        }
    }

    CnetVsaAstNode *node = &g->nodes[g->node_count++];
    node->node_id = (int)g->node_count;
    snprintf(node->name, sizeof(node->name), "%s", name);
    snprintf(node->kind, sizeof(node->kind), "%s", kind);

    uint64_t seed = hash_str(name);
    cnet_vsa_random(node->vector, g->dim, &seed);

    cnet_vsa_codebook_add(&g->symbol_vocab, name, node->vector);

    return node->node_id;
}

int cnet_vsa_ast_add_edge(CnetVsaAstGraph *g, const char *src_name, const char *dst_name, CnetVsaAstRelation rel) {
    if (!g || !src_name || !dst_name || g->edge_count >= CNET_VSA_AST_MAX_EDGES) return -1;

    int src_id = cnet_vsa_ast_add_node(g, src_name, "unknown");
    int dst_id = cnet_vsa_ast_add_node(g, dst_name, "unknown");

    if (src_id <= 0 || dst_id <= 0) return -1;

    CnetVsaAstEdge *edge = &g->edges[g->edge_count++];
    edge->src_node_id = src_id;
    edge->dst_node_id = dst_id;
    edge->rel = rel;

    return (int)g->edge_count;
}

int cnet_vsa_ast_compile(CnetVsaAstGraph *g) {
    if (!g || g->edge_count == 0) return -1;

    double accum[CNET_VSA_DEFAULT_DIM] = {0};

    for (size_t i = 0; i < g->edge_count; ++i) {
        CnetVsaAstEdge *e = &g->edges[i];
        CnetVsaAstNode *src = &g->nodes[e->src_node_id - 1];
        CnetVsaAstNode *dst = &g->nodes[e->dst_node_id - 1];
        const float *rel_v = g->rel_vectors[e->rel];

        /* Bind triple: tuple = V_src * R_rel * V_dst */
        float bound_sr[CNET_VSA_DEFAULT_DIM];
        float tuple[CNET_VSA_DEFAULT_DIM];

        cnet_vsa_bind(bound_sr, src->vector, rel_v, g->dim);
        cnet_vsa_bind(tuple, bound_sr, dst->vector, g->dim);

        for (int j = 0; j < g->dim; ++j) {
            accum[j] += (double)tuple[j];
        }
    }

    for (int j = 0; j < g->dim; ++j) {
        g->graph_bundle[j] = (float)accum[j];
    }
    cnet_vsa_normalize(g->graph_bundle, g->dim);

    return 0;
}

int cnet_vsa_ast_query_callee(const CnetVsaAstGraph *g, const char *src_name, CnetVsaAstRelation rel,
                               char *out_dst_name, size_t max_len, float *out_sim) {
    if (!g || !src_name || !out_dst_name || max_len == 0) return -1;

    float src_vec[CNET_VSA_DEFAULT_DIM];
    if (cnet_vsa_codebook_find(&g->symbol_vocab, src_name, src_vec) != 0) {
        return -1;
    }

    const float *rel_v = g->rel_vectors[rel];

    /* Key = V_src * R_rel */
    float key[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_bind(key, src_vec, rel_v, g->dim);

    /* Probe = G * Key */
    float probe[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_unbind(probe, g->graph_bundle, key, g->dim);

    return cnet_vsa_codebook_cleanup(&g->symbol_vocab, probe, NULL, out_dst_name, max_len, out_sim);
}

int cnet_vsa_ast_query_caller(const CnetVsaAstGraph *g, const char *dst_name, CnetVsaAstRelation rel,
                               char *out_src_name, size_t max_len, float *out_sim) {
    if (!g || !dst_name || !out_src_name || max_len == 0) return -1;

    float dst_vec[CNET_VSA_DEFAULT_DIM];
    if (cnet_vsa_codebook_find(&g->symbol_vocab, dst_name, dst_vec) != 0) {
        return -1;
    }

    const float *rel_v = g->rel_vectors[rel];

    /* Key = R_rel * V_dst */
    float key[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_bind(key, rel_v, dst_vec, g->dim);

    /* Probe = G * Key */
    float probe[CNET_VSA_DEFAULT_DIM];
    cnet_vsa_unbind(probe, g->graph_bundle, key, g->dim);

    return cnet_vsa_codebook_cleanup(&g->symbol_vocab, probe, NULL, out_src_name, max_len, out_sim);
}

int cnet_vsa_ast_check_leak_invariants(const CnetVsaAstGraph *g, const char *fn_name) {
    if (!g || !fn_name) return -1;

    char alloc_target[CNET_VSA_NAME_MAX] = {0};
    float alloc_sim = 0.0f;
    int rc_alloc = cnet_vsa_ast_query_callee(g, fn_name, CNET_VSA_EDGE_ALLOCATES,
                                             alloc_target, sizeof(alloc_target), &alloc_sim);

    if (rc_alloc != 0 || alloc_sim < 0.25f) {
        /* Function does not allocate resources */
        return 0;
    }

    char free_target[CNET_VSA_NAME_MAX] = {0};
    float free_sim = 0.0f;
    int rc_free = cnet_vsa_ast_query_callee(g, fn_name, CNET_VSA_EDGE_FREES,
                                           free_target, sizeof(free_target), &free_sim);

    char verify_target[CNET_VSA_NAME_MAX] = {0};
    float verify_sim = 0.0f;
    int rc_verify = cnet_vsa_ast_query_callee(g, fn_name, CNET_VSA_EDGE_VERIFIES,
                                              verify_target, sizeof(verify_target), &verify_sim);

    if ((rc_free == 0 && free_sim >= 0.25f) || (rc_verify == 0 && verify_sim >= 0.25f)) {
        return 0; /* Invariant satisfied */
    }

    return 1; /* Invariant violated: allocates without free/verification */
}
