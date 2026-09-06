#ifndef CNET_SEMANTIC_CORTEX_H
#define CNET_SEMANTIC_CORTEX_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_export.h"
#include "cnet_shared_workspace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum CnetSemanticBackend {
    CNET_SEMANTIC_HERMETIC = 1,
    CNET_SEMANTIC_RESIDUAL_HTTP = 2
} CnetSemanticBackend;

typedef int (*CnetSemanticTopkFn)(const double *input,
                                 double *output,
                                 void *context,
                                 int k);

typedef struct CnetSemanticCortex {
    CnetSemanticBackend backend;
    CnetSemanticTopkFn topk;
    void *context;
    const int *window_ids;
    size_t window_size;
    char source_tag[CNET_WORKSPACE_SOURCE_MAX];
} CnetSemanticCortex;

/* Constrained, whole-request intent adapter; does not certify an answer.
 * Supported forms: "convert N INPUT_TAG to OUTPUT_TAG" and
 * "how many OUTPUT_TAG in N INPUT_TAG?". Tags retain their exact spelling.
 * 1 = typed proposal; 0 = unrelated; -1 = recognized but invalid/ambiguous.
 * Numeric range is 0..65535; actual ports and coverage must still be checked. */
CNET_API int cnet_semantic_capsule_intent(const char *query, char *typed, size_t cap);

CNET_API int cnet_semantic_cortex_init_hermetic(
    CnetSemanticCortex *cortex, const char *source_tag);
CNET_API int cnet_semantic_cortex_init_residual_http(
    CnetSemanticCortex *cortex,
    CnetSemanticTopkFn topk,
    void *context,
    const int *window_ids,
    size_t window_size,
    const char *source_tag);
CNET_API int cnet_semantic_cortex_propose(
    const CnetSemanticCortex *cortex,
    const char *query,
    size_t proposal_limit,
    uint64_t timestamp_ms,
    CnetSharedWorkspace *workspace,
    size_t *out_proposals);

#ifdef __cplusplus
}
#endif

#endif
