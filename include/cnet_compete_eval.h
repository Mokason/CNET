#ifndef CNET_COMPETE_EVAL_H
#define CNET_COMPETE_EVAL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cnet_compete.h"

#define CNET_COMPETE_EVAL_ID_MAX 64
#define CNET_COMPETE_EVAL_PROMPT_MAX 1024
#define CNET_COMPETE_EVAL_OUTPUT_MAX 8192
#define CNET_COMPETE_EVAL_BACKEND_MAX 48
#define CNET_COMPETE_EVAL_IDENTITY_MAX 768
#define CNET_COMPETE_EVAL_PATH_MAX 1024

#define CNET_COMPETE_BASELINE_MODEL \
    "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf"
#define CNET_COMPETE_BASELINE_SHA256 \
    "284a335aa3fb2ced3b1b01fcb40b08aa783e3b70832767f0dd2e3fdfa134bd54"
#define CNET_COMPETE_BASELINE_PARAMETERS 8188548096ULL
#define CNET_COMPETE_BASELINE_BYTES 1158654496ULL
#define CNET_COMPETE_BASELINE_ENDPOINT \
    "http://127.0.0.1:8080/v1/chat/completions"
#define CNET_COMPETE_BASELINE_MODELS_ENDPOINT \
    "http://127.0.0.1:8080/v1/models"
#define CNET_COMPETE_BASELINE_SERVER_EXE \
    "/home/marble/AI/llama.cpp-fallback/build-rocm/bin/llama-server"
#define CNET_COMPETE_BASELINE_SERVER_SHA256 \
    "a2fdbcb7b90414238f60de6d9dde498f36e41188c49c9087ac03ab3ee4e95d93"
#define CNET_COMPETE_BASELINE_SERVER_CONFIG_SHA256 \
    "a5dfa5304722964bd9f8a11ac5cf19021c4241c29b14700f9230815f5ec56d86"
#define CNET_COMPETE_BASELINE_RUNTIME_SET_SHA256 \
    "4f31ec38c7fc57be505924451c1db034ccc4390d99bb846bac59602e43348134"
#define CNET_COMPETE_BASELINE_ENVIRONMENT_SHA256 \
    "044b9de09e4bbf3b3701c812ac3d2a90e0272dc5e8064528752e639bf91fc3f4"
#define CNET_COMPETE_BASELINE_SERVER_PID 2288
#define CNET_COMPETE_BASELINE_SERVER_START_TICKS 788ULL
#define CNET_COMPETE_BASELINE_BOOT_ID \
    "3e044f04-284f-456f-92fb-97f0829571d3"
#define CNET_COMPETE_RESULTS_DIRECTORY CNET_COMPETE_STATE_ROOT "/results"
#define CNET_COMPETE_RELEASE_BIN_DIRECTORY CNET_COMPETE_STATE_ROOT "/bin"
#define CNET_COMPETE_EVIDENCE_DIRECTORY CNET_COMPETE_STATE_ROOT "/evidence"
#define CNET_COMPETE_INPUT_DIRECTORY CNET_COMPETE_STATE_ROOT "/inputs"
#ifndef CNET_COMPETE_FROZEN_FIXTURE_PATH
#define CNET_COMPETE_FROZEN_FIXTURE_PATH \
    CNET_COMPETE_FIXTURE_PATH
#endif
#ifndef CNET_COMPETE_FROZEN_SYSTEM_PATH
#define CNET_COMPETE_FROZEN_SYSTEM_PATH \
    CNET_COMPETE_SYSTEM_PATH
#endif
#ifndef CNET_COMPETE_FROZEN_GENERATOR_PATH
#define CNET_COMPETE_FROZEN_GENERATOR_PATH \
    CNET_COMPETE_GENERATOR_PATH
#endif
#ifndef CNET_COMPETE_FROZEN_ARTIFACT_ROOT
#define CNET_COMPETE_FROZEN_ARTIFACT_ROOT CNET_COMPETE_ARTIFACT_ROOT
#endif
#define CNET_COMPETE_FIXTURE_RUNNER_PATH \
    CNET_COMPETE_RELEASE_BIN_DIRECTORY "/cnet_compete_run_fixture"
#define CNET_COMPETE_BASELINE_RUNNER_PATH \
    CNET_COMPETE_RELEASE_BIN_DIRECTORY "/cnet_compete_run_baseline"
#define CNET_COMPETE_SCORER_PATH \
    CNET_COMPETE_RELEASE_BIN_DIRECTORY "/cnet_compete_score"
#define CNET_COMPETE_BASELINE_RESULT_PATH \
    CNET_COMPETE_RESULTS_DIRECTORY "/bonsai_8b_cpu_q1_0.results"
#define CNET_COMPETE_CNET_RESULT_PATH \
    CNET_COMPETE_RESULTS_DIRECTORY "/cnet_native_c.results"
#define CNET_COMPETE_CAPSULE_EVIDENCE_PATH \
    CNET_COMPETE_EVIDENCE_DIRECTORY "/cnet_7b_capsules_san.log"
#define CNET_COMPETE_BUILD_EVIDENCE_PATH \
    CNET_COMPETE_EVIDENCE_DIRECTORY "/cnet_7b_eval_build.log"
#define CNET_COMPETE_BUILD_TOOLCHAIN_SET_SHA256 \
    "6b48233c6d4900f94ab2f0e9651757cbadbd52340bb6675990aff2906658e50f"
#define CNET_COMPETE_BUILD_SYSTEM_INPUT_SET_SHA256 \
    "ceab6860dfdde66c10cf500586bcd16ae9668f05fa493def912b8d6224182844"

typedef enum {
    CNET_EVAL_VALUE_NONE = 0,
    CNET_EVAL_VALUE_INTEGER,
    CNET_EVAL_VALUE_STRING
} CnetCompeteEvalValueKind;

typedef struct {
    char id[CNET_COMPETE_EVAL_ID_MAX];
    CnetCompeteLane lane;
    char intent[32];
    CnetCompeteEvalValueKind value_kind;
    long expected_integer;
    char expected_string[16];
    char prompt[CNET_COMPETE_EVAL_PROMPT_MAX];
} CnetCompeteEvalRow;

typedef struct {
    CnetCompeteEvalRow *rows;
    size_t count;
} CnetCompeteEvalFixture;

typedef struct {
    int answered;
    char intent[32];
    CnetCompeteEvalValueKind value_kind;
    long integer;
    char string[16];
} CnetCompeteParsedResult;

typedef struct {
    char backend[CNET_COMPETE_EVAL_BACKEND_MAX];
    char identity[CNET_COMPETE_EVAL_IDENTITY_MAX];
    unsigned long long parameters;
    unsigned long long model_bytes;
    unsigned long long artifact_bytes;
    size_t capsule_payload_bytes;
    unsigned long long capsule_artifact_bytes;
    size_t imported_units;
    size_t certified_rows;
    size_t composition_members;
    size_t residual_calls;
} CnetCompeteJournalHeader;

typedef struct {
    char id[CNET_COMPETE_EVAL_ID_MAX];
    uint64_t latency_ns;
    int run_rc;
    size_t guard_checks;
    char output[CNET_COMPETE_EVAL_OUTPUT_MAX];
} CnetCompeteJournalRow;

typedef struct {
    FILE *file;
    int lock_descriptor;
    int lock_owned;
    CnetCompeteJournalHeader header;
    size_t completed_rows;
    size_t guard_checks;
    size_t interrupted_rows;
    int complete;
    int issued;
    char issued_id[CNET_COMPETE_EVAL_ID_MAX];
    char record_sha256[65];
    size_t record_count;
    char anchor_path[CNET_COMPETE_EVAL_PATH_MAX];
} CnetCompeteJournal;

typedef struct {
    char root[CNET_COMPETE_EVAL_PATH_MAX];
    char fixture[CNET_COMPETE_EVAL_PATH_MAX];
    char system[CNET_COMPETE_EVAL_PATH_MAX];
    char generator[CNET_COMPETE_EVAL_PATH_MAX];
    char artifact_root[CNET_COMPETE_EVAL_PATH_MAX];
    char model[CNET_COMPETE_EVAL_PATH_MAX];
    char metadata[CNET_COMPETE_EVAL_PATH_MAX];
    char capsule_root[CNET_COMPETE_EVAL_PATH_MAX];
    char manifest[CNET_COMPETE_EVAL_PATH_MAX];
    unsigned long long artifact_bytes;
    unsigned long long capsule_bytes;
    char artifact_sha256[65];
    int has_artifacts;
} CnetCompeteEvalSnapshot;

int cnet_compete_eval_load_fixture(const char *path,
                                   CnetCompeteEvalFixture *fixture,
                                   char *error, size_t error_capacity);
void cnet_compete_eval_free_fixture(CnetCompeteEvalFixture *fixture);

int cnet_compete_eval_parse_result(const char *json,
                                   CnetCompeteParsedResult *result);
int cnet_compete_eval_response_correct(const CnetCompeteEvalRow *row,
                                       const CnetCompeteParsedResult *result);

int cnet_compete_eval_extract_chat_content(const char *response,
                                           const char *expected_model,
                                           char *content, size_t capacity);
int cnet_compete_eval_models_response_matches(
    const char *response, const char *expected_model,
    unsigned long long expected_parameters, const char *expected_ftype);
int cnet_compete_eval_json_escape(const char *input, char *output,
                                  size_t capacity);

int cnet_compete_eval_hex_encode(const char *input, char *output,
                                 size_t capacity);
int cnet_compete_eval_hex_decode(const char *input, char *output,
                                 size_t capacity);

int cnet_compete_eval_verify_frozen(char *error, size_t error_capacity);
int cnet_compete_eval_verify_artifact_manifest(
    const char *manifest_path, unsigned long long *total_bytes,
    unsigned long long *capsule_bytes, char manifest_sha256[65],
    char *error, size_t error_capacity);
int cnet_compete_eval_file_sha256(const char *path, char output[65]);
int cnet_compete_eval_regular_file_size(const char *path,
                                        unsigned long long *size_out);
uint64_t cnet_compete_eval_now_ns(void);
int cnet_compete_eval_git_identity_valid(const char *commit,
                                         const char *tree);
int cnet_compete_eval_prepare_results_directory(char *error,
                                                size_t error_capacity);
int cnet_compete_eval_snapshot_create(CnetCompeteEvalSnapshot *snapshot,
                                      int include_artifacts,
                                      char *error, size_t error_capacity);
int cnet_compete_eval_snapshot_verify(
    const CnetCompeteEvalSnapshot *snapshot,
    char *error, size_t error_capacity);
void cnet_compete_eval_snapshot_destroy(CnetCompeteEvalSnapshot *snapshot);

int cnet_compete_journal_open(CnetCompeteJournal *journal,
                              const char *path,
                              const CnetCompeteEvalFixture *fixture,
                              const CnetCompeteJournalHeader *header,
                              char *error, size_t error_capacity);
int cnet_compete_journal_issue(CnetCompeteJournal *journal,
                               const CnetCompeteEvalRow *row);
int cnet_compete_journal_append(CnetCompeteJournal *journal,
                                const CnetCompeteEvalRow *row,
                                uint64_t latency_ns, int run_rc,
                                size_t guard_checks, const char *output);
int cnet_compete_journal_finish(CnetCompeteJournal *journal,
                                size_t expected_rows);
int cnet_compete_journal_fail(CnetCompeteJournal *journal,
                              const char *reason);
void cnet_compete_journal_close(CnetCompeteJournal *journal);

int cnet_compete_journal_read(const char *path,
                              const CnetCompeteEvalFixture *fixture,
                              CnetCompeteJournalHeader *header,
                              CnetCompeteJournalRow **rows_out,
                              size_t *row_count_out,
                              char *error, size_t error_capacity);

#endif
