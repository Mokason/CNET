#ifndef CNET_VSA_DOC_GRAFT_H
#define CNET_VSA_DOC_GRAFT_H

#include "cnet_vsa.h"
#include "cnet_vsa_memory.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CNET_VSA_DOC_MAX_CLAUSES 128
#define CNET_VSA_DOC_TEXT_MAX 512

typedef struct {
    int clause_id;
    int section_id;
    char section_name[CNET_VSA_NAME_MAX];
    char text[CNET_VSA_DOC_TEXT_MAX];
    int line_number;
    float vector[CNET_VSA_DEFAULT_DIM];
} CnetVsaDocClause;

typedef struct {
    int section_id;
    char section_name[CNET_VSA_NAME_MAX];
    float centroid[CNET_VSA_DEFAULT_DIM];
    size_t clause_count;
} CnetVsaDocSection;

typedef struct {
    int dim;
    size_t clause_count;
    size_t section_count;
    CnetVsaDocClause clauses[CNET_VSA_DOC_MAX_CLAUSES];
    CnetVsaDocSection sections[32];
} CnetVsaDocGraft;

/* Initialize Doc-Graft container */
int  cnet_vsa_doc_graft_init(CnetVsaDocGraft *dg, int dim);
void cnet_vsa_doc_graft_free(CnetVsaDocGraft *dg);

/* Ingest a text clause into the graph, assigning it to a section and computing its VSA vector */
int  cnet_vsa_doc_graft_add_clause(CnetVsaDocGraft *dg, const char *section_name,
                                  const char *clause_text, int line_number,
                                  const float *semantic_vec);

/* Hierarchical Needle Query:
   First matches query_vec against section centroids, then searches within top sections.
   Returns best matching clause text, line number, and similarity score. */
int  cnet_vsa_doc_graft_query(const CnetVsaDocGraft *dg, const float *query_vec,
                              const CnetVsaDocClause **out_best_clause,
                              float *out_sim);

/* Cross-Clause Contradiction / Conflict Discovery:
   Given two topic probe vectors, finds the highest-relevance clause pair and verifies
   whether they constrain or qualify each other. */
int  cnet_vsa_doc_graft_find_cross_reference(const CnetVsaDocGraft *dg,
                                             const float *probe_a,
                                             const float *probe_b,
                                             const CnetVsaDocClause **out_clause_a,
                                             const CnetVsaDocClause **out_clause_b,
                                             float *out_joint_score);

#ifdef __cplusplus
}
#endif

#endif /* CNET_VSA_DOC_GRAFT_H */
