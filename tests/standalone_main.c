/*
 * Entry point for the individual per-suite test targets.
 *
 * tests/test_all.c is the unified runner and owns main() there, so the
 * per-suite sources expose only run_test_<suite>() and carry no main of their
 * own. That left every individual target in the Makefile failing to link with
 * "undefined reference to `main'". Linking this shim alongside one suite gives
 * it an entry point without adding a second main to the unified build.
 *
 * The target selects the suite by defining CNET_TEST_ENTRY, e.g.
 *
 *     $(CC) ... -DCNET_TEST_ENTRY=run_test_contract \
 *         $(SRC) $(CONTRACT_TEST) tests/standalone_main.c
 *
 * run_test_<suite>() returns 0 when the suite passes; the non-zero value is a
 * failure count in some suites and a flag in others, so it is normalised here
 * rather than returned raw, which would alias counts above 125 onto the
 * shell's reserved exit statuses.
 */

#ifndef CNET_TEST_ENTRY
#error "define CNET_TEST_ENTRY to the run_test_<suite> symbol to link against"
#endif

int CNET_TEST_ENTRY(void);

int main(void) { return CNET_TEST_ENTRY() == 0 ? 0 : 1; }
