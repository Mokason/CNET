/*
 * Lifecycle spine benchmark (NOT part of make test; budgeted study).
 * The spine must cost nothing when lifecycle_enabled == 0, and the RESET-skip
 * guard must be negligible when enabled. Reports ns/route_plan over a registry
 * scaled to N primitives for three policies: OFF, ON (no RESET), ON (half the
 * decoys RESET). The route is identical across policies (only unused decoys are
 * RESET), so any delta is pure guard overhead. Timings are machine-indicative.
 */
#include "../include/nn.h"
#include "../include/router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static Port P(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static int make_btn(BinaryTransformNetwork *b, size_t in, size_t out,
                    Port input_port, Port output_port) {
    if (btn_init(b, in, out, 1, 4, 0.5, 1u) != 0) return -1;
    return btn_set_ports(b, input_port, output_port);
}

/* Average ns per route_plan over `iters`. */
static double bench_plan(const PrimitiveRegistry *reg, Port in, Port goal,
                         size_t iters) {
    RoutePlan plan;
    clock_t t0, t1;
    size_t i;
    volatile long sink = 0;
    t0 = clock();
    for (i = 0; i < iters; ++i) {
        sink += route_plan(reg, in, goal, &plan);
    }
    t1 = clock();
    (void)sink;
    return (double)(t1 - t0) / (double)CLOCKS_PER_SEC * 1e9 / (double)iters;
}

int main(void) {
    const size_t sizes[] = {16, 64, 256};
    const size_t n_sizes = sizeof sizes / sizeof sizes[0];
    const size_t ITERS = 200000;
    Port in = P(PORT_ONEHOT, 16, 1);
    Port mid = P(PORT_BINARY_MSB, 4, 1);
    Port goal = P(PORT_BINARY_MSB, 5, 1);
    size_t s;

    printf("=== lifecycle spine benchmark (ns/route_plan, %zu iters) ===\n", ITERS);
    printf("%6s %14s %16s %18s\n", "N", "OFF", "ON(no reset)", "ON(50% reset)");

    for (s = 0; s < n_sizes; ++s) {
        size_t N = sizes[s];
        BinaryTransformNetwork *btns = calloc(N, sizeof *btns);
        char (*names)[16] = malloc(N * sizeof *names);
        PrimitiveRegistry reg;
        RoutePlan plan;
        double off, on0, on50;
        size_t i;
        int ok;

        if (btns == NULL || names == NULL) { printf("OOM\n"); free(btns); free(names); return 1; }

        /* prim 0: ONEHOT16 -> BINARY_MSB4 ; prim 1: BINARY_MSB4 -> BINARY_MSB5.
           prims 2..N-1: decoys ONEHOT8 -> BINARY_MSB3 (one shared output type,
           never on the route) to bulk up the entry count cheaply. */
        make_btn(&btns[0], 16, 4, in, mid);
        make_btn(&btns[1], 4, 5, mid, goal);
        for (i = 2; i < N; ++i) {
            make_btn(&btns[i], 8, 3, P(PORT_ONEHOT, 8, 1), P(PORT_BINARY_MSB, 3, 1));
        }
        registry_init(&reg);
        for (i = 0; i < N; ++i) {
            snprintf(names[i], sizeof names[i], "p%zu", i);
            registry_add(&reg, &btns[i], names[i]);
        }

        ok = (route_plan(&reg, in, goal, &plan) == 0 && plan.length == 2);

        reg.lifecycle_enabled = 0;
        off = bench_plan(&reg, in, goal, ITERS);

        reg.lifecycle_enabled = 1;
        on0 = bench_plan(&reg, in, goal, ITERS);

        for (i = 2; i < N; i += 2) registry_set_state(&reg, names[i], PRIM_RESET);
        on50 = bench_plan(&reg, in, goal, ITERS);

        /* RESET only hit unused decoys -> the route is unchanged. */
        ok = ok && (route_plan(&reg, in, goal, &plan) == 0 && plan.length == 2);

        printf("%6zu %12.1f %16.1f %18.1f   %s\n",
               N, off, on0, on50, ok ? "route ok" : "ROUTE BROKEN");

        registry_free(&reg);
        for (i = 0; i < N; ++i) btn_free(&btns[i]);
        free(btns);
        free(names);
    }
    printf("\nOFF is the legacy baseline; ON(no reset) shows the guard's branch\n");
    printf("cost; deltas should sit within timing noise (route is identical).\n");
    return 0;
}
