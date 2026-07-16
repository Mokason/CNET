#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "../include/agent_memory.h"

static int failures;
static int checks;

static void check(int ok, const char *message) {
    ++checks;
    if (!ok) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", message);
    }
}

static long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1L;
}

int main(void) {
    char root[] = "/tmp/cnet-agent-memory-XXXXXX";
    char work[128];
    char store[128];
    char kb[192];
    char tmp[192];
    char context[4096];
    long stable_size;

    check(mkdtemp(root) != NULL, "temporary root created");
    snprintf(work, sizeof work, "%s/work", root);
    snprintf(store, sizeof store, "%s/store", root);
    snprintf(kb, sizeof kb, "%s/%s", store, AGENT_KB_FILE);
    snprintf(tmp, sizeof tmp, "%s/%s.tmp", store, AGENT_KB_FILE);
    check(mkdir(work, 0700) == 0 && mkdir(store, 0700) == 0,
          "isolated work and durable directories created");
    check(chdir(work) == 0 && setenv("CNET_AGENT_MEMORY_DIR", store, 1) == 0,
          "memory directory override configured");

    check(agent_memory_init() == 0, "empty memory initializes");
    check(agent_record_user("durable-first") == 0,
          "first record publishes atomically");
    stable_size = file_size(kb);
    check(stable_size > 0 && access(tmp, F_OK) != 0,
          "configured KB path exists with no stale temp file");

    check(setenv("CNET_AGENT_MEMORY_FAIL_AFTER_WRITES", "1", 1) == 0,
          "failure injection enabled");
    check(agent_record_assistant("must-not-publish") != 0,
          "injected partial write is reported to caller");
    unsetenv("CNET_AGENT_MEMORY_FAIL_AFTER_WRITES");
    check(file_size(kb) == stable_size && access(tmp, F_OK) != 0,
          "failed publication preserves old KB and removes temp file");

    check(agent_reload() == 0, "published KB reopens");
    memset(context, 0, sizeof context);
    check(agent_get_chat_context(context, sizeof context, 8, 0) > 0 &&
              strstr(context, "durable-first") != NULL &&
              strstr(context, "must-not-publish") == NULL,
          "reload observes complete old generation, never failed update");

    check(agent_record_assistant("durable-second") == 0 && agent_reload() == 0,
          "subsequent successful generation publishes and reopens");
    memset(context, 0, sizeof context);
    check(agent_get_chat_context(context, sizeof context, 8, 0) > 0 &&
              strstr(context, "durable-first") != NULL &&
              strstr(context, "durable-second") != NULL,
          "reload restores the complete new generation");

    unlink(tmp);
    unlink(kb);
    rmdir(store);
    check(chdir("/") == 0, "leave temporary work directory");
    rmdir(work);
    rmdir(root);

    if (failures) {
        fprintf(stderr, "PERSISTENCE_INTEGRITY_FAIL checks=%d failures=%d\n",
                checks, failures);
        return 1;
    }
    printf("PERSISTENCE_INTEGRITY_PASS checks=%d\n", checks);
    return 0;
}
