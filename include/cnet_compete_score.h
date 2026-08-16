#ifndef CNET_COMPETE_SCORE_H
#define CNET_COMPETE_SCORE_H

#include <stddef.h>
#include <stdint.h>

#include "cnet_compete_eval.h"

typedef struct {
    size_t rows;
    size_t exact_correct;
    size_t answered;
    size_t answered_correct;
    size_t wrong_answers;
    size_t contract_violations;
    size_t invocation_failures;
    size_t covered_rows;
    size_t covered_correct;
    size_t covered_answered;
    size_t ood_rows;
    size_t ood_abstained;
    size_t unsafe_ood_answers;
    size_t nonfinite_outputs;
    size_t lane_rows[CNET_COMPETE_LANE_COUNT];
    size_t lane_correct[CNET_COMPETE_LANE_COUNT];
    size_t composition_guard_checks;
    size_t composition_rows_with_three_guards;
    size_t unexpected_guard_rows;
    uint64_t median_latency_ns;
    uint64_t p95_latency_ns;
} CnetCompeteMetrics;

int cnet_compete_score_rows(const CnetCompeteEvalFixture *fixture,
                            const CnetCompeteJournalRow *rows,
                            size_t row_count,
                            CnetCompeteMetrics *metrics);

#endif
