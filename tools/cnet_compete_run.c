#include "cnet_compete_runtime.h"

#include <stdio.h>
#include <string.h>

#define PROMPT_CAPACITY 8192

static int execute_one(CnetCompeteRuntime *runtime, const char *prompt) {
    CnetCompeteResult result;
    char json[192];
    if (cnet_compete_runtime_execute(runtime, prompt, &result) != 0 ||
        cnet_compete_result_json(&result, json, sizeof json) != 0)
        return -1;
    printf("%s\n", json);
    return ferror(stdout) ? -1 : 0;
}

int main(int argc, char **argv) {
    CnetCompeteRuntime *runtime = NULL;
    CnetCompeteRuntimeReport report;
    char prompt[PROMPT_CAPACITY];
    int rc = 1;
    if (argc != 4 && argc != 5) {
        fprintf(stderr, "usage: %s MODEL META CAPSULE_ROOT [PROMPT]\n", argv[0]);
        return 2;
    }
    memset(&report, 0, sizeof report);
    if (cnet_compete_runtime_load(argv[1], argv[2], argv[3], &runtime,
                                  &report) != 0) {
        fprintf(stderr, "CNET runtime load refused\n");
        return 1;
    }
    if (argc == 5) {
        rc = execute_one(runtime, argv[4]) == 0 ? 0 : 1;
    } else {
        rc = 0;
        while (fgets(prompt, sizeof prompt, stdin) != NULL) {
            size_t length = strlen(prompt);
            if (length == 0) continue;
            if (prompt[length - 1u] == '\n') {
                prompt[--length] = '\0';
                if (length > 0 && prompt[length - 1u] == '\r')
                    prompt[--length] = '\0';
            } else if (!feof(stdin)) {
                int character;
                while ((character = fgetc(stdin)) != '\n' && character != EOF) {}
                fprintf(stderr, "prompt exceeds %d bytes\n", PROMPT_CAPACITY - 1);
                rc = 1;
                break;
            }
            if (execute_one(runtime, prompt) != 0) {
                rc = 1;
                break;
            }
        }
        if (ferror(stdin)) rc = 1;
    }
    cnet_compete_runtime_free(runtime);
    return rc;
}
