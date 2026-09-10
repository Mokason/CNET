#include "../include/cnet_vsa_doc_graft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

int cnet_vsa_doc_graft_init(CnetVsaDocGraft *dg, int dim) {
    if (!dg || dim <= 0) return -1;
    memset(dg, 0, sizeof(*dg));
    dg->dim = dim;
    return 0;
}

void cnet_vsa_doc_graft_free(CnetVsaDocGraft *dg) {
    if (!dg) return;
    memset(dg, 0, sizeof(*dg));
}

int cnet_vsa_doc_graft_add_clause(CnetVsaDocGraft *dg, const char *section_name,
                                  const char *clause_text, int line_number,
                                  const float *semantic_vec) {
    if (!dg || !section_name || !clause_text || !semantic_vec ||
        dg->clause_count >= CNET_VSA_DOC_MAX_CLAUSES) {
        return -1;
    }

    /* Find or create section */
    int sec_idx = -1;
    for (size_t i = 0; i < dg->section_count; ++i) {
        if (strncmp(dg->sections[i].section_name, section_name, CNET_VSA_NAME_MAX) == 0) {
            sec_idx = (int)i;
            break;
        }
    }

    if (sec_idx < 0) {
        if (dg->section_count >= 32) return -1;
        sec_idx = (int)dg->section_count++;
        CnetVsaDocSection *sec = &dg->sections[sec_idx];
        sec->section_id = sec_idx + 1;
        snprintf(sec->section_name, sizeof(sec->section_name), "%s", section_name);
        memcpy(sec->centroid, semantic_vec, (size_t)dg->dim * sizeof(float));
        sec->clause_count = 1;
    } else {
        CnetVsaDocSection *sec = &dg->sections[sec_idx];
        /* Update section centroid running average */
        for (int i = 0; i < dg->dim; ++i) {
            sec->centroid[i] = sec->centroid[i] * (float)sec->clause_count + semantic_vec[i];
        }
        cnet_vsa_normalize(sec->centroid, dg->dim);
        sec->clause_count++;
    }

    CnetVsaDocClause *cl = &dg->clauses[dg->clause_count++];
    cl->clause_id = (int)dg->clause_count;
    cl->section_id = sec_idx + 1;
    snprintf(cl->section_name, sizeof(cl->section_name), "%s", section_name);
    snprintf(cl->text, sizeof(cl->text), "%s", clause_text);
    cl->line_number = line_number;
    memcpy(cl->vector, semantic_vec, (size_t)dg->dim * sizeof(float));

    return cl->clause_id;
}

int cnet_vsa_doc_graft_query(const CnetVsaDocGraft *dg, const float *query_vec,
                              const CnetVsaDocClause **out_best_clause,
                              float *out_sim) {
    if (!dg || !query_vec || dg->clause_count == 0 || !out_best_clause) return -1;

    /* Phase 1: Rank sections by centroid similarity */
    float sec_sims[32] = {0};
    size_t sec_order[32] = {0};
    for (size_t s = 0; s < dg->section_count; ++s) {
        sec_sims[s] = cnet_vsa_similarity(query_vec, dg->sections[s].centroid, dg->dim);
        sec_order[s] = s;
    }

    for (size_t i = 0; i < dg->section_count; ++i) {
        for (size_t j = i + 1; j < dg->section_count; ++j) {
            if (sec_sims[sec_order[j]] > sec_sims[sec_order[i]]) {
                size_t tmp = sec_order[i];
                sec_order[i] = sec_order[j];
                sec_order[j] = tmp;
            }
        }
    }

    /* Phase 2: Hierarchical search in top sections */
    float best_sim = -2.0f;
    const CnetVsaDocClause *best_clause = NULL;

    size_t search_sections = dg->section_count < 3 ? dg->section_count : 3;
    for (size_t s_rank = 0; s_rank < search_sections; ++s_rank) {
        int target_sec_id = (int)sec_order[s_rank] + 1;
        for (size_t c = 0; c < dg->clause_count; ++c) {
            if (dg->clauses[c].section_id == target_sec_id) {
                float sim = cnet_vsa_similarity(query_vec, dg->clauses[c].vector, dg->dim);
                if (sim > best_sim) {
                    best_sim = sim;
                    best_clause = &dg->clauses[c];
                }
            }
        }
    }

    if (!best_clause) return -1;

    *out_best_clause = best_clause;
    if (out_sim) *out_sim = best_sim;
    return 0;
}

int cnet_vsa_doc_graft_find_cross_reference(const CnetVsaDocGraft *dg,
                                             const float *probe_a,
                                             const float *probe_b,
                                             const CnetVsaDocClause **out_clause_a,
                                             const CnetVsaDocClause **out_clause_b,
                                             float *out_joint_score) {
    if (!dg || !probe_a || !probe_b || !out_clause_a || !out_clause_b) return -1;

    float sim_a = 0.0f, sim_b = 0.0f;
    int rc_a = cnet_vsa_doc_graft_query(dg, probe_a, out_clause_a, &sim_a);
    int rc_b = cnet_vsa_doc_graft_query(dg, probe_b, out_clause_b, &sim_b);

    if (rc_a != 0 || rc_b != 0) return -1;

    if (out_joint_score) {
        *out_joint_score = (sim_a + sim_b) / 2.0f;
    }
    return 0;
}
