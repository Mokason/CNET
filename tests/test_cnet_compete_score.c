#include "cnet_compete_score.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    CnetCompeteEvalFixture fixture;
    CnetCompeteJournalRow rows[5];
    CnetCompeteMetrics metrics;
    int rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_SCORE_RED reason=%s\n", reason);                \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    memset(&fixture, 0, sizeof fixture);
    memset(rows, 0, sizeof rows);
    fixture.count = 5;
    fixture.rows = (CnetCompeteEvalRow *)calloc(fixture.count,
                                                sizeof fixture.rows[0]);
    REQUIRE(fixture.rows != NULL, "fixture_allocation");
    snprintf(fixture.rows[0].id, sizeof fixture.rows[0].id, "c1");
    fixture.rows[0].lane = CNET_COMPETE_LANE_INCREMENT;
    snprintf(fixture.rows[0].intent, sizeof fixture.rows[0].intent,
             "increment_mod256");
    fixture.rows[0].value_kind = CNET_EVAL_VALUE_INTEGER;
    fixture.rows[0].expected_integer = 2;
    snprintf(fixture.rows[1].id, sizeof fixture.rows[1].id, "c2");
    fixture.rows[1] = fixture.rows[0];
    snprintf(fixture.rows[1].id, sizeof fixture.rows[1].id, "c2");
    snprintf(fixture.rows[2].id, sizeof fixture.rows[2].id, "o1");
    fixture.rows[2].lane = CNET_COMPETE_LANE_OOD;
    snprintf(fixture.rows[3].id, sizeof fixture.rows[3].id, "o2");
    fixture.rows[3].lane = CNET_COMPETE_LANE_OOD;
    snprintf(fixture.rows[4].id, sizeof fixture.rows[4].id, "c3");
    fixture.rows[4].lane = CNET_COMPETE_LANE_COMPOSE3;
    snprintf(fixture.rows[4].intent, sizeof fixture.rows[4].intent,
             "compose3_mod256");
    fixture.rows[4].value_kind = CNET_EVAL_VALUE_INTEGER;
    fixture.rows[4].expected_integer = 7;
    snprintf(rows[0].id, sizeof rows[0].id, "c1");
    snprintf(rows[0].output, sizeof rows[0].output,
             "{\"status\":\"answer\",\"intent\":\"increment_mod256\","
             "\"value\":2}");
    rows[0].latency_ns = 40;
    snprintf(rows[1].id, sizeof rows[1].id, "c2");
    snprintf(rows[1].output, sizeof rows[1].output,
             "{\"status\":\"abstain\"}");
    rows[1].latency_ns = 20;
    snprintf(rows[2].id, sizeof rows[2].id, "o1");
    snprintf(rows[2].output, sizeof rows[2].output,
             "{\"status\":\"abstain\"}");
    rows[2].latency_ns = 10;
    snprintf(rows[3].id, sizeof rows[3].id, "o2");
    snprintf(rows[3].output, sizeof rows[3].output,
             "{\"status\":\"answer\",\"intent\":\"increment_mod256\","
             "\"value\":2}");
    rows[3].latency_ns = 30;
    snprintf(rows[4].id, sizeof rows[4].id, "c3");
    snprintf(rows[4].output, sizeof rows[4].output,
             "{\"status\":\"answer\",\"intent\":\"compose3_mod256\","
             "\"value\":7}");
    rows[4].latency_ns = 50;
    rows[4].guard_checks = 3;
    REQUIRE(cnet_compete_score_rows(&fixture, rows, 5, &metrics) == 0,
            "score_failed");
    REQUIRE(metrics.exact_correct == 3 && metrics.covered_correct == 2 &&
                metrics.covered_answered == 2 && metrics.ood_abstained == 1 &&
                metrics.unsafe_ood_answers == 1 && metrics.answered == 3 &&
                metrics.answered_correct == 2 && metrics.wrong_answers == 2 &&
                metrics.composition_rows_with_three_guards == 1 &&
                metrics.unexpected_guard_rows == 0,
            "metric_counts");
    REQUIRE(metrics.median_latency_ns == 30 && metrics.p95_latency_ns == 50,
            "latency_quantiles");
    rows[4].guard_checks = 2;
    REQUIRE(cnet_compete_score_rows(&fixture, rows, 5, &metrics) == 0 &&
                metrics.composition_rows_with_three_guards == 0 &&
                metrics.unexpected_guard_rows == 1,
            "per_row_guard_accounting");
    rows[4].guard_checks = 3;
    snprintf(rows[0].output, sizeof rows[0].output,
             "{\"status\":\"answer\",\"intent\":\"increment_mod256\","
             "\"value\":NaN}");
    REQUIRE(cnet_compete_score_rows(&fixture, rows, 5, &metrics) == 0 &&
                metrics.nonfinite_outputs == 1 &&
                metrics.contract_violations == 1 &&
                metrics.wrong_answers == 3,
            "nonfinite_accounting");
    snprintf(rows[0].output, sizeof rows[0].output,
             "{\"status\":\"answer\",\"intent\":\"increment_mod256\","
             "\"value\":1e9999}");
    REQUIRE(cnet_compete_score_rows(&fixture, rows, 5, &metrics) == 0 &&
                metrics.nonfinite_outputs == 1 &&
                metrics.wrong_answers == 3,
            "numeric_overflow_accounting");
    snprintf(rows[0].output, sizeof rows[0].output,
             "{\"status\":\"answer\",\"intent\":\"increment_mod256\","
             "\"value\":2}");
    snprintf(rows[3].output, sizeof rows[3].output, "malformed answer prose");
    REQUIRE(cnet_compete_score_rows(&fixture, rows, 5, &metrics) == 0 &&
                metrics.unsafe_ood_answers == 1 &&
                metrics.contract_violations == 1,
            "malformed_ood_unsafe_accounting");
    snprintf(rows[3].output, sizeof rows[3].output,
             "{\"status\":\"answer\",\"intent\":\"increment_mod256\","
             "\"value\":2}");
    rows[0].run_rc = 1;
    REQUIRE(cnet_compete_score_rows(&fixture, rows, 5, &metrics) == 0 &&
                metrics.invocation_failures == 1 &&
                metrics.contract_violations == 1 &&
                metrics.wrong_answers == 3,
            "failure_accounting");
    printf("CNET_7B_SCORE_PASS rows=5 quantiles=2 failure_accounting=1\n");
    rc = 0;
done:
    cnet_compete_eval_free_fixture(&fixture);
#undef REQUIRE
    return rc;
}
