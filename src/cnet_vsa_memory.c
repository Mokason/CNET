#include "../include/cnet_vsa_memory.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int cnet_vsa_stm_init(CnetVsaStm *stm, int dim) {
    if (!stm || dim <= 0) return -1;
    stm->dim = dim;
    stm->state = (float *)calloc((size_t)dim, sizeof(float));
    if (!stm->state) return -1;
    stm->stack_top = 0;
    return 0;
}

void cnet_vsa_stm_free(CnetVsaStm *stm) {
    if (!stm) return;
    free(stm->state);
    memset(stm, 0, sizeof(*stm));
}

void cnet_vsa_stm_reset(CnetVsaStm *stm) {
    if (!stm || !stm->state) return;
    memset(stm->state, 0, (size_t)stm->dim * sizeof(float));
    stm->stack_top = 0;
}

void cnet_vsa_stm_step(CnetVsaStm *stm, const float *input, float decay, float input_weight) {
    if (!stm || !stm->state || !input) return;
    for (int i = 0; i < stm->dim; ++i) {
        stm->state[i] = decay * stm->state[i] + input_weight * input[i];
    }
    cnet_vsa_normalize(stm->state, stm->dim);
}

int cnet_vsa_stm_push(CnetVsaStm *stm) {
    if (!stm || !stm->state || stm->stack_top >= CNET_VSA_STM_MAX_STACK) return -1;
    memcpy(stm->stack[stm->stack_top], stm->state, (size_t)stm->dim * sizeof(float));
    stm->stack_top++;
    return 0;
}

int cnet_vsa_stm_pop(CnetVsaStm *stm) {
    if (!stm || !stm->state || stm->stack_top <= 0) return -1;
    stm->stack_top--;
    memcpy(stm->state, stm->stack[stm->stack_top], (size_t)stm->dim * sizeof(float));
    return 0;
}

int cnet_vsa_graph_init(CnetVsaDocGraph *graph, int dim, size_t capacity) {
    if (!graph || dim <= 0 || capacity == 0) return -1;
    graph->dim = dim;
    graph->count = 0;
    graph->capacity = capacity;
    graph->nodes = (CnetVsaGraphNode *)calloc(capacity, sizeof(CnetVsaGraphNode));
    if (!graph->nodes) return -1;
    return 0;
}

void cnet_vsa_graph_free(CnetVsaDocGraph *graph) {
    if (!graph) return;
    free(graph->nodes);
    memset(graph, 0, sizeof(*graph));
}

int cnet_vsa_graph_add_node(CnetVsaDocGraph *graph, int parent_id,
                             const char *label, const char *text_span,
                             const float *vector) {
    if (!graph || !label || !vector || graph->count >= graph->capacity) return -1;
    CnetVsaGraphNode *n = &graph->nodes[graph->count];
    n->id = (int)graph->count + 1;
    n->parent_id = parent_id;
    strncpy(n->label, label, CNET_VSA_NAME_MAX - 1);
    n->label[CNET_VSA_NAME_MAX - 1] = '\0';
    if (text_span) {
        strncpy(n->text_span, text_span, CNET_VSA_TEXT_SPAN_MAX - 1);
        n->text_span[CNET_VSA_TEXT_SPAN_MAX - 1] = '\0';
    } else {
        n->text_span[0] = '\0';
    }
    memcpy(n->vector, vector, (size_t)graph->dim * sizeof(float));
    graph->count++;
    return n->id;
}

int cnet_vsa_graph_query(const CnetVsaDocGraph *graph, const float *query_vec,
                          size_t top_k, const CnetVsaGraphNode **out_nodes,
                          float *out_scores, size_t *out_count) {
    if (!graph || !query_vec || !out_nodes || !out_scores || !out_count || top_k == 0)
        return -1;
    *out_count = 0;
    if (graph->count == 0) return 0;

    size_t k_limit = top_k < graph->count ? top_k : graph->count;

    /* Simple selection for top-k */
    for (size_t k = 0; k < k_limit; ++k) {
        float best_sim = -2.0f;
        size_t best_idx = 0;

        for (size_t i = 0; i < graph->count; ++i) {
            /* Check if already selected */
            int already = 0;
            for (size_t prev = 0; prev < *out_count; ++prev) {
                if (out_nodes[prev] == &graph->nodes[i]) {
                    already = 1;
                    break;
                }
            }
            if (already) continue;

            float sim = cnet_vsa_similarity(query_vec, graph->nodes[i].vector, graph->dim);
            if (sim > best_sim) {
                best_sim = sim;
                best_idx = i;
            }
        }

        if (best_sim > -2.0f) {
            out_nodes[*out_count] = &graph->nodes[best_idx];
            out_scores[*out_count] = best_sim;
            (*out_count)++;
        }
    }
    return 0;
}

int cnet_vsa_memory_init(CnetVsaMemory3Way *mem, int dim,
                          size_t graph_cap, size_t ltm_cap) {
    if (!mem || dim <= 0) return -1;
    mem->dim = dim;
    if (cnet_vsa_stm_init(&mem->stm, dim) != 0) return -1;
    if (cnet_vsa_graph_init(&mem->graph, dim, graph_cap) != 0) {
        cnet_vsa_stm_free(&mem->stm);
        return -1;
    }
    if (cnet_vsa_codebook_init(&mem->ltm, dim, ltm_cap) != 0) {
        cnet_vsa_graph_free(&mem->graph);
        cnet_vsa_stm_free(&mem->stm);
        return -1;
    }
    return 0;
}

void cnet_vsa_memory_free(CnetVsaMemory3Way *mem) {
    if (!mem) return;
    cnet_vsa_stm_free(&mem->stm);
    cnet_vsa_graph_free(&mem->graph);
    cnet_vsa_codebook_free(&mem->ltm);
}

int cnet_vsa_memory_consolidate(CnetVsaMemory3Way *mem, float merge_threshold,
                                 size_t *promoted_count) {
    if (!mem || !promoted_count) return -1;
    *promoted_count = 0;

    for (size_t i = 0; i < mem->graph.count; ++i) {
        const CnetVsaGraphNode *node = &mem->graph.nodes[i];
        float best_sim = 0.0f;
        char clean_name[CNET_VSA_NAME_MAX] = {0};
        float clean_vec[CNET_VSA_DEFAULT_DIM];

        int rc = cnet_vsa_codebook_cleanup(&mem->ltm, node->vector, clean_vec,
                                            clean_name, sizeof(clean_name), &best_sim);
        /* If not found or novel enough below merge_threshold, promote to permanent LTM */
        if (rc != 0 || best_sim < merge_threshold) {
            if (cnet_vsa_codebook_add(&mem->ltm, node->label, node->vector) == 0) {
                (*promoted_count)++;
            }
        }
    }
    return 0;
}
