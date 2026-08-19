#include "cnet_semantic_cortex.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CNET_SEMANTIC_MAX_PROPOSALS 8u
#define CNET_SEMANTIC_TOKEN_MAX 96u

static void semantic_copy(char *dst, size_t dst_size, const char *src) {
    size_t n;
    if (!dst || dst_size == 0) return;
    n = src ? strlen(src) : 0;
    if (n >= dst_size) n = dst_size - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint64_t semantic_hash(const char *text) {
    uint64_t hash = UINT64_C(1469598103934665603);
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        hash ^= (uint64_t)*p++;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int semantic_publish(const CnetSemanticCortex *cortex,
                            CnetSharedWorkspace *workspace,
                            const char *text,
                            double score,
                            uint64_t timestamp_ms) {
    return cnet_workspace_push(workspace, CNET_WORKSPACE_CANDIDATE,
                               CNET_WORKSPACE_UNCERTIFIED,
                               cortex->source_tag, text, score,
                               timestamp_ms);
}

static int semantic_propose_hermetic(const CnetSemanticCortex *cortex,
                                     const char *query,
                                     size_t limit,
                                     uint64_t timestamp_ms,
                                     CnetSharedWorkspace *workspace,
                                     size_t *out_proposals) {
    char token[CNET_SEMANTIC_TOKEN_MAX];
    size_t token_len = 0, query_len = strlen(query), emitted = 0, i;
    for (i = 0; i <= query_len && emitted < limit; i++) {
        const unsigned char c = (unsigned char)query[i];
        if (isalnum(c) || c == '_' || c == '-') {
            if (token_len + 1 < sizeof(token))
                token[token_len++] = (char)tolower(c);
            continue;
        }
        if (token_len >= 3) {
            char proposal[CNET_WORKSPACE_TEXT_MAX];
            const double score = 0.35 +
                0.45 * ((double)token_len / (double)(token_len + 8));
            token[token_len] = '\0';
            (void)snprintf(proposal, sizeof(proposal),
                           "semantic-candidate:%s", token);
            if (semantic_publish(cortex, workspace, proposal, score,
                                 timestamp_ms + emitted) != 0) {
                return -2;
            }
            emitted++;
        }
        token_len = 0;
    }
    *out_proposals = emitted;
    return emitted > 0 ? 0 : 1;
}

static int semantic_propose_residual(const CnetSemanticCortex *cortex,
                                     const char *query,
                                     size_t limit,
                                     uint64_t timestamp_ms,
                                     CnetSharedWorkspace *workspace,
                                     size_t *out_proposals) {
    double *input, *output;
    size_t rank, i, emitted = 0;
    int status;
    input = (double *)calloc(cortex->window_size, sizeof(*input));
    output = (double *)calloc(limit * cortex->window_size, sizeof(*output));
    if (!input || !output) {
        free(input);
        free(output);
        return -3;
    }
    input[semantic_hash(query) % cortex->window_size] = 1.0;
    status = cortex->topk(input, output, cortex->context, (int)limit);
    if (status != 0) {
        free(input);
        free(output);
        return -4;
    }
    for (rank = 0; rank < limit; rank++) {
        size_t best = 0;
        char proposal[CNET_WORKSPACE_TEXT_MAX];
        for (i = 1; i < cortex->window_size; i++) {
            if (output[rank * cortex->window_size + i] >
                output[rank * cortex->window_size + best]) {
                best = i;
            }
        }
        if (output[rank * cortex->window_size + best] <= 0.0) continue;
        (void)snprintf(proposal, sizeof(proposal),
                       "residual-token:%d", cortex->window_ids[best]);
        if (semantic_publish(cortex, workspace, proposal,
                             1.0 / (double)(rank + 1),
                             timestamp_ms + emitted) != 0) {
            free(input);
            free(output);
            return -2;
        }
        emitted++;
    }
    free(input);
    free(output);
    *out_proposals = emitted;
    return emitted > 0 ? 0 : 1;
}

int cnet_semantic_cortex_init_hermetic(CnetSemanticCortex *cortex,
                                        const char *source_tag) {
    if (!cortex) return -1;
    memset(cortex, 0, sizeof(*cortex));
    cortex->backend = CNET_SEMANTIC_HERMETIC;
    semantic_copy(cortex->source_tag, sizeof(cortex->source_tag),
                  source_tag && source_tag[0] ? source_tag :
                  "semantic_cortex:hermetic");
    return 0;
}

int cnet_semantic_cortex_init_residual_http(
    CnetSemanticCortex *cortex,
    CnetSemanticTopkFn topk,
    void *context,
    const int *window_ids,
    size_t window_size,
    const char *source_tag) {
    if (!cortex || !topk || !window_ids || window_size == 0) return -1;
    memset(cortex, 0, sizeof(*cortex));
    cortex->backend = CNET_SEMANTIC_RESIDUAL_HTTP;
    cortex->topk = topk;
    cortex->context = context;
    cortex->window_ids = window_ids;
    cortex->window_size = window_size;
    semantic_copy(cortex->source_tag, sizeof(cortex->source_tag),
                  source_tag && source_tag[0] ? source_tag :
                  "semantic_cortex:residual_http");
    return 0;
}

int cnet_semantic_cortex_propose(const CnetSemanticCortex *cortex,
                                 const char *query,
                                 size_t proposal_limit,
                                 uint64_t timestamp_ms,
                                 CnetSharedWorkspace *workspace,
                                 size_t *out_proposals) {
    if (out_proposals) *out_proposals = 0;
    if (!cortex || !query || query[0] == '\0' || !workspace ||
        !out_proposals || proposal_limit == 0 ||
        proposal_limit > CNET_SEMANTIC_MAX_PROPOSALS) {
        return -1;
    }
    if (cortex->backend == CNET_SEMANTIC_HERMETIC) {
        return semantic_propose_hermetic(cortex, query, proposal_limit,
                                         timestamp_ms, workspace,
                                         out_proposals);
    }
    if (cortex->backend == CNET_SEMANTIC_RESIDUAL_HTTP &&
        cortex->topk && cortex->window_ids && cortex->window_size > 0) {
        if (cortex->window_size > SIZE_MAX / proposal_limit ||
            cortex->window_size * proposal_limit >
                SIZE_MAX / sizeof(double)) {
            return -1;
        }
        return semantic_propose_residual(cortex, query, proposal_limit,
                                         timestamp_ms, workspace,
                                         out_proposals);
    }
    return -1;
}
