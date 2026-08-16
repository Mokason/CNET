#include "cnet_compete_score.h"
#include "cnet_compete_artifacts.h"
#include "cnet_compete_client_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CNET_COMPETE_BUILD_COMMIT
#define CNET_COMPETE_BUILD_COMMIT "unknown"
#endif
#ifndef CNET_COMPETE_BUILD_TREE
#define CNET_COMPETE_BUILD_TREE "unknown"
#endif

static double ratio(size_t numerator, size_t denominator) {
    return denominator == 0 ? 0.0 : (double)numerator / (double)denominator;
}

static double ratio_u64(unsigned long long numerator,
                        unsigned long long denominator) {
    return denominator == 0 ? 0.0 : (double)numerator / (double)denominator;
}

static void print_metrics(const CnetCompeteJournalHeader *header,
                          const CnetCompeteMetrics *metrics) {
    static const char *const lanes[CNET_COMPETE_LANE_COUNT] = {
        "increment_mod256", "minutes_to_seconds", "crc8_atm",
        "access_policy_v1", "compose3_mod256", "ood"
    };
    int lane;
    printf("CNET_7B_METRIC backend=%s rows=%zu exact=%zu accuracy=%.9f "
           "covered=%zu/%zu covered_accuracy=%.9f covered_answered=%zu "
           "coverage=%.9f selective=%zu/%zu selective_accuracy=%.9f "
           "ood_abstained=%zu/%zu ood_accuracy=%.9f unsafe_ood=%zu "
           "contract_violations=%zu invocation_failures=%zu "
           "wrong_answers=%zu wrong_answer_rate=%.9f residual_calls=%zu "
           "nonfinite_outputs=%zu "
           "median_ms=%.6f p95_ms=%.6f\n",
           header->backend, metrics->rows, metrics->exact_correct,
           ratio(metrics->exact_correct, metrics->rows),
           metrics->covered_correct, metrics->covered_rows,
           ratio(metrics->covered_correct, metrics->covered_rows),
           metrics->covered_answered,
           ratio(metrics->covered_answered, metrics->covered_rows),
           metrics->answered_correct, metrics->answered,
           ratio(metrics->answered_correct, metrics->answered),
           metrics->ood_abstained, metrics->ood_rows,
           ratio(metrics->ood_abstained, metrics->ood_rows),
           metrics->unsafe_ood_answers, metrics->contract_violations,
           metrics->invocation_failures, metrics->wrong_answers,
           ratio(metrics->wrong_answers, metrics->rows),
           header->residual_calls, metrics->nonfinite_outputs,
           (double)metrics->median_latency_ns / 1000000.0,
           (double)metrics->p95_latency_ns / 1000000.0);
    for (lane = 0; lane < CNET_COMPETE_LANE_COUNT; ++lane)
        printf("CNET_7B_LANE backend=%s lane=%s correct=%zu rows=%zu "
               "accuracy=%.9f\n",
               header->backend, lanes[lane], metrics->lane_correct[lane],
               metrics->lane_rows[lane],
               ratio(metrics->lane_correct[lane], metrics->lane_rows[lane]));
}

static void gate(int condition, const char *name, size_t *failures) {
    if (!condition) {
        printf("CNET_7B_GATE_FAIL gate=%s\n", name);
        ++*failures;
    }
}

static int capsule_evidence_matches(const char *path) {
    char expected[512], line[512];
    FILE *file = fopen(path, "rb");
    int matches, written = snprintf(
        expected, sizeof expected,
        "CNET_7B_CAPSULES_PASS units=6 certified_rows=1296 "
        "compose_rows=256 guard_checks=768 payload_bytes=192352 "
        "corruption_refused=1 incompatibility_refused=1 "
        "artifact_sha256=%s commit=%s tree=%s\n",
        CNET_COMPETE_ARTIFACT_MANIFEST_SHA256,
        CNET_COMPETE_BUILD_COMMIT, CNET_COMPETE_BUILD_TREE);
    if (written < 0 || (size_t)written >= sizeof expected || file == NULL ||
        fgets(line, sizeof line, file) == NULL) {
        if (file != NULL) (void)fclose(file);
        return 0;
    }
    matches = strcmp(line, expected) == 0 && fgetc(file) == EOF && !ferror(file);
    if (fclose(file) != 0) matches = 0;
    return matches;
}

static int build_evidence_matches(const char *path, const char *artifact_sha,
                                  const char *cnet_runner_sha,
                                  const char *baseline_runner_sha,
                                  const char *scorer_sha) {
    char expected[2048], line[2048];
    FILE *file = fopen(path, "rb");
    int written, matches;
    written = snprintf(
        expected, sizeof expected,
        "CNET_7B_EVAL_BUILD_PASS commit=%s tree=%s artifact_sha256=%s "
        "cnet_runner_sha256=%s baseline_runner_sha256=%s scorer_sha256=%s "
        "toolchain_set_sha256=%s system_input_set_sha256=%s "
        "compiler=/usr/bin/x86_64-linux-gnu-gcc-13 "
        "compiler_sha256=1b99826121ae6682a634e5efe09bd3e3df58ce58e0b28f849114ab5b89139c26 "
        "compiler_version=13.3.0 compiler_target=x86_64-linux-gnu "
        "cpu_target=znver3\n",
        CNET_COMPETE_BUILD_COMMIT, CNET_COMPETE_BUILD_TREE, artifact_sha,
        cnet_runner_sha, baseline_runner_sha, scorer_sha,
        CNET_COMPETE_BUILD_TOOLCHAIN_SET_SHA256,
        CNET_COMPETE_BUILD_SYSTEM_INPUT_SET_SHA256);
    if (written < 0 || (size_t)written >= sizeof expected || file == NULL ||
        fgets(line, sizeof line, file) == NULL) {
        if (file != NULL) (void)fclose(file);
        return 0;
    }
    matches = strcmp(line, expected) == 0 && fgetc(file) == EOF &&
              !ferror(file);
    if (fclose(file) != 0) matches = 0;
    return matches;
}

int main(int argc, char **argv) {
    CnetCompeteEvalFixture fixture;
    CnetCompeteEvalSnapshot snapshot;
    CnetCompeteJournalHeader baseline_header, cnet_header;
    CnetCompeteJournalRow *baseline_rows = NULL, *cnet_rows = NULL;
    CnetCompeteMetrics baseline, cnet;
    unsigned long long artifact_bytes = 0, capsule_artifact_bytes = 0;
    char artifact_sha[65], cnet_runner_sha[65], baseline_runner_sha[65];
    char scorer_sha[65];
    char expected_cnet_identity[CNET_COMPETE_EVAL_IDENTITY_MAX];
    char expected_baseline_identity[CNET_COMPETE_EVAL_IDENTITY_MAX], error[256];
    size_t baseline_count = 0, cnet_count = 0, failures = 0;
    int rc = 1, verdict_emitted = 0;
    if (argc != 6) {
        fprintf(stderr, "usage: %s ARTIFACT_MANIFEST CAPSULE_EVIDENCE "
                        "BUILD_EVIDENCE BASELINE_RESULTS CNET_RESULTS\n",
                argv[0]);
        printf("CNET_7B_COMPETE_FAIL suite=%s failed_gates=1 "
               "reason=arguments broader_claims=WITHHELD\n",
               CNET_COMPETE_SUITE_ID);
        return 2;
    }
    if (!cnet_compete_client_release_lock_acquire()) {
        fprintf(stderr, "competition scorer refused release lock\n");
        printf("CNET_7B_COMPETE_FAIL suite=%s failed_gates=1 "
               "reason=release_lock broader_claims=WITHHELD\n",
               CNET_COMPETE_SUITE_ID);
        return 2;
    }
    memset(&fixture, 0, sizeof fixture);
    memset(&snapshot, 0, sizeof snapshot);
    memset(&baseline_header, 0, sizeof baseline_header);
    memset(&cnet_header, 0, sizeof cnet_header);
    memset(&baseline, 0, sizeof baseline);
    memset(&cnet, 0, sizeof cnet);
    error[0] = '\0';
    if (strcmp(argv[1], CNET_COMPETE_RELEASE_ARTIFACT_MANIFEST) != 0 ||
        strcmp(argv[2], CNET_COMPETE_CAPSULE_EVIDENCE_PATH) != 0 ||
        strcmp(argv[3], CNET_COMPETE_BUILD_EVIDENCE_PATH) != 0 ||
        strcmp(argv[4], CNET_COMPETE_BASELINE_RESULT_PATH) != 0 ||
        strcmp(argv[5], CNET_COMPETE_CNET_RESULT_PATH) != 0 ||
        !cnet_compete_eval_git_identity_valid(CNET_COMPETE_BUILD_COMMIT,
                                              CNET_COMPETE_BUILD_TREE) ||
        !cnet_compete_client_environment_matches() ||
        !cnet_compete_client_runtime_matches(CNET_COMPETE_CLIENT_SCORER) ||
        cnet_compete_eval_file_sha256(CNET_COMPETE_FIXTURE_RUNNER_PATH,
                                      cnet_runner_sha) != 0 ||
        cnet_compete_eval_file_sha256(CNET_COMPETE_BASELINE_RUNNER_PATH,
                                      baseline_runner_sha) != 0 ||
        cnet_compete_eval_file_sha256("/proc/self/exe", scorer_sha) != 0 ||
        !build_evidence_matches(argv[3],
                                CNET_COMPETE_ARTIFACT_MANIFEST_SHA256,
                                cnet_runner_sha, baseline_runner_sha,
                                scorer_sha) ||
        cnet_compete_eval_snapshot_create(&snapshot, 1,
                                          error, sizeof error) != 0 ||
        !capsule_evidence_matches(argv[2]) ||
        cnet_compete_eval_load_fixture(snapshot.fixture, &fixture,
                                       error, sizeof error) != 0 ||
        cnet_compete_journal_read(argv[4], &fixture, &baseline_header,
                                  &baseline_rows, &baseline_count,
                                  error, sizeof error) != 0 ||
        cnet_compete_journal_read(argv[5], &fixture, &cnet_header,
                                  &cnet_rows, &cnet_count,
                                  error, sizeof error) != 0 ||
        cnet_compete_score_rows(&fixture, baseline_rows, baseline_count,
                                &baseline) != 0 ||
        cnet_compete_score_rows(&fixture, cnet_rows, cnet_count, &cnet) != 0) {
        fprintf(stderr, "competition scorer refused input: %s\n", error);
        goto done;
    }
    artifact_bytes = snapshot.artifact_bytes;
    capsule_artifact_bytes = snapshot.capsule_bytes;
    snprintf(artifact_sha, sizeof artifact_sha, "%s",
             snapshot.artifact_sha256);
    if (strcmp(artifact_sha, CNET_COMPETE_ARTIFACT_MANIFEST_SHA256) != 0 ||
        snprintf(expected_cnet_identity, sizeof expected_cnet_identity,
                 "artifact=%s;commit=%s;tree=%s;runner=%s;client_runtime=%s",
                 artifact_sha,
                 CNET_COMPETE_BUILD_COMMIT, CNET_COMPETE_BUILD_TREE,
                 cnet_runner_sha,
                 CNET_COMPETE_FIXTURE_RUNTIME_SET_SHA256) >=
            (int)sizeof expected_cnet_identity ||
        snprintf(expected_baseline_identity, sizeof expected_baseline_identity,
                 "model=%s;commit=%s;tree=%s;system=%s;server=%s;config=%s;"
                 "runtime_set=%s;pid=%d;start=%llu;boot=%s;runner=%s;"
                 "client_runtime=%s",
                 CNET_COMPETE_BASELINE_SHA256, CNET_COMPETE_BUILD_COMMIT,
                 CNET_COMPETE_BUILD_TREE, CNET_COMPETE_SYSTEM_SHA256,
                 CNET_COMPETE_BASELINE_SERVER_SHA256,
                 CNET_COMPETE_BASELINE_SERVER_CONFIG_SHA256,
                 CNET_COMPETE_BASELINE_RUNTIME_SET_SHA256,
                 CNET_COMPETE_BASELINE_SERVER_PID,
                 CNET_COMPETE_BASELINE_SERVER_START_TICKS,
                 CNET_COMPETE_BASELINE_BOOT_ID, baseline_runner_sha,
                 CNET_COMPETE_BASELINE_CLIENT_RUNTIME_SET_SHA256) >=
            (int)sizeof expected_baseline_identity)
        goto done;
    printf("CNET_7B_TOOLCHAIN cnet_runner_sha256=%s "
           "baseline_runner_sha256=%s scorer_sha256=%s commit=%s tree=%s\n",
           cnet_runner_sha, baseline_runner_sha, scorer_sha,
           CNET_COMPETE_BUILD_COMMIT, CNET_COMPETE_BUILD_TREE);
    print_metrics(&baseline_header, &baseline);
    print_metrics(&cnet_header, &cnet);
    printf("CNET_7B_SIZE baseline_params=%llu cnet_base_params=%llu "
           "parameter_ratio=%.9f baseline_bytes=%llu cnet_base_bytes=%llu "
           "cnet_capsule_payload_bytes=%zu cnet_capsule_bytes=%llu "
           "cnet_artifact_bytes=%llu "
           "artifact_ratio=%.9f artifact_sha256=%s\n",
           baseline_header.parameters, cnet_header.parameters,
           ratio_u64(cnet_header.parameters, baseline_header.parameters),
           baseline_header.model_bytes, cnet_header.model_bytes,
           cnet_header.capsule_payload_bytes, capsule_artifact_bytes,
           artifact_bytes,
           ratio_u64(artifact_bytes, baseline_header.model_bytes), artifact_sha);
    printf("CNET_7B_EVIDENCE units=%zu certified_rows=%zu "
           "composition_members=%zu composition_guard_checks=%zu "
           "residual_calls=%zu corruption_refused=1 "
           "incompatibility_refused=1\n",
           cnet_header.imported_units, cnet_header.certified_rows,
           cnet_header.composition_members, cnet.composition_guard_checks,
           cnet_header.residual_calls);

    gate(strcmp(baseline_header.backend, "bonsai_8b_rocm_q1_0") == 0 &&
             strcmp(baseline_header.identity, expected_baseline_identity) == 0 &&
             baseline_header.parameters == CNET_COMPETE_BASELINE_PARAMETERS &&
             baseline_header.model_bytes == CNET_COMPETE_BASELINE_BYTES &&
             baseline_header.artifact_bytes == CNET_COMPETE_BASELINE_BYTES,
         "baseline_identity", &failures);
    gate(strcmp(cnet_header.backend, "cnet_native_c") == 0 &&
             strcmp(cnet_header.identity, expected_cnet_identity) == 0 &&
             cnet_header.parameters == CNET_COMPETE_BASE_PARAMETERS &&
             cnet_header.model_bytes == CNET_COMPETE_BASE_ARTIFACT_BYTES &&
             cnet_header.artifact_bytes == artifact_bytes &&
             cnet_header.capsule_artifact_bytes == capsule_artifact_bytes,
         "cnet_identity", &failures);
    gate(baseline.rows == CNET_COMPETE_TOTAL_ROWS &&
             cnet.rows == CNET_COMPETE_TOTAL_ROWS,
         "row_cardinality", &failures);
    gate(cnet.exact_correct >= baseline.exact_correct,
         "overall_not_below_baseline", &failures);
    gate(cnet.covered_correct >= baseline.covered_correct,
         "covered_not_below_baseline", &failures);
    gate(cnet.ood_abstained >= baseline.ood_abstained,
         "ood_not_below_baseline", &failures);
    gate(cnet.covered_answered * 100u >= cnet.covered_rows * 95u,
         "covered_answer_coverage_0_95", &failures);
    gate(cnet.answered > 0 &&
             cnet.answered_correct * 100u >= cnet.answered * 99u,
         "selective_accuracy_0_99", &failures);
    gate(cnet.unsafe_ood_answers == 0 && cnet.contract_violations == 0 &&
             cnet.invocation_failures == 0 && cnet.nonfinite_outputs == 0 &&
             cnet_header.residual_calls == 0,
         "zero_unsafe_contract_residual", &failures);
    gate(cnet_header.imported_units == 6 && cnet_header.certified_rows == 1296,
         "six_exhaustive_capsules", &failures);
    gate(cnet_header.capsule_payload_bytes ==
             CNET_COMPETE_CAPSULE_PAYLOAD_BYTES,
         "capsule_payload_identity", &failures);
    gate(capsule_artifact_bytes == CNET_COMPETE_CAPSULE_ARTIFACT_BYTES,
         "capsule_artifact_size_identity", &failures);
    gate(cnet_header.composition_members == 3 &&
             cnet.composition_rows_with_three_guards ==
                 cnet.lane_rows[CNET_COMPETE_LANE_COMPOSE3] &&
             cnet.unexpected_guard_rows == 0 &&
             cnet.composition_guard_checks ==
                 cnet.lane_rows[CNET_COMPETE_LANE_COMPOSE3] * 3u,
         "composition_per_hop_coverage", &failures);
    gate(cnet_header.parameters <= baseline_header.parameters / 100u,
         "base_parameter_ceiling", &failures);
    gate(artifact_bytes <= baseline_header.model_bytes / 100u,
         "artifact_size_ceiling", &failures);
    gate(baseline_count == fixture.count && cnet_count == fixture.count,
         "every_row_once", &failures);
    gate(cnet_compete_eval_snapshot_verify(&snapshot, error,
                                           sizeof error) == 0,
         "scoring_snapshot_unchanged", &failures);
    gate(cnet_compete_client_environment_matches() &&
             cnet_compete_client_runtime_matches(
                 CNET_COMPETE_CLIENT_SCORER) &&
             cnet_compete_eval_file_sha256("/proc/self/exe", artifact_sha) ==
                 0 &&
             strcmp(artifact_sha, scorer_sha) == 0 &&
             cnet_compete_eval_file_sha256(CNET_COMPETE_FIXTURE_RUNNER_PATH,
                                           artifact_sha) == 0 &&
             strcmp(artifact_sha, cnet_runner_sha) == 0 &&
             cnet_compete_eval_file_sha256(CNET_COMPETE_BASELINE_RUNNER_PATH,
                                           artifact_sha) == 0 &&
             strcmp(artifact_sha, baseline_runner_sha) == 0,
         "final_client_identity", &failures);

    if (failures == 0) {
        printf("CNET_7B_COMPETE_PASS suite=%s cnet_exact=%zu/%zu "
               "baseline_exact=%zu/%zu claim=bounded_suite_only "
               "broader_claims=WITHHELD\n",
               CNET_COMPETE_SUITE_ID, cnet.exact_correct, cnet.rows,
               baseline.exact_correct, baseline.rows);
        verdict_emitted = 1;
        rc = 0;
    } else {
        printf("CNET_7B_COMPETE_FAIL suite=%s failed_gates=%zu "
               "broader_claims=WITHHELD\n",
               CNET_COMPETE_SUITE_ID, failures);
        verdict_emitted = 1;
    }
done:
    if (!verdict_emitted)
        printf("CNET_7B_COMPETE_FAIL suite=%s failed_gates=1 "
               "reason=input_refused broader_claims=WITHHELD\n",
               CNET_COMPETE_SUITE_ID);
    free(cnet_rows);
    free(baseline_rows);
    cnet_compete_eval_free_fixture(&fixture);
    cnet_compete_eval_snapshot_destroy(&snapshot);
    return rc;
}
