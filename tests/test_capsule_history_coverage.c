#define main original_capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include "cnet_capsule_core.h"
int main(void) {
    char root[] = "/tmp/cnet-history-coverage-XXXXXX", pack[256], error[160];
    if (!mkdtemp(root)) return 2;
    snprintf(pack, sizeof pack, "%s/pack", root);
    CnetBase b; HybridAi h; CnetCapsuleReport report;
    cnb_init(&b); hybrid_ai_init(&h);
    check(!build_unit(&b, &h, "history_fixture"), "fixture builds");
    /* The shared fixture seals all eight labels but gates five; create a truly
     * sampled contract here so input six has no sealed label at all. */
    BinaryTransformNetwork btn = {0}; Contract ct = {0};
    check(!cnb_get_unit(&b, "history_fixture", &btn, &ct), "materialize fixture");
    ct.exemplar_count = COV_ROWS;
    cnb_free(&b); cnb_init(&b);
    check(!cnb_add_unit(&b, &btn, &ct, NULL), "seal only five labels");
    contract_free(&ct); btn_free(&btn);
    h.coverage[0].n_rows = 1;
    check(!cnet_capsule_export(&b, &h, "history_fixture", pack, &report), "narrow coverage exports");
    CnetCapsuleCore *core = cnet_capsule_core_open(root, error, sizeof error);
    check(core != NULL, "narrow guard remains valid");
#ifndef HISTORY_COVERAGE_RED_BASELINE
    size_t obligations = 0;
    check(core && !cnet_capsule_core_validate_growth(core, core, &obligations) && obligations == 2,
          "history uses only the one covered row, replayed in both inventories");
#endif
    cnet_capsule_core_close(core);
    oh(h.coverage[0].rows, 6); /* contract labels only cover 0..4 */
    check(!cnet_capsule_export(&b, &h, "history_fixture", pack, &report), "legacy wider guard exports");
    core = cnet_capsule_core_open(root, error, sizeof error);
    check(core == NULL, "core refuses a guard row without a sealed label");
    cnet_capsule_core_close(core);
    cnb_free(&b); hybrid_ai_free(&h); rm_pack(pack); rmdir(root);
    printf("CAPSULE_HISTORY_COVERAGE_%s failures=%d\n", failures ? "RED" : "PASS", failures);
    return failures != 0;
}
