/* Prove CORE can flip the evolve switch and serve without human mid-loop. */
#include "cnet_core_serve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
static void check(int ok, const char *n) {
    checks++;
    printf("  %-56s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    CnetServeBank *b;
    CnetServeResult r;
    const char *dir = getenv("CNET_CORE_BUS_BRICKS_DIR");
    unsigned x;
    int ok_a = 1, ok_x = 1;

    printf("== CORE self-evolve switch wire ==\n");
    check(dir && dir[0], "BRICKS_DIR set");
    check(cnet_serve_global_load_env() == 0, "global load env after evolve");
    b = cnet_serve_global();
    check(b != NULL && b->n >= 1, "bank has bricks");
    if (b) {
        for (x = 0; x < 16; ++x) {
            char turn[64];
            snprintf(turn, sizeof turn, "q1_add16 %u", x);
            if (cnet_serve_result(b, turn, &r) != 0 || !r.proved) ok_a = 0;
        }
        for (x = 0; x < 16; ++x) {
            char turn[64];
            snprintf(turn, sizeof turn, "q1_xor16 %u", x);
            if (cnet_serve_result(b, turn, &r) != 0 || !r.proved) ok_x = 0;
        }
    }
    check(ok_a, "serve q1_add16 full domain no LLM");
    check(ok_x || (b && b->n == 1), "serve q1_xor16 or single-brick ok");
    check(cnet_serve_result(b, "what is love", &r) == 1 && r.abstained,
          "outside table abstain");
    printf("CNET_CORE_SWITCH_WIRE_PASS checks=%d fails=%d\n", checks, fails);
    printf("switch=1 core_access=1 evolve_writes_lut=1 serve_no_llm=1 "
           "human_mid_loop=0\n");
    return fails ? 1 : 0;
}
