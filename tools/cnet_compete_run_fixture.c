#include "cnet_compete_eval.h"
#include "cnet_compete_artifacts.h"
#include "cnet_compete_client_identity.h"
#include "cnet_compete_runtime.h"

#include <stdio.h>
#include <string.h>

#ifndef CNET_COMPETE_BUILD_COMMIT
#define CNET_COMPETE_BUILD_COMMIT "unknown"
#endif
#ifndef CNET_COMPETE_BUILD_TREE
#define CNET_COMPETE_BUILD_TREE "unknown"
#endif

int main(int argc, char **argv) {
    CnetCompeteEvalFixture fixture;
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport runtime_report;
    CnetCompeteEvalSnapshot snapshot;
    CnetCompeteJournalHeader header;
    CnetCompeteJournal journal;
    unsigned long long artifact_bytes = 0, capsule_artifact_bytes = 0;
    char artifact_sha[65], runner_sha[65], error[256] = "";
    size_t index;
    int rc = 1;
    if (argc != 6) {
        fprintf(stderr, "usage: %s MODEL META CAPSULE_ROOT ARTIFACT_MANIFEST RESULT\n",
                argv[0]);
        return 2;
    }
    if (!cnet_compete_client_release_lock_acquire()) {
        fprintf(stderr, "CNET fixture runner refused release lock\n");
        return 2;
    }
    if (!cnet_compete_artifact_paths_are_canonical(
            argv[1], argv[2], argv[3], argv[4]) ||
        strcmp(argv[5], CNET_COMPETE_CNET_RESULT_PATH) != 0) {
        fprintf(stderr, "CNET fixture runner refused noncanonical artifacts\n");
        return 2;
    }
    if (!cnet_compete_eval_git_identity_valid(CNET_COMPETE_BUILD_COMMIT,
                                              CNET_COMPETE_BUILD_TREE)) {
        fprintf(stderr, "CNET fixture runner refused unknown build identity\n");
        return 2;
    }
    memset(&fixture, 0, sizeof fixture);
    memset(&runtime_report, 0, sizeof runtime_report);
    memset(&snapshot, 0, sizeof snapshot);
    memset(&header, 0, sizeof header);
    memset(&journal, 0, sizeof journal);
    if (!cnet_compete_client_environment_matches() ||
        !cnet_compete_client_runtime_matches(CNET_COMPETE_CLIENT_FIXTURE) ||
        cnet_compete_eval_file_sha256("/proc/self/exe", runner_sha) != 0 ||
        cnet_compete_eval_snapshot_create(&snapshot, 1,
                                          error, sizeof error) != 0 ||
        cnet_compete_eval_load_fixture(snapshot.fixture, &fixture,
                                       error, sizeof error) != 0 ||
        cnet_compete_runtime_load(snapshot.model, snapshot.metadata,
                                  snapshot.capsule_root, &runtime,
                                  &runtime_report) != 0) {
        fprintf(stderr, "CNET fixture runner refused: %s\n", error);
        goto done;
    }
    artifact_bytes = snapshot.artifact_bytes;
    capsule_artifact_bytes = snapshot.capsule_bytes;
    snprintf(artifact_sha, sizeof artifact_sha, "%s",
             snapshot.artifact_sha256);
    if (strcmp(artifact_sha, CNET_COMPETE_ARTIFACT_MANIFEST_SHA256) != 0 ||
        runtime_report.imported_units != 6 ||
        runtime_report.certified_rows != 1296 ||
        runtime_report.composition_members != 3 ||
        runtime_report.base_parameters != CNET_COMPETE_BASE_PARAMETERS ||
        runtime_report.base_artifact_bytes !=
            CNET_COMPETE_BASE_ARTIFACT_BYTES ||
        runtime_report.capsule_payload_bytes !=
            CNET_COMPETE_CAPSULE_PAYLOAD_BYTES) {
        fprintf(stderr, "CNET fixture runner refused runtime evidence\n");
        goto done;
    }
    snprintf(header.backend, sizeof header.backend, "cnet_native_c");
    if (snprintf(header.identity, sizeof header.identity,
                 "artifact=%s;commit=%s;tree=%s;runner=%s;client_runtime=%s",
                 artifact_sha,
                 CNET_COMPETE_BUILD_COMMIT, CNET_COMPETE_BUILD_TREE,
                 runner_sha, CNET_COMPETE_FIXTURE_RUNTIME_SET_SHA256) >=
        (int)sizeof header.identity)
        goto done;
    header.parameters = (unsigned long long)runtime_report.base_parameters;
    header.model_bytes = runtime_report.base_artifact_bytes;
    header.artifact_bytes = artifact_bytes;
    header.capsule_payload_bytes = runtime_report.capsule_payload_bytes;
    header.capsule_artifact_bytes = capsule_artifact_bytes;
    header.imported_units = runtime_report.imported_units;
    header.certified_rows = runtime_report.certified_rows;
    header.composition_members = runtime_report.composition_members;
    header.residual_calls = 0;
    if (cnet_compete_eval_prepare_results_directory(error, sizeof error) != 0 ||
        cnet_compete_journal_open(&journal, argv[5], &fixture, &header,
                                  error, sizeof error) != 0) {
        fprintf(stderr, "CNET result journal refused: %s\n", error);
        goto done;
    }
    if (!journal.complete) {
        for (index = journal.completed_rows; index < fixture.count; ++index) {
            CnetCompeteResult result;
            char json[192] = "";
            uint64_t start, finish;
            if (!cnet_compete_client_environment_matches() ||
                !cnet_compete_client_runtime_shape_matches(
                    CNET_COMPETE_CLIENT_FIXTURE)) {
                (void)cnet_compete_journal_fail(&journal,
                                                "client_identity_changed");
                goto done;
            }
            if (cnet_compete_journal_issue(&journal, &fixture.rows[index]) != 0)
                goto done;
            start = cnet_compete_eval_now_ns();
            int run_rc = cnet_compete_runtime_execute(runtime,
                                                       fixture.rows[index].prompt,
                                                       &result) == 0 ? 0 : 1;
            if (run_rc == 0 &&
                cnet_compete_result_json(&result, json, sizeof json) != 0)
                run_rc = 2;
            finish = cnet_compete_eval_now_ns();
            if (start == 0 || finish < start) run_rc = 3;
            if (cnet_compete_journal_append(
                    &journal, &fixture.rows[index],
                    finish >= start ? finish - start : 0,
                    run_rc, run_rc == 0 ? result.composition_guard_checks : 0,
                    run_rc == 0 ? json : "") != 0) {
                fprintf(stderr, "CNET result journal write failed at row %zu\n",
                        index);
                goto done;
            }
            if ((index + 1u) % 64u == 0 || index + 1u == fixture.count)
                fprintf(stderr, "CNET held-out progress %zu/%zu\n",
                        index + 1u, fixture.count);
        }
        if (cnet_compete_eval_snapshot_verify(&snapshot, error,
                                              sizeof error) != 0 ||
            !cnet_compete_client_environment_matches() ||
            !cnet_compete_client_runtime_matches(
                CNET_COMPETE_CLIENT_FIXTURE)) {
            (void)cnet_compete_journal_fail(&journal,
                                            "fixture_identity_changed");
            goto done;
        }
        if (cnet_compete_journal_finish(&journal, fixture.count) != 0)
            goto done;
    }
    if (cnet_compete_eval_snapshot_verify(&snapshot, error,
                                          sizeof error) != 0 ||
        !cnet_compete_client_environment_matches() ||
        !cnet_compete_client_runtime_matches(CNET_COMPETE_CLIENT_FIXTURE))
        goto done;
    printf("CNET_7B_CNET_RUN_PASS rows=%zu guard_checks=%zu "
           "artifact_bytes=%llu identity=%s\n",
           journal.completed_rows, journal.guard_checks, artifact_bytes,
           header.identity);
    rc = 0;
done:
    cnet_compete_journal_close(&journal);
    cnet_compete_runtime_free(runtime);
    cnet_compete_eval_free_fixture(&fixture);
    cnet_compete_eval_snapshot_destroy(&snapshot);
    return rc;
}
