#include "cnet_shared_workspace.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main(void) {
    CnetSharedWorkspace workspace;
    CnetWorkspaceEntry entries[4];
    uint64_t digest_before;

    check(cnet_workspace_init(&workspace, 3) == 0, "workspace initializes");
    check(cnet_workspace_init(&workspace, 0) != 0, "zero capacity rejected");
    check(cnet_workspace_init(&workspace, 3) == 0, "workspace reinitializes");

    check(cnet_workspace_push(&workspace, CNET_WORKSPACE_NOTE,
          CNET_WORKSPACE_CERTIFIED, "planner", "retain first note",
          0.8, 10) == 0, "first push succeeds");
    check(cnet_workspace_push(&workspace, CNET_WORKSPACE_CANDIDATE,
          CNET_WORKSPACE_UNCERTIFIED, "semantic:hermetic",
          "candidate alpha", 0.4, 20) == 0, "second push succeeds");
    check(cnet_workspace_push(&workspace, CNET_WORKSPACE_EVIDENCE,
          CNET_WORKSPACE_PROVISIONAL, "memory", "evidence beta",
          0.7, 30) == 0, "third push succeeds");
    digest_before = cnet_workspace_digest(&workspace);
    check(digest_before != 0, "nonempty digest exists");

    check(cnet_workspace_push(&workspace, CNET_WORKSPACE_NOTE,
          CNET_WORKSPACE_CERTIFIED, "planner", "newest gamma",
          0.9, 40) == 0, "ring overwrite succeeds");
    check(cnet_workspace_recent(&workspace, entries, 4) == 3,
          "recent returns capacity");
    check(strcmp(entries[0].text_or_latent_ref, "newest gamma") == 0,
          "recent is newest first");
    check(strcmp(entries[2].text_or_latent_ref, "candidate alpha") == 0,
          "oldest retained entry is correct");
    check(cnet_workspace_query(&workspace, "BETA", entries, 4) == 1,
          "query is case insensitive");
    check(entries[0].trust == CNET_WORKSPACE_PROVISIONAL,
          "query preserves trust");
    check(cnet_workspace_query(&workspace, "retain first", entries, 4) == 0,
          "evicted entry is not queryable");
    check(cnet_workspace_digest(&workspace) != digest_before,
          "digest changes after overwrite");

    check(cnet_workspace_push(&workspace, CNET_WORKSPACE_NOTE,
          CNET_WORKSPACE_CERTIFIED, "", "invalid", 1.0, 50) != 0,
          "empty source rejected");
    cnet_workspace_clear(&workspace);
    check(workspace.count == 0 && workspace.capacity == 3,
          "clear preserves configured capacity");
    check(cnet_workspace_digest(&workspace) == UINT64_C(1469598103934665603),
          "empty digest is deterministic");

    if (failures) return 1;
    puts("SHARED_WORKSPACE_PASS metric=1.000");
    return 0;
}
