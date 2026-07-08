#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int failures = 0;

#define CHECK(cond, desc) do { \
    if (cond) { printf("  ok   %s\n", (desc)); } \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static void* xmalloc(size_t n) {
    void* p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    return p;
}

static char* join_path(const char* dir, const char* leaf) {
    size_t n = strlen(dir) + 1 + strlen(leaf) + 1;
    char* out = (char*)xmalloc(n);
    snprintf(out, n, "%s/%s", dir, leaf);
    return out;
}

static int write_text(const char* path, const char* text) {
    FILE* f = fopen(path, "wb");
    if (!f) {
        return 0;
    }
    fputs(text, f);
    fclose(f);
    return 1;
}

static char* read_all(const char* path) {
    FILE* f = fopen(path, "rb");
    long size;
    char* data;
    size_t got;
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    data = (char*)malloc((size_t)size + 1);
    if (!data) {
        fclose(f);
        return NULL;
    }
    got = fread(data, 1, (size_t)size, f);
    fclose(f);
    data[got] = '\0';
    return data;
}

static int count_substring(const char* text, const char* needle) {
    int count = 0;
    const char* p = text;
    while ((p = strstr(p, needle)) != NULL) {
        ++count;
        p += strlen(needle);
    }
    return count;
}

static char* build_command(const char* log_path, const char* training_path, const char* suggestions_path) {
    const char* prefix = "./bin/register_compression_improvements --log ";
    const char* mid = " --training-data ";
    const char* tail = " --suggestions ";
    size_t n = strlen(prefix) + strlen(log_path) + strlen(mid) + strlen(training_path) +
               strlen(tail) + strlen(suggestions_path) + 1;
    char* command = (char*)xmalloc(n);
    snprintf(command, n, "%s%s%s%s%s%s", prefix, log_path, mid, training_path, tail, suggestions_path);
    return command;
}

int main(void) {
    char dir_template[] = "/tmp/cnet_phase5_c_test_XXXXXX";
    char* dir = mkdtemp(dir_template);
    char* log_path;
    char* training_path;
    char* suggestions_dir;
    char* suggestions_path;
    char* command;
    char* suggestions;
    char* training;
    const char* source = "cnet_model_compression_phase_log";

    printf("phase 5 C integration bridge tests:\n");
    CHECK(dir != NULL, "temporary directory created");
    if (!dir) {
        return 1;
    }

    log_path = join_path(dir, "implementation_log.jsonl");
    training_path = join_path(dir, "training_data.jsonl");
    suggestions_dir = join_path(dir, "suggestions");
    suggestions_path = join_path(suggestions_dir, "cnet_compression_suggestions.jsonl");
    mkdir(suggestions_dir, 0777);

    CHECK(write_text(log_path,
        "{\"date\":\"2026-07-06\",\"phase\":\"phase1_counterfactual_routing\",\"status\":\"implemented_initial_slice\",\"artifacts\":[\"include/cce/cce_router.h\"],\"notes\":\"Added deterministic counterfactual route sampling.\"}\n"
        "{\"date\":\"2026-07-06\",\"phase\":\"Phase 4\",\"summary\":\"Added uncertainty-aware activation scoring.\",\"tests\":[\"make phase4_uncertainty_test\"],\"pending\":[\"calibrate thresholds\"]}\n"),
        "sample implementation log written");
    CHECK(write_text(training_path,
        "{\"instruction\":\"preserve me\",\"response\":\"ok\"}\n"
        "{\"instruction\":\"old generated row\",\"response\":\"replace me\",\"metadata\":{\"source\":\"cnet_model_compression_phase_log\"}}\n"),
        "sample training data written");

    command = build_command(log_path, training_path, suggestions_path);
    CHECK(system(command) == 0, "first C bridge export succeeds");
    CHECK(system(command) == 0, "second C bridge export is idempotent for generated rows");

    suggestions = read_all(suggestions_path);
    training = read_all(training_path);
    CHECK(suggestions != NULL, "suggestions output written");
    CHECK(training != NULL, "training output written");
    if (suggestions) {
        CHECK(strstr(suggestions, "cnet-compression-phase-1") != NULL,
              "phase 1 suggestion exported");
        CHECK(strstr(suggestions, "cnet-compression-phase-4") != NULL,
              "phase 4 suggestion exported");
    }
    if (training) {
        CHECK(strstr(training, "preserve me") != NULL,
              "unrelated training row preserved");
        CHECK(strstr(training, "old generated row") == NULL,
              "previous generated row replaced");
        CHECK(count_substring(training, source) == 2,
              "exactly two generated training rows remain after repeated export");
    }

    free(command);
    free(suggestions);
    free(training);
    free(log_path);
    free(training_path);
    free(suggestions_path);
    free(suggestions_dir);

    if (failures == 0) {
        printf("\nAll phase 5 C integration bridge tests passed.\n");
        return 0;
    }
    printf("\n%d phase 5 test(s) FAILED.\n", failures);
    return 1;
}
