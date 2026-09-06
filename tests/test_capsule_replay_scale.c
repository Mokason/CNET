/* Isolate aggregate replay exhaustion without changing any budget. */
#include "../src/serve/cnet_capsule_core.c"
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    char error[160];
    CnetCapsuleCore *before = cnet_capsule_core_open(argv[1], error, sizeof error);
    CnetCapsuleCore *after = cnet_capsule_core_open_candidate(argv[1], argv[2], error, sizeof error);
    if (!before || !after) { fprintf(stderr, "%s\n", error); return 2; }
    CoreBudget budget; budget_init(&budget, 2000000); size_t checked = 0;
    int rc = replay_labels(before, after, &budget, &checked, NULL, 0) ||
             replay_labels(after, after, &budget, &checked, NULL, 0);
    printf("CAPSULE_REPLAY_SCALE_%s remaining=%zu obligations=%zu\n", rc ? "RED" : "PASS", budget.left, checked);
    cnet_capsule_core_close(before); cnet_capsule_core_close(after);
    return rc ? 1 : 0;
}
