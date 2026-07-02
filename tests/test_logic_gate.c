/*
 * tests/test_logic_gate.c -- evidence for the learnable boolean circuit.
 *
 * The keystone: each task is learned to a DISCRETE circuit that is PROVEN exact
 * over its ENTIRE 2^n truth table (lgn_certify_exhaustive -> 0 mismatches) --
 * a primitive that is verifiable the moment it is learned, with zero margin
 * because boolean logic has no rounding. Standalone via `make logicgate`, or
 * under `make test`.
 */
#include "../include/logic_gate_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;

#define CHECK(cond, desc) do {                       \
    if (cond) { printf("  ok   %s\n", (desc)); }     \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* Enumerate the full n-bit input domain (MSB-first) into inputs[2^n * n]. */
static void fill_inputs(double *inputs, size_t n) {
    size_t rows = (size_t)1 << n, r, j;
    for (r = 0; r < rows; ++r)
        for (j = 0; j < n; ++j)
            inputs[r * n + j] = (double)((r >> (n - 1 - j)) & 1);
}

int run_test_logic_gate(void);
int run_test_logic_gate(void) {
    failures = 0;
    printf("== logic gate network: a primitive born exact ==\n");

    /* ---- XOR (2 -> 1) ----------------------------------------------------- */
    {
        double in[4 * 2], tg[4 * 1]; size_t r, mm; LogicGateNetwork net;
        size_t hid[2] = {8, 8};
        fill_inputs(in, 2);
        for (r = 0; r < 4; ++r) tg[r] = (double)(((int)in[r*2] ) ^ ((int)in[r*2+1]));
        CHECK(lgn_learn_exact(&net, 2, 1, hid, 2, in, tg, 4, 2000, 0.5, 40, 1u) == 0,
              "XOR learned to an EXACT boolean circuit");
        lgn_certify_exhaustive(&net, in, tg, 4, &mm);
        CHECK(mm == 0, "XOR PROVEN: 0 mismatches over all 4 canonical inputs");
        {   double x[2] = {1,0}, o[1]; lgn_eval(&net, x, o);
            CHECK(o[0] == 1.0, "discrete eval exact: 1 XOR 0 = 1"); }
        {   double x[2] = {1,1}, o[1]; lgn_eval(&net, x, o);
            CHECK(o[0] == 0.0, "discrete eval exact: 1 XOR 1 = 0"); }
        lgn_free(&net);
    }

    /* ---- 3-bit parity (3 -> 1) ------------------------------------------- */
    {
        double in[8 * 3], tg[8 * 1]; size_t r, mm; LogicGateNetwork net;
        size_t hid[3] = {12, 12, 12};
        fill_inputs(in, 3);
        for (r = 0; r < 8; ++r)
            tg[r] = (double)(((int)in[r*3]) ^ ((int)in[r*3+1]) ^ ((int)in[r*3+2]));
        CHECK(lgn_learn_exact(&net, 3, 1, hid, 3, in, tg, 8, 3000, 0.5, 60, 11u) == 0,
              "3-bit parity learned to an EXACT circuit");
        lgn_certify_exhaustive(&net, in, tg, 8, &mm);
        CHECK(mm == 0, "parity PROVEN over all 8 inputs");
        lgn_free(&net);
    }

    /* ---- full adder (3 -> 2): sum = a^b^cin, cout = majority -------------- */
    /* Kept alive after proving, to compose a wider adder below. */
    LogicGateNetwork fa;
    {
        double in[8 * 3], tg[8 * 2]; size_t r, mm;
        size_t hid[3] = {16, 16, 16};
        fill_inputs(in, 3);
        for (r = 0; r < 8; ++r) {
            int a = (int)in[r*3], b = (int)in[r*3+1], c = (int)in[r*3+2];
            tg[r*2 + 0] = (double)(a ^ b ^ c);                    /* sum  */
            tg[r*2 + 1] = (double)((a & b) | (a & c) | (b & c));  /* cout */
        }
        CHECK(lgn_learn_exact(&fa, 3, 2, hid, 3, in, tg, 8, 4000, 0.4, 80, 7u) == 0,
              "full adder learned to an EXACT 2-output circuit");
        lgn_certify_exhaustive(&fa, in, tg, 8, &mm);
        CHECK(mm == 0, "full adder PROVEN over all 8 inputs (sum + carry)");
        {   double x[3] = {1,1,1}, o[2]; lgn_eval(&fa, x, o);
            CHECK(o[0] == 1.0 && o[1] == 1.0, "1+1+1 -> sum=1, carry=1 (exact)"); }
        printf("  [info] full adder circuit: %zu gates\n", fa.total_gates);
    }

    /* ---- composition: two proven full-adders -> a 2-bit ripple-carry adder.
       A single flat random-wired net does NOT reliably learn the 2-bit adder --
       that is the reach limit of flat learning. CNET's answer is composition:
       chain the PROVEN primitive and the whole stays exact, for free. -------- */
    {
        size_t r, mm = 0;
        for (r = 0; r < 16; ++r) {
            int a1 = (int)((r >> 3) & 1), a0 = (int)((r >> 2) & 1);
            int b1 = (int)((r >> 1) & 1), b0 = (int)(r & 1);
            double x0[3], o0[2], x1[3], o1[2];
            int s0, c0, s1, c1, got, want;
            x0[0] = a0; x0[1] = b0; x0[2] = 0.0;          /* FA0: a0 + b0 */
            lgn_eval(&fa, x0, o0); s0 = (int)o0[0]; c0 = (int)o0[1];
            x1[0] = a1; x1[1] = b1; x1[2] = (double)c0;   /* FA1: a1 + b1 + carry */
            lgn_eval(&fa, x1, o1); s1 = (int)o1[0]; c1 = (int)o1[1];
            got  = (c1 << 2) | (s1 << 1) | s0;
            want = (((a1 << 1) | a0) + ((b1 << 1) | b0));
            if (got != want) ++mm;
        }
        CHECK(mm == 0,
              "2-bit adder COMPOSED from two proven full-adders is exact over all 16 inputs (composition extends reach losslessly)");
        printf("  [info] flat learning tops out at the full adder; the 2-bit adder comes from COMPOSITION\n");

        /* ---- benchmark: exact discrete inference throughput -------------- */
        {
            const int iters = 200000; int it; double x[3] = {1,1,1}, o[2];
            volatile double sink = 0.0;
            clock_t t0 = clock();
            for (it = 0; it < iters; ++it) { lgn_eval(&fa, x, o); sink += o[0]; }
            clock_t t1 = clock();
            double secs = (double)(t1 - t0) / CLOCKS_PER_SEC;
            printf("  [bench] discrete circuit inference: %.0f evals/sec (%zu gates), sink=%.0f\n",
                   (secs > 0 ? iters / secs : 0.0), fa.total_gates, sink);
            CHECK(secs >= 0.0, "discrete inference benchmark ran");
        }
        lgn_free(&fa);
    }

    printf("== logic gate tests done: %d failure(s) ==\n", failures);
    return failures;
}

#ifndef TEST_ALL
int main(void) { return run_test_logic_gate() == 0 ? 0 : 1; }
#endif
