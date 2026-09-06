#include "cnet_semantic_cortex.h"

#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CNET_SEMANTIC_MAX_PROPOSALS 8u
#define CNET_SEMANTIC_TOKEN_MAX 96u

int cnet_semantic_capsule_intent(const char *query, char *typed, size_t cap) {
    if (!typed || !cap) return -1;
    typed[0] = 0;
    if (!query) return -1;
    while (isspace((unsigned char)*query)) query++;
    char first[32] = "", second[32] = "", third[32] = "";
    (void)sscanf(query, "%31s %31s %31s", first, second, third);
    if (!strcasecmp(first, "please")) {
        if (strcasecmp(second, "convert")) return 0;
        query += strlen(first);
        while (isspace((unsigned char)*query)) query++;
        (void)sscanf(query, "%31s %31s %31s", first, second, third);
    }
    int bare = (isdigit((unsigned char)first[0]) || first[0] == '-' || first[0] == '+') && !strcasecmp(third, "in");
    int what = !strcasecmp(first, "what") && !strcasecmp(second, "is") &&
        (isdigit((unsigned char)third[0]) || third[0] == '-' || third[0] == '+');
    if (strcasecmp(first, "convert") &&
        (strcasecmp(first, "how") || strcasecmp(second, "many")) && !bare && !what) return 0;
    char buf[512], token[8][32]; size_t n = 0, len = strlen(query);
    if (len >= sizeof buf) return -1;
    memcpy(buf, query, len + 1);
    while (len && isspace((unsigned char)buf[len - 1])) buf[--len] = 0;
    if (len && buf[len - 1] == '?') buf[--len] = 0;
    char *save = NULL;
    for (char *p = strtok_r(buf, " \t\r\n\v\f", &save); p; p = strtok_r(NULL, " \t\r\n\v\f", &save)) {
        if (n == 8 || strlen(p) >= sizeof token[0]) return -1;
        strcpy(token[n++], p);
    }
    if (!n) return 0;
    for (char *p = token[0]; *p; p++) *p = (char)tolower((unsigned char)*p);
    const char *input, *output, *value;
    if (!strcmp(token[0], "convert")) {
        if (n != 5 || (strcasecmp(token[3], "to") && strcasecmp(token[3], "into"))) return -1;
        value = token[1]; input = token[2]; output = token[4];
    } else if (!strcmp(token[0], "how") && n > 1 && !strcasecmp(token[1], "many")) {
        if (n == 6 && !strcasecmp(token[3], "in")) {
            output = token[2]; value = token[4]; input = token[5];
        } else if (n == 7 && !strcasecmp(token[3], "are") && !strcasecmp(token[4], "in")) {
            output = token[2]; value = token[5]; input = token[6];
        } else return -1;
    } else if (what) {
        if (n != 6 || strcasecmp(token[4], "in")) return -1;
        value = token[2]; input = token[3]; output = token[5];
    } else if (bare) {
        if (n != 4) return -1;
        value = token[0]; input = token[1]; output = token[3];
    } else return 0;
    unsigned v = 0;
    for (const char *p = value; *p; p++) {
        if (!isdigit((unsigned char)*p)) return -1;
        v = v * 10 + (unsigned)(*p - '0'); if (v > 65535) return -1;
    }
    const char *tags[] = {input, output};
    for (size_t i = 0; i < 2; i++) for (const char *p = tags[i]; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') return -1;
    int written = snprintf(typed, cap, "capsule %s %s %u", input, output, v);
    if (written < 0 || (size_t)written >= cap) { typed[0] = 0; return -1; }
    return 1;
}

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
