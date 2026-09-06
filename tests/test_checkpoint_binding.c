#define main capsule_fixture_main
#include "test_knowledge_capsule.c"
#undef main
#include "../src/personal_ai.c"

int main(void) {
    PersonalAi *ai = calloc(1, sizeof *ai);
    check(ai != NULL, "allocate private checkpoint");
    if (!ai) return 1;
    cnb_init(&ai->lane.base); hybrid_ai_init(&ai->hybrid);
    check(!build_unit(&ai->lane.base, &ai->hybrid, "hyb_struct_valid"), "sealed fixture");
    BinaryTransformNetwork btn = {0}; Contract ct = {0};
    check(!cnb_get_unit(&ai->lane.base, "hyb_struct_valid", &btn, &ct), "materialize bounded seal");
    ct.exemplar_count = COV_ROWS;
    cnb_free(&ai->lane.base); cnb_init(&ai->lane.base);
    check(!cnb_add_unit(&ai->lane.base, &btn, &ct, NULL), "seal only recorded subset");
    btn_free(&btn); contract_free(&ct);
    check(!coverage_selfcheck(ai, 0), "matching checkpoint remains available");
    double row[SYM]; oh(row, 7);
    memcpy(ai->hybrid.coverage[0].rows, row, sizeof row);
    check(coverage_selfcheck(ai, 0) == 1, "widened sidecar rejected against older seal");
    check(hybrid_coverage_admits_unit(&ai->hybrid, "hyb_struct_valid", row, SYM) == 0,
          "mismatched guard cannot override fail-closed");
    hybrid_ai_free(&ai->hybrid); cnb_free(&ai->lane.base);
    hybrid_ai_init(&ai->hybrid); cnb_init(&ai->lane.base);
    check(!build_unit(&ai->lane.base, &ai->hybrid, "hyb_struct_valid"), "fresh sealed fixture");
    HybridCoverage valid = ai->hybrid.coverage[0];
    ai->hybrid.coverage[0] = valid;
    ai->hybrid.coverage[0].rows = malloc(COV_ROWS * SYM * sizeof(double));
    memcpy(ai->hybrid.coverage[0].rows, valid.rows, COV_ROWS * SYM * sizeof(double));
    ai->hybrid.coverage[0].targets = NULL;
    snprintf(ai->hybrid.coverage[0].unit, sizeof ai->hybrid.coverage[0].unit, "hyb_struct_stale");
    ai->hybrid.coverage[1] = valid; ai->hybrid.coverage_count = 2;
    check(!coverage_selfcheck(ai, 0) && ai->hybrid.coverage_count == 1 &&
          !strcmp(ai->hybrid.coverage[0].unit, "hyb_struct_valid"), "stale compaction preserves moved valid guard");
    /* A narrower, reordered checkpoint must remain valid. */
    if (ai->hybrid.coverage_count) {
        oh(ai->hybrid.coverage[0].rows, 4); ai->hybrid.coverage[0].n_rows = 1;
        check(!coverage_selfcheck(ai, 0), "older subset guard binds newer sealed contract");
    }
    hybrid_ai_free(&ai->hybrid); cnb_free(&ai->lane.base);
    hybrid_ai_init(&ai->hybrid);
    double *large = calloc(1024 * 10, sizeof *large);
    check(large != NULL, "large reservoir allocation");
    if (!large) return 1;
    for (size_t r = 0; r < 1024; r++)
        for (size_t b = 0; b < 10; b++) large[r * 10 + b] = (double)((r >> b) & 1);
    Contract large_ct = {0}; large_ct.inputs = large; large_ct.exemplar_count = 1024;
    for (size_t i = 0; i < 5; i++) {
        HybridCoverage *g = &ai->hybrid.coverage[i];
        g->active = 1; g->n_rows = 1024; g->in_dim = 10;
        g->rows = calloc(1024 * 10, sizeof(double));
        if (!g->rows) return 1;
        snprintf(g->unit, sizeof g->unit, "hyb_struct_large%zu", i);
        for (size_t r = 0; r < 1024; r++)
            memcpy(g->rows + r * 10, large + (1023-r) * 10, 10 * sizeof(double));
        ai->hybrid.coverage_count++;
        check(checkpoint_rows_bind(&ai->hybrid, g->unit, &large_ct, 10), "1024 reordered rows bind without quadratic budget refusal");
    }
    ai->hybrid.coverage[0].rows[1023 * 10] = -0.0;
    check(!checkpoint_rows_bind(&ai->hybrid, ai->hybrid.coverage[0].unit, &large_ct, 10),
          "signed-zero guard cannot widen exact byte membership");
    free(large); hybrid_ai_free(&ai->hybrid); free(ai);
    printf("CHECKPOINT_BINDING_%s checks=%d failures=%d\n", failures ? "RED" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
