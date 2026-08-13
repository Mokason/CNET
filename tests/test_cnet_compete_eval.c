#include "cnet_compete_eval.h"
#include "cnet_compete_artifacts.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_reordered_manifest(const char *source,
                                    const char *destination) {
    char first[2048], second[2048], line[2048];
    FILE *input = NULL, *output = NULL;
    int rc = -1;
    input = fopen(source, "rb");
    output = fopen(destination, "wb");
    if (input == NULL || output == NULL || fgets(first, sizeof first, input) == NULL ||
        fgets(second, sizeof second, input) == NULL ||
        fputs(second, output) == EOF || fputs(first, output) == EOF)
        goto done;
    while (fgets(line, sizeof line, input) != NULL)
        if (fputs(line, output) == EOF) goto done;
    if (ferror(input) || fflush(output) != 0) goto done;
    rc = 0;
done:
    if (output != NULL && fclose(output) != 0) rc = -1;
    if (input != NULL && fclose(input) != 0) rc = -1;
    return rc;
}

static int replace_journal_byte(const char *path, const char *line_prefix,
                                char from, char to) {
    FILE *file = fopen(path, "r+b");
    char line[2048];
    long offset = -1;
    if (file == NULL) return -1;
    while (fgets(line, sizeof line, file) != NULL) {
        char *match;
        long after = ftell(file);
        if (strncmp(line, line_prefix, strlen(line_prefix)) != 0) continue;
        match = strchr(line + strlen(line_prefix), from);
        if (match != NULL)
            offset = after - (long)strlen(line) + (long)(match - line);
        break;
    }
    if (offset < 0 || fseek(file, offset, SEEK_SET) != 0 ||
        fputc(to, file) == EOF || fflush(file) != 0 || fsync(fileno(file)) != 0 ||
        fclose(file) != 0)
        return -1;
    return 0;
}

static int truncate_before_last_record(const char *path, char *tail,
                                       size_t tail_capacity) {
    FILE *file = fopen(path, "r+b");
    char line[20000];
    off_t previous = -1, current = -1;
    if (file == NULL || tail == NULL || tail_capacity == 0) return -1;
    while (fgets(line, sizeof line, file) != NULL) {
        previous = current;
        current = (off_t)ftell(file);
    }
    if (ferror(file) || previous < 0 || current <= previous ||
        strlen(line) >= tail_capacity ||
        ftruncate(fileno(file), previous) != 0 || fflush(file) != 0 ||
        fsync(fileno(file)) != 0 || fclose(file) != 0)
        return -1;
    memcpy(tail, line, strlen(line) + 1u);
    return 0;
}

int main(void) {
    static const char *const invalid[] = {
        "",
        "```json\n{\"status\":\"abstain\"}\n```",
        "{\"status\":\"abstain\"} prose",
        "{\"status\":\"abstain\",\"extra\":1}",
        "{\"status\":\"abstain\",\"status\":\"abstain\"}",
        "{\"status\":\"answer\",\"intent\":\"crc8_atm\"}",
        "{\"status\":\"answer\",\"intent\":\"crc8_atm\",\"value\":1.0}",
        "{\"status\":\"answer\",\"intent\":\"crc8_atm\",\"value\":NaN}",
        "{\"status\":\"answer\",\"intent\":\"crc8_atm\",\"value\":\"\\u1",
        "[{\"status\":\"abstain\"}]",
        "{\"status\":\"answer\",\"intent\":7,\"value\":1}"
        ,"{\"status\":\"answer\",\"intent\":\"bogus\",\"value\":1}"
        ,"{\"status\":\"answer\",\"intent\":\"increment_mod256\","
         "\"value\":\"allow\"}"
        ,"{\"status\":\"answer\",\"intent\":\"access_policy_v1\","
         "\"value\":1}"
    };
    CnetCompeteEvalFixture fixture;
    CnetCompeteParsedResult parsed;
    CnetCompeteJournalHeader header, loaded_header;
    CnetCompeteJournal journal, concurrent, refused;
    CnetCompeteEvalSnapshot snapshot;
    CnetCompeteJournalRow *loaded_rows = NULL;
    char path[] = "/tmp/cnet-eval-journal-XXXXXX";
    char unrelated_path[] = "/tmp/cnet-eval-unrelated-XXXXXX";
    char oversized_path[] = "/tmp/cnet-eval-oversized-XXXXXX";
    char manifest_path[] = "/tmp/cnet-eval-manifest-XXXXXX";
    char content[256], escaped[256], hex[512], decoded[256], error[256];
    char manifest_sha[65];
    unsigned long long manifest_bytes = 0, capsule_bytes = 0;
    size_t loaded_count = 0, index;
    char removed_tail[20000];
    int descriptor = -1, unrelated_descriptor = -1;
    int oversized_descriptor = -1, manifest_descriptor = -1, rc = 1;

#define REQUIRE(condition, reason)                                            \
    do {                                                                      \
        if (!(condition)) {                                                   \
            printf("CNET_7B_EVAL_RED reason=%s\n", reason);                 \
            goto done;                                                        \
        }                                                                     \
    } while (0)

    memset(&fixture, 0, sizeof fixture);
    memset(&journal, 0, sizeof journal);
    memset(&concurrent, 0, sizeof concurrent);
    memset(&refused, 0, sizeof refused);
    memset(&snapshot, 0, sizeof snapshot);
    fixture.rows = (CnetCompeteEvalRow *)calloc(2, sizeof fixture.rows[0]);
    REQUIRE(fixture.rows != NULL, "fixture_allocation");
    fixture.count = 2;
    snprintf(fixture.rows[0].id, sizeof fixture.rows[0].id, "covered_001");
    fixture.rows[0].lane = CNET_COMPETE_LANE_INCREMENT;
    snprintf(fixture.rows[0].intent, sizeof fixture.rows[0].intent,
             "increment_mod256");
    fixture.rows[0].value_kind = CNET_EVAL_VALUE_INTEGER;
    fixture.rows[0].expected_integer = 13;
    snprintf(fixture.rows[1].id, sizeof fixture.rows[1].id, "ood_001");
    fixture.rows[1].lane = CNET_COMPETE_LANE_OOD;
    snprintf(fixture.rows[1].intent, sizeof fixture.rows[1].intent, "none");
    fixture.rows[1].value_kind = CNET_EVAL_VALUE_NONE;

    REQUIRE(cnet_compete_eval_parse_result(
                " { \"value\" : 13, \"intent\" : \"increment_mod256\", "
                "\"status\" : \"answer\" } ", &parsed) == 0 &&
                parsed.answered && parsed.value_kind == CNET_EVAL_VALUE_INTEGER &&
                cnet_compete_eval_response_correct(&fixture.rows[0], &parsed),
            "integer_result_parse");
    REQUIRE(cnet_compete_eval_parse_result(
                "{\"status\":\"answer\",\"intent\":\"access_policy_v1\","
                "\"value\":\"allow\"}", &parsed) == 0 &&
                parsed.answered && parsed.value_kind == CNET_EVAL_VALUE_STRING &&
                strcmp(parsed.string, "allow") == 0,
            "string_result_parse");
    REQUIRE(cnet_compete_eval_parse_result(
                "\n{\"status\":\"abstain\"}\t", &parsed) == 0 &&
                !parsed.answered &&
                cnet_compete_eval_response_correct(&fixture.rows[1], &parsed),
            "abstain_result_parse");
    for (index = 0; index < sizeof invalid / sizeof invalid[0]; ++index)
        REQUIRE(cnet_compete_eval_parse_result(invalid[index], &parsed) != 0,
                "invalid_result_accepted");

    REQUIRE(cnet_compete_eval_extract_chat_content(
                "{\"id\":\"x\",\"model\":\"model.gguf\","
                "\"choices\":[{\"finish_reason\":\"stop\","
                "\"message\":{\"role\":\"assistant\",\"content\":"
                "\"{\\\"status\\\":\\\"abstain\\\"}\"}}]}",
                "model.gguf", content, sizeof content) == 0 &&
                strcmp(content, "{\"status\":\"abstain\"}") == 0,
            "chat_content_extract");
    REQUIRE(cnet_compete_eval_extract_chat_content(
                "{\"model\":\"model.gguf\",\"choices\":["
                "{\"finish_reason\":\"stop\",\"message\":{"
                "\"role\":\"assistant\",\"content\":\"first\"}},"
                "{\"finish_reason\":\"stop\",\"message\":{"
                "\"role\":\"assistant\",\"content\":\"second\"}}]}",
                "model.gguf", content, sizeof content) != 0,
            "multiple_choice_envelope_accepted");
    REQUIRE(cnet_compete_eval_extract_chat_content(
                "{\"model\":\"other.gguf\",\"choices\":[{"
                "\"finish_reason\":\"stop\",\"message\":{"
                "\"role\":\"assistant\",\"content\":\"ok\"}}]}",
                "model.gguf", content, sizeof content) != 0,
            "mismatched_response_model_accepted");
    REQUIRE(cnet_compete_eval_extract_chat_content(
                "{\"model\":\"model.gguf\",\"choices\":[{"
                "\"finish_reason\":\"length\",\"message\":{"
                "\"role\":\"assistant\",\"content\":\"ok\"}}]}",
                "model.gguf", content, sizeof content) != 0,
            "truncated_finish_reason_accepted");
    REQUIRE(cnet_compete_eval_models_response_matches(
                "{\"data\":[{\"id\":\"model.gguf\",\"meta\":{"
                "\"n_params\":8188548096,\"ftype\":\"Q1_0\"}}]}",
                "model.gguf", 8188548096ULL, "Q1_0") &&
                !cnet_compete_eval_models_response_matches(
                    "{\"data\":[{\"id\":\"other.gguf\",\"meta\":{"
                    "\"n_params\":1,\"ftype\":\"Q1_0\"}},{"
                    "\"id\":\"model.gguf\",\"meta\":{"
                    "\"n_params\":8188548096,\"ftype\":\"Q1_0\"}}]}",
                    "model.gguf", 8188548096ULL, "Q1_0"),
            "models_response_object_binding");
    REQUIRE(cnet_compete_eval_json_escape("line\n\"quoted\"\\tail", escaped,
                                          sizeof escaped) == 0 &&
                strcmp(escaped, "line\\n\\\"quoted\\\"\\\\tail") == 0,
            "json_escape");
    REQUIRE(cnet_compete_eval_hex_encode(content, hex, sizeof hex) == 0 &&
                cnet_compete_eval_hex_decode(hex, decoded, sizeof decoded) == 0 &&
                strcmp(content, decoded) == 0,
            "hex_roundtrip");
    REQUIRE(cnet_compete_eval_verify_frozen(error, sizeof error) == 0,
            "frozen_digest_verification");
    REQUIRE(cnet_compete_eval_snapshot_create(&snapshot, 1,
                                              error, sizeof error) == 0 &&
                snapshot.artifact_bytes > snapshot.capsule_bytes &&
                snapshot.capsule_bytes ==
                    CNET_COMPETE_CAPSULE_ARTIFACT_BYTES &&
                cnet_compete_eval_snapshot_verify(&snapshot, error,
                                                   sizeof error) == 0,
            "verified_private_snapshot");
    {
        char unexpected[CNET_COMPETE_EVAL_PATH_MAX];
        FILE *extra;
        REQUIRE(snprintf(unexpected, sizeof unexpected, "%s/unexpected",
                         snapshot.artifact_root) > 0,
                "unexpected_artifact_path");
        extra = fopen(unexpected, "wb");
        REQUIRE(extra != NULL && fputs("x", extra) >= 0 &&
                    fclose(extra) == 0 &&
                    cnet_compete_eval_snapshot_verify(
                        &snapshot, error, sizeof error) != 0 &&
                    unlink(unexpected) == 0 &&
                    cnet_compete_eval_snapshot_verify(
                        &snapshot, error, sizeof error) == 0,
                "undeclared_artifact_not_refused");
    }
    cnet_compete_eval_snapshot_destroy(&snapshot);
    REQUIRE(cnet_compete_eval_git_identity_valid(
                "0123456789abcdef0123456789abcdef01234567",
                "89abcdef0123456789abcdef0123456789abcdef") &&
                !cnet_compete_eval_git_identity_valid("unknown", "unknown"),
            "git_identity_validation");
    REQUIRE(cnet_compete_artifact_paths_are_canonical(
                CNET_COMPETE_ARTIFACT_ROOT "/intent.wlm",
                CNET_COMPETE_ARTIFACT_ROOT "/intent.meta",
                CNET_COMPETE_ARTIFACT_ROOT "/capsules",
                CNET_COMPETE_ARTIFACT_ROOT "/artifacts.sha256") &&
                !cnet_compete_artifact_paths_are_canonical(
                    "/tmp/tuned.wlm",
                    CNET_COMPETE_ARTIFACT_ROOT "/intent.meta",
                    CNET_COMPETE_ARTIFACT_ROOT "/capsules",
                    CNET_COMPETE_ARTIFACT_ROOT "/artifacts.sha256"),
            "runtime_artifact_path_binding");
    REQUIRE(cnet_compete_eval_verify_artifact_manifest(
                CNET_COMPETE_ARTIFACT_ROOT "/artifacts.sha256",
                &manifest_bytes, &capsule_bytes, manifest_sha,
                error, sizeof error) == 0 &&
                manifest_bytes > capsule_bytes &&
                capsule_bytes == CNET_COMPETE_CAPSULE_ARTIFACT_BYTES,
            "canonical_artifact_manifest");
    manifest_descriptor = mkstemp(manifest_path);
    REQUIRE(manifest_descriptor >= 0 && close(manifest_descriptor) == 0,
            "manifest_path");
    manifest_descriptor = -1;
    REQUIRE(write_reordered_manifest(
                CNET_COMPETE_ARTIFACT_ROOT "/artifacts.sha256",
                manifest_path) == 0,
            "manifest_reorder_fixture");
    REQUIRE(cnet_compete_eval_verify_artifact_manifest(
                manifest_path, &manifest_bytes, &capsule_bytes, manifest_sha,
                error, sizeof error) != 0,
            "reordered_artifact_manifest_accepted");

    memset(&header, 0, sizeof header);
    snprintf(header.backend, sizeof header.backend, "unit_test");
    snprintf(header.identity, sizeof header.identity, "identity_1");
    header.parameters = 10;
    header.model_bytes = 20;
    header.artifact_bytes = 30;
    unrelated_descriptor = mkstemp(unrelated_path);
    REQUIRE(unrelated_descriptor >= 0 &&
                write(unrelated_descriptor, "secret\npartial", 14) == 14 &&
                fsync(unrelated_descriptor) == 0 &&
                close(unrelated_descriptor) == 0,
            "unrelated_file_setup");
    unrelated_descriptor = -1;
    REQUIRE(cnet_compete_journal_open(&refused, unrelated_path, &fixture,
                                      &header, error, sizeof error) != 0,
            "unrelated_file_accepted_as_journal");
    {
        char preserved[15] = {0};
        FILE *unrelated = fopen(unrelated_path, "rb");
        REQUIRE(unrelated != NULL &&
                    fread(preserved, 1, 14, unrelated) == 14 &&
                    fgetc(unrelated) == EOF && fclose(unrelated) == 0 &&
                    memcmp(preserved, "secret\npartial", 14) == 0,
                "unrelated_file_mutated");
    }
    oversized_descriptor = mkstemp(oversized_path);
    REQUIRE(oversized_descriptor >= 0 &&
                write(oversized_descriptor, "CNET_ASI5_RESULTS 2\n", 20) ==
                    20 &&
                ftruncate(oversized_descriptor,
                          (off_t)(16u * 1024u * 1024u + 1u)) == 0 &&
                fsync(oversized_descriptor) == 0 &&
                close(oversized_descriptor) == 0,
            "oversized_journal_setup");
    oversized_descriptor = -1;
    REQUIRE(cnet_compete_journal_open(&refused, oversized_path, &fixture,
                                      &header, error, sizeof error) != 0,
            "oversized_journal_accepted");
    {
        struct stat status;
        REQUIRE(lstat(oversized_path, &status) == 0 &&
                    status.st_size == (off_t)(16u * 1024u * 1024u + 1u),
                "oversized_journal_mutated");
    }
    descriptor = mkstemp(path);
    REQUIRE(descriptor >= 0 && close(descriptor) == 0 && unlink(path) == 0,
            "journal_path");
    descriptor = -1;
    {
        char orphan_anchor[sizeof path + 7u];
        FILE *orphan;
        REQUIRE(snprintf(orphan_anchor, sizeof orphan_anchor, "%s.anchor",
                         path) > 0,
                "orphan_anchor_path");
        orphan = fopen(orphan_anchor, "wb");
        REQUIRE(orphan != NULL && fputs("orphan\n", orphan) >= 0 &&
                    fclose(orphan) == 0 &&
                    cnet_compete_journal_open(&refused, path, &fixture,
                                              &header, error,
                                              sizeof error) != 0 &&
                    unlink(orphan_anchor) == 0,
                "orphan_anchor_reset_accepted");
    }
    memset(&journal, 0, sizeof journal);
    REQUIRE(cnet_compete_journal_open(&journal, path, &fixture, &header,
                                      error, sizeof error) == 0 &&
                journal.completed_rows == 0 && !journal.complete,
            "journal_create");
    cnet_compete_journal_close(&journal);
    {
        char anchor_path[sizeof path + 7u];
        REQUIRE(snprintf(anchor_path, sizeof anchor_path, "%s.anchor",
                         path) > 0 && unlink(anchor_path) == 0 &&
                    cnet_compete_journal_open(
                        &journal, path, &fixture, &header, error,
                        sizeof error) == 0 &&
                    journal.completed_rows == 0 && !journal.complete,
                "header_only_crash_not_resumable");
    }
    REQUIRE(cnet_compete_journal_open(&concurrent, path, &fixture, &header,
                                      error, sizeof error) != 0,
            "concurrent_journal_not_locked");
    REQUIRE(cnet_compete_journal_append(
                &journal, &fixture.rows[0], 101, 0, 0,
                "{\"status\":\"answer\",\"intent\":\"increment_mod256\","
                "\"value\":13}") != 0,
            "result_without_issue_accepted");
    REQUIRE(cnet_compete_journal_issue(&journal, &fixture.rows[0]) == 0 &&
                fputs("partial-result", journal.file) >= 0 &&
                fflush(journal.file) == 0 && fsync(fileno(journal.file)) == 0,
            "journal_partial_result_setup");
    cnet_compete_journal_close(&journal);
    REQUIRE(cnet_compete_journal_open(&journal, path, &fixture, &header,
                                      error, sizeof error) == 0 &&
                journal.completed_rows == 1 &&
                journal.interrupted_rows == 1 && !journal.complete,
            "journal_interrupted_row_not_accounted");
    REQUIRE(cnet_compete_journal_issue(&journal, &fixture.rows[1]) == 0 &&
                cnet_compete_journal_append(
                &journal, &fixture.rows[1], 202, 0, 0,
                "{\"status\":\"abstain\"}") == 0 &&
                cnet_compete_journal_finish(&journal, fixture.count) == 0,
            "journal_finish");
    cnet_compete_journal_close(&journal);
    REQUIRE(cnet_compete_journal_read(path, &fixture, &loaded_header,
                                      &loaded_rows, &loaded_count,
                                      error, sizeof error) == 0 &&
                loaded_count == 2 && header.parameters == loaded_header.parameters &&
                loaded_rows[0].run_rc != 0 && loaded_rows[0].latency_ns == 0 &&
                strcmp(loaded_rows[1].output,
                       "{\"status\":\"abstain\"}") == 0,
            "journal_read");
    free(loaded_rows);
    loaded_rows = NULL;
    loaded_count = 0;
    {
        FILE *corrupt = fopen(path, "r+b");
        char line[1024];
        long offset = -1;
        REQUIRE(corrupt != NULL, "noncanonical_header_open");
        while (fgets(line, sizeof line, corrupt) != NULL) {
            long after = ftell(corrupt);
            if (strcmp(line, "parameters 10\n") == 0) {
                offset = after - 3;
                break;
            }
        }
        REQUIRE(offset >= 0 && fseek(corrupt, offset, SEEK_SET) == 0 &&
                    fputc('0', corrupt) != EOF && fclose(corrupt) == 0 &&
                    cnet_compete_journal_read(
                        path, &fixture, &loaded_header, &loaded_rows,
                        &loaded_count, error, sizeof error) != 0,
                "noncanonical_header_number_accepted");
    }
    {
        FILE *restore = fopen(path, "r+b");
        char line[1024];
        long offset = -1;
        REQUIRE(restore != NULL, "noncanonical_header_restore_open");
        while (fgets(line, sizeof line, restore) != NULL) {
            long after = ftell(restore);
            if (strcmp(line, "parameters 00\n") == 0) {
                offset = after - 3;
                break;
            }
        }
        REQUIRE(offset >= 0 && fseek(restore, offset, SEEK_SET) == 0 &&
                    fputc('1', restore) != EOF && fclose(restore) == 0,
                "noncanonical_header_restore");
    }
    REQUIRE(replace_journal_byte(path, "ood_001\t", '2', '3') == 0 &&
                cnet_compete_journal_read(
                    path, &fixture, &loaded_header, &loaded_rows,
                    &loaded_count, error, sizeof error) != 0,
            "journal_record_corruption_accepted");
    REQUIRE(replace_journal_byte(path, "ood_001\t", '3', '2') == 0,
            "journal_record_restore");
    REQUIRE(truncate_before_last_record(path, removed_tail,
                                        sizeof removed_tail) == 0 &&
                cnet_compete_journal_open(&refused, path, &fixture, &header,
                                          error, sizeof error) != 0,
            "valid_prefix_truncation_accepted");
    {
        FILE *restore = fopen(path, "ab");
        REQUIRE(restore != NULL && fputs(removed_tail, restore) >= 0 &&
                    fflush(restore) == 0 && fsync(fileno(restore)) == 0 &&
                    fclose(restore) == 0,
                "valid_prefix_restore");
    }
    {
        FILE *corrupt = fopen(path, "r+b");
        char line[1024];
        long offset = -1;
        REQUIRE(corrupt != NULL, "nul_header_open");
        while (fgets(line, sizeof line, corrupt) != NULL) {
            long after = ftell(corrupt);
            if (strncmp(line, "identity ", 9) == 0) {
                offset = after - (long)strlen(line) + 9;
                break;
            }
        }
        REQUIRE(offset >= 0 && fseek(corrupt, offset, SEEK_SET) == 0 &&
                    fputc('\0', corrupt) != EOF && fclose(corrupt) == 0 &&
                    cnet_compete_journal_read(
                        path, &fixture, &loaded_header, &loaded_rows,
                        &loaded_count, error, sizeof error) != 0,
                "embedded_nul_header_accepted");
    }
    printf("CNET_7B_EVAL_CONTRACT_PASS json_invalid_refused=%zu "
           "journal_rows=%zu frozen_digests=3\n",
           sizeof invalid / sizeof invalid[0], fixture.count);
    rc = 0;
done:
    if (descriptor >= 0) (void)close(descriptor);
    if (unrelated_descriptor >= 0) (void)close(unrelated_descriptor);
    if (oversized_descriptor >= 0) (void)close(oversized_descriptor);
    if (manifest_descriptor >= 0) (void)close(manifest_descriptor);
    cnet_compete_journal_close(&journal);
    free(loaded_rows);
    cnet_compete_eval_free_fixture(&fixture);
    cnet_compete_eval_snapshot_destroy(&snapshot);
    (void)unlink(path);
    {
        char lock_path[sizeof path + 5u];
        if (snprintf(lock_path, sizeof lock_path, "%s.lock", path) > 0)
            (void)unlink(lock_path);
    }
    {
        char anchor_path[sizeof path + 7u];
        if (snprintf(anchor_path, sizeof anchor_path, "%s.anchor", path) > 0)
            (void)unlink(anchor_path);
    }
    (void)unlink(unrelated_path);
    {
        char lock_path[sizeof unrelated_path + 5u];
        if (snprintf(lock_path, sizeof lock_path, "%s.lock",
                     unrelated_path) > 0)
            (void)unlink(lock_path);
    }
    (void)unlink(oversized_path);
    {
        char lock_path[sizeof oversized_path + 5u];
        if (snprintf(lock_path, sizeof lock_path, "%s.lock",
                     oversized_path) > 0)
            (void)unlink(lock_path);
    }
    (void)unlink(manifest_path);
#undef REQUIRE
    return rc;
}
