#define main capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#define main miner_main
#include "../tools/struct_mine_persist.c"
#undef main

int main(void) {
    char root[] = "/tmp/cnet-mine-persist-XXXXXX", cov[600];
    if (!mkdtemp(root)) return 2;
    PersonalAi *ai = calloc(1, sizeof *ai);
    CnetBase fixture, reload; HybridAi h; BinaryTransformNetwork btn = {0}; Contract ct = {0};
    cnb_init(&fixture); cnb_init(&reload); hybrid_ai_init(&h);
    check(!build_unit(&fixture, &h, "hyb_struct_valid"), "external evidence fixture");
    check(!cnb_get_unit(&fixture, "hyb_struct_valid", &btn, &ct), "materialize student");
    snprintf(ai->lane.base_path, sizeof ai->lane.base_path, "%s/base.cnb", root);
    snprintf(ai->lane.ledger_path, sizeof ai->lane.ledger_path, "%s/gaps", root);
    ai->lane.loaded = 1;
    check(persist_student(ai, &btn, "hyb_struct_valid") != 0 && !ai->lane.base.unit_count,
          "missing recorded teacher rows refuses before sealing");
    ai->hybrid = h; memset(&h, 0, sizeof h);
    check(!persist_student(ai, &btn, "hyb_struct_valid"), "persist actual owner rows and guard");
    check(!cnb_load(&reload, ai->lane.base_path) && cnb_has_unit(&reload, "hyb_struct_valid"), "disk contains exact owner");
    snprintf(cov, sizeof cov, "%s.coverage", ai->lane.base_path);
    HybridAi guard; hybrid_ai_init(&guard);
    double row[SYM]; oh(row, 2);
    check(!hybrid_coverage_load(&guard, cov) && hybrid_coverage_admits_unit(&guard, "hyb_struct_valid", row, SYM) == 1, "coverage survives restart");
    hybrid_ai_free(&guard); hybrid_ai_free(&ai->hybrid);
    cnb_free(&ai->lane.base); cnb_free(&fixture); cnb_free(&reload); contract_free(&ct); btn_free(&btn);
    unlink(cov); unlink(ai->lane.base_path); unlink(ai->lane.ledger_path); rmdir(root); free(ai);
    printf("STRUCT_MINE_PERSISTENCE_%s checks=%d failures=%d\n", failures ? "RED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
