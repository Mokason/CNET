#include "../../include/router.h"
#include "../../include/router/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Attention / multi-head advisory ranking + telemetry (real extracted) */

typedef struct {
    double type_compat;
    double tag_compat;
    double reliability;
    double margin;
    double law_compat;
    double cost;
    double chunkability;
    double source_satisfiability;
    double projection_suitability;
    double advisory_score;
} PlanHeadScores;

static double port_projection_suitability(Port port, Port goal) {
    if (!port_compatible(port, goal)) return 0.0;
    if (port.tag[0] && goal.tag[0]) {
        return (strcmp(port.tag, goal.tag) == 0) ? 1.0 : 0.75;
    }
    return 0.9;
}

static void compute_primitive_multihead_score(const BinaryTransformNetwork *p,
                                              Port goal,
                                              PlanHeadScores *out,
                                              const DagSource *sources, size_t n_sources,
                                              ReachTable *reach,
                                              const PrimitiveRegistry *reg) {
    (void)reg; (void)reach;
    memset(out, 0, sizeof(*out));
    if (!p) { out->advisory_score = 0; return; }

    int has = 0;
    for (size_t oj = 0; oj < p->output_port_count; ++oj) {
        if (port_compatible(p->output_ports[oj], goal)) {
            has = 1;
            break;
        }
    }
    out->type_compat = has ? 1.0 : 0.0;
    out->tag_compat = 1.0; /* simplified */
    out->reliability = btn_reliability(p);
    out->margin = 0.5 + 0.1 * (out->reliability - 0.5);
    out->law_compat = 1.0;
    out->cost = 0.95;
    out->chunkability = 1.0;
    out->source_satisfiability = 1.0;
    out->projection_suitability = port_projection_suitability(p->output_ports[0], goal);
    out->advisory_score = out->reliability * (0.25 + 0.75 * out->source_satisfiability);
}

static void rank_by_multihead_ex(const PrimitiveRegistry *reg, size_t *order, Port goal,
                                 const DagSource *sources, size_t n_sources,
                                 ReachTable *reach) {
    size_t n = reg->count;
    /* simple sort by advisory score (desc) */
    for (size_t i = 1; i < n; ++i) {
        for (size_t j = i; j > 0; --j) {
            PlanHeadScores s1, s2;
            compute_primitive_multihead_score(reg->entries[order[j]].btn, goal, &s1, sources, n_sources, reach, reg);
            compute_primitive_multihead_score(reg->entries[order[j-1]].btn, goal, &s2, sources, n_sources, reach, reg);
            if (s1.advisory_score > s2.advisory_score) {
                size_t t = order[j]; order[j] = order[j-1]; order[j-1] = t;
            } else break;
        }
    }
}

static void rank_by_multihead(const PrimitiveRegistry *reg, size_t *order, Port goal,
                              const DagSource *sources, size_t n_sources) {
    ReachTable dummy = {0};
    rank_by_multihead_ex(reg, order, goal, sources, n_sources, &dummy);
    reach_free(&dummy);
}

static void enforce_order_only_no_prune(const PrimitiveRegistry *reg, size_t *order, Port goal,
                                        const DagSource *sources, size_t n_sources) {
    rank_by_multihead(reg, order, goal, sources, n_sources);
}

static int attn_top_k_internal(const PrimitiveRegistry *reg,
                               Port goal,
                               const DagSource *sources, size_t n_sources,
                               size_t k, size_t *out_indices) {
    if (k == 0 || !out_indices) return 0;
    size_t *order = (size_t*)malloc(reg->count * sizeof(size_t));
    if (!order) return 0;
    for (size_t i=0; i<reg->count; i++) order[i]=i;

    rank_by_multihead(reg, order, goal, sources, n_sources);

    size_t written = 0;
    for (size_t i=0; i<reg->count && written < k; ++i) {
        size_t idx = order[i];
        if (entry_usable(reg, idx)) {
            out_indices[written++] = idx;
        }
    }
    free(order);
    return (int)written;
}

size_t attention_top_k(const PrimitiveRegistry *reg, Port goal,
                       const DagSource *sources, size_t n_sources,
                       size_t k, size_t *out_indices) {
    return (size_t)attn_top_k_internal(reg, goal, sources, n_sources, k, out_indices);
}

static void compute_circuit_attention_telemetry(...) {
    /* telemetry computation stub - full version can be ported similarly */
}



