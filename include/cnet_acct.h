/* Serve/learn accounting — cheap counters for open-lab style economics. */
#ifndef CNET_ACCT_H
#define CNET_ACCT_H

#include "cnet_export.h"

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t tier_a_hits;
    uint64_t hard_expert_hits;
    uint64_t tier_b_hits;
    uint64_t tier_c_hits;
    uint64_t teacher_forwards;
    uint64_t gap_notes;
    uint64_t abstains;
    uint64_t errors;
    uint64_t activated_steps; /* sum of plan lengths / hard steps */
    uint64_t adapter_tick_pass;
    uint64_t adapter_tick_reject;
    uint64_t dense_heal_skips; /* certified adapter avoided dense heal */
} CnetAcct;

CNET_API void cnet_acct_reset(void);
CNET_API void cnet_acct_get(CnetAcct *out);
CNET_API void cnet_acct_add_tier_a(uint64_t steps);
CNET_API void cnet_acct_add_hard(uint64_t steps);
CNET_API void cnet_acct_add_tier_b(void);
CNET_API void cnet_acct_add_tier_c(void);
CNET_API void cnet_acct_add_teacher(void);
CNET_API void cnet_acct_add_gap(void);
CNET_API void cnet_acct_add_abstain(void);
CNET_API void cnet_acct_add_error(void);
CNET_API void cnet_acct_add_adapter_pass(void);
CNET_API void cnet_acct_add_adapter_reject(void);

/* Append one JSON line to path (CNET_ACCT_LOG or arg). Returns 0/-1. */
CNET_API int cnet_acct_dump(const char *path);

#ifdef __cplusplus
}
#endif
#endif
