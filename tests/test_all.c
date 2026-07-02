/*
 * Single test executable for the entire CNET test suite.
 * This replaces the previous proliferation of separate test_*.exe files.
 *
 * Build with: make test   (produces ./test_all)
 * Individual suites can still be built standalone for quick debugging
 * (e.g. make test_router still works).
 */

#include <stdio.h>

int run_test_nn(void);
int run_test_encode_oob(void);
int run_test_composition(void);
int run_test_contract(void);
int run_test_router(void);
int run_test_dag(void);
int run_test_consolidate(void);
int run_test_certify(void);
int run_test_property(void);
int run_test_decimal(void);
int run_test_circuit(void);
int run_test_library(void);
int run_test_fastpath(void);
int run_test_residue(void);
int run_test_expr(void);
int run_test_lifecycle(void);
int run_test_proposal_sidecar(void);
int run_test_below_beam_recovery(void);
int run_test_belowbeam_chars(void);
int run_test_structural_pref(void);
int run_test_structural_pref_b0(void);
int run_test_structural_pref_adversarial(void);
int run_test_structural_pref_adversarial_circuit(void);
int run_test_distillation_gate(void);
int run_test_expansion(void);
int run_test_mcp_security(void);
int run_test_coverage(void);
int run_test_conformal(void);
int run_test_logic_gate(void);
int run_test_tfidf(void);
int run_test_tiermem(void);
int run_test_graduate(void);
int run_test_fontdecode(void);
int run_test_pdf(void);
int run_test_synonyms(void);
int run_test_tileindex(void);
int run_test_tile_consolidate(void);

int main(void) {
    int total_failures = 0;

    printf("=== CNET unified test suite (single executable) ===\n\n");

    total_failures += run_test_nn();
    total_failures += run_test_encode_oob();
    total_failures += run_test_composition();
    total_failures += run_test_contract();
    total_failures += run_test_router();
    total_failures += run_test_dag();
    total_failures += run_test_consolidate();
    total_failures += run_test_certify();
    total_failures += run_test_property();
    total_failures += run_test_decimal();
    total_failures += run_test_circuit();
    total_failures += run_test_library();
    total_failures += run_test_fastpath();
    total_failures += run_test_residue();
    total_failures += run_test_expr();
    total_failures += run_test_lifecycle();
    total_failures += run_test_proposal_sidecar();
    total_failures += run_test_below_beam_recovery();
    total_failures += run_test_belowbeam_chars();
    total_failures += run_test_structural_pref();
    total_failures += run_test_structural_pref_b0();
    total_failures += run_test_structural_pref_adversarial();
    total_failures += run_test_structural_pref_adversarial_circuit();
    total_failures += run_test_distillation_gate();
    total_failures += run_test_expansion();
    total_failures += run_test_mcp_security();
    total_failures += run_test_coverage();
    total_failures += run_test_conformal();
    total_failures += run_test_logic_gate();
    total_failures += run_test_tfidf();
    total_failures += run_test_tiermem();
    total_failures += run_test_graduate();
    total_failures += run_test_fontdecode();
    total_failures += run_test_pdf();
    total_failures += run_test_synonyms();
    total_failures += run_test_tileindex();
    total_failures += run_test_tile_consolidate();

    printf("\n=====================================================\n");
    if (total_failures == 0) {
        printf("ALL TESTS PASSED (single exe)\n");
        return 0;
    } else {
        printf("%d TEST SUITE(S) FAILED\n", total_failures);
        return 1;
    }
}
