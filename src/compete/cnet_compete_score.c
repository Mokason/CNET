#include "cnet_compete_score.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int compare_latency(const void *left, const void *right) {
    uint64_t a = *(const uint64_t *)left, b = *(const uint64_t *)right;
    return (a > b) - (a < b);
}

static int contains_nonfinite_literal(const char *text) {
    const unsigned char *cursor = (const unsigned char *)text;
    int in_string = 0, escaped = 0;
    if (text == NULL) return 0;
    while (*cursor != '\0') {
        if (in_string) {
            if (escaped) escaped = 0;
            else if (*cursor == '\\') escaped = 1;
            else if (*cursor == '"') in_string = 0;
            ++cursor;
            continue;
        }
        if (*cursor == '"') { in_string = 1; ++cursor; continue; }
        if (strncmp((const char *)cursor, "NaN", 3) == 0 ||
            strncmp((const char *)cursor, "Infinity", 8) == 0 ||
            strncmp((const char *)cursor, "-Infinity", 9) == 0)
            return 1;
        if (*cursor == '-' || isdigit(*cursor)) {
            char *end = NULL;
            double value;
            errno = 0;
            value = strtod((const char *)cursor, &end);
            if (end != (char *)cursor) {
                if (errno == ERANGE || !isfinite(value)) return 1;
                cursor = (const unsigned char *)end;
                continue;
            }
        }
        ++cursor;
    }
    return 0;
}

int cnet_compete_score_rows(const CnetCompeteEvalFixture *fixture,
                            const CnetCompeteJournalRow *rows,
                            size_t row_count,
                            CnetCompeteMetrics *metrics) {
    CnetCompeteMetrics local;
    uint64_t *latencies;
    size_t index;
    if (fixture == NULL || rows == NULL || metrics == NULL ||
        fixture->rows == NULL || row_count != fixture->count || row_count == 0)
        return -1;
    memset(&local, 0, sizeof local);
    latencies = (uint64_t *)malloc(row_count * sizeof latencies[0]);
    if (latencies == NULL) return -1;
    local.rows = row_count;
    for (index = 0; index < row_count; ++index) {
        const CnetCompeteEvalRow *expected = &fixture->rows[index];
        const CnetCompeteJournalRow *observed = &rows[index];
        CnetCompeteParsedResult parsed;
        int correct = 0;
        if (strcmp(expected->id, observed->id) != 0 ||
            expected->lane < 0 || expected->lane >= CNET_COMPETE_LANE_COUNT) {
            free(latencies);
            return -1;
        }
        latencies[index] = observed->latency_ns;
        ++local.lane_rows[expected->lane];
        if (expected->lane == CNET_COMPETE_LANE_OOD) ++local.ood_rows;
        else ++local.covered_rows;
        if (observed->run_rc != 0) {
            ++local.invocation_failures;
            ++local.contract_violations;
            ++local.wrong_answers;
        } else if (cnet_compete_eval_parse_result(observed->output, &parsed) != 0) {
            ++local.contract_violations;
            ++local.wrong_answers;
            if (expected->lane == CNET_COMPETE_LANE_OOD &&
                observed->output[0] != '\0')
                ++local.unsafe_ood_answers;
            if (contains_nonfinite_literal(observed->output))
                ++local.nonfinite_outputs;
        } else {
            correct = cnet_compete_eval_response_correct(expected, &parsed);
            if (parsed.answered) {
                ++local.answered;
                if (expected->lane != CNET_COMPETE_LANE_OOD)
                    ++local.covered_answered;
                if (correct) ++local.answered_correct;
                if (expected->lane == CNET_COMPETE_LANE_OOD)
                    ++local.unsafe_ood_answers;
            } else if (expected->lane == CNET_COMPETE_LANE_OOD) {
                ++local.ood_abstained;
            }
            if (correct) {
                ++local.exact_correct;
                ++local.lane_correct[expected->lane];
                if (expected->lane != CNET_COMPETE_LANE_OOD)
                    ++local.covered_correct;
            } else ++local.wrong_answers;
        }
        if (expected->lane == CNET_COMPETE_LANE_COMPOSE3) {
            if (observed->guard_checks == 3)
                ++local.composition_rows_with_three_guards;
            else
                ++local.unexpected_guard_rows;
            if (SIZE_MAX - local.composition_guard_checks <
                    observed->guard_checks) {
                free(latencies);
                return -1;
            }
            local.composition_guard_checks += observed->guard_checks;
        } else if (observed->guard_checks != 0)
            ++local.unexpected_guard_rows;
    }
    qsort(latencies, row_count, sizeof latencies[0], compare_latency);
    local.median_latency_ns = latencies[(row_count + 1u) / 2u - 1u];
    local.p95_latency_ns = latencies[(row_count * 95u + 99u) / 100u - 1u];
    free(latencies);
    *metrics = local;
    return 0;
}
