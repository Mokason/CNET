/*
 * End-to-end routing demonstration.
 *
 * Loads the real frozen hex_value and increment primitives, registers them, and
 * asks the planner for a route ONEHOT16 -> BINARY_MSB5. With no hand-wiring the
 * planner discovers [hex_value, increment] and route_execute runs the chain,
 * canonicalizing the handoff. Asserts the routed result equals value+1 for all
 * 16 hex digits -- i.e. "increment a hex digit", assembled and executed
 * automatically from frozen parts.
 *
 * Requires weight files from ./nn_demo. Run from the repo root (make route).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/topology.h"

#include <stdio.h>
#include <string.h>

/* Committed frozen primitive library (regenerate with `make freeze`). Guarded so
   route_demo still builds if it is absent (the audit then reports that). */
#if __has_include("../include/generated.h")
#include "../include/generated.h"
#define HAVE_COMMITTED 1
#endif

/* Read-only topology audit over the REAL committed frozen primitive library.
   Pure observability: it builds a registry and analyzes it, never invoking the
   planner, executor, or certification -- zero authority over routing. */
static int run_topology_audit(int write_json) {
#ifdef HAVE_COMMITTED
    static const char *names[] = {
        "hex_value", "increment", "combine", "split",
        "dec_value", "dec_to_symbol", "dec_full_add", "dec_swap_ab", "dec_add_unit"
    };
    size_t total = sizeof(names) / sizeof(names[0]);
    BinaryTransformNetwork bins[16];
    PrimitiveRegistry reg;
    TopoReport report;
    size_t loaded = 0, i;

    registry_init(&reg);
    for (i = 0; i < total; ++i)
        if (btn_init_committed(&bins[loaded], names[i]) == 0) {
            registry_add(&reg, &bins[loaded], names[i]);
            loaded++;
        }

    printf("Auditing the committed frozen primitive library (%lu primitives)\n\n",
           (unsigned long)loaded);
    if (topology_analyze(&reg, 0, &report) != 0) {
        fprintf(stderr, "FAIL: topology_analyze\n");
        registry_free(&reg);
        for (i = 0; i < loaded; ++i) btn_free(&bins[i]);
        return 1;
    }
    topology_print_report(&report);

    if (write_json) {
        const char *path = "artifacts/topology/registry_topology.json";
        if (topology_write_json(&report, path) == 0) printf("\nwrote %s\n", path);
        else fprintf(stderr, "WARN: could not write %s\n", path);
    }

    registry_free(&reg);
    for (i = 0; i < loaded; ++i) btn_free(&bins[i]);
    return 0;
#else
    (void)write_json;
    fprintf(stderr, "topology audit needs include/generated.h (run `make freeze`).\n");
    return 1;
#endif
}

static Port P(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static int bits_to_int(const double *values, size_t n) {
    int value = 0;
    size_t i;

    for (i = 0; i < n; ++i) {
        value = (value << 1) | (values[i] >= 0.5 ? 1 : 0);
    }
    return value;
}

int main(int argc, char **argv) {
    static const char digits[] = "0123456789ABCDEF";
    int do_topology = 0, topology_json = 0, a;

    for (a = 1; a < argc; ++a) {
        if (strcmp(argv[a], "--topology") == 0) do_topology = 1;
        else if (strcmp(argv[a], "--topology-json") == 0) { do_topology = 1; topology_json = 1; }
    }
    if (do_topology) return run_topology_audit(topology_json);
    BinaryTransformNetwork hexval = {0};
    BinaryTransformNetwork incr = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    int failures = 0;
    int i;
    size_t s;

    if (btn_load(&hexval, "hex_value_weights.txt") != 0 ||
        btn_load(&incr, "increment_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load frozen primitives (run ./nn_demo "
                        "first).\n");
        return 1;
    }

    /* Restore accumulated reliability evidence; -1 just means no history
       yet (first run, or nn_demo retrained and invalidated the sidecars). */
    (void)btn_load_stats(&hexval, "hex_value_stats.txt");
    (void)btn_load_stats(&incr, "increment_stats.txt");

    registry_init(&reg);
    registry_add(&reg, &hexval, "hex_value");
    registry_add(&reg, &incr, "increment");

    if (route_plan(&reg, P(PORT_ONEHOT, 16, 1),
                   P(PORT_BINARY_MSB, 5, 1), &plan) != 0) {
        fprintf(stderr, "FAIL: planner found no route.\n");
        registry_free(&reg);
        btn_free(&hexval);
        btn_free(&incr);
        return 1;
    }

    printf("discovered route ONEHOT16 -> BINARY_MSB5 (%lu hops):",
           (unsigned long)plan.length);
    for (s = 0; s < plan.length; ++s) {
        printf(" %s", plan.names[s]);
    }
    printf("\n\nexecuting routed chain (no hand-wiring):\n");

    for (i = 0; i < 16; ++i) {
        double input[16] = {0};
        double output[5] = {0};
        int got;
        int expected = i + 1;

        input[i] = 1.0; /* one-hot for the hex digit whose value is i */
        if (route_execute(&plan, input, 16, output, 5) != 0) {
            fprintf(stderr, "FAIL: route_execute failed for '%c'\n", digits[i]);
            ++failures;
            continue;
        }
        got = bits_to_int(output, 5);
        printf("  %c -> [routed] -> %2d | expected %2d %s\n",
               digits[i], got, expected, got == expected ? "OK" : "<-- MISMATCH");
        if (got != expected) {
            ++failures;
        }
    }

    /* Checkpoint the evidence: repeated runs keep accumulating. */
    printf("\nreliability evidence (lifetime): hex_value %lu/%lu, "
           "increment %lu/%lu (successes/failures)\n",
           hexval.output_successes, hexval.output_failures,
           incr.output_successes, incr.output_failures);
    if (btn_save_stats(&hexval, "hex_value_stats.txt") != 0 ||
        btn_save_stats(&incr, "increment_stats.txt") != 0) {
        fprintf(stderr, "WARN: could not checkpoint reliability stats.\n");
    }

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);

    if (failures == 0) {
        printf("\nROUTE PASS: the planner discovered AND executed "
               "hex_value->increment with no hand-wiring.\n");
        return 0;
    }
    printf("\nROUTE FAIL: %d/16 mismatched.\n", failures);
    return 1;
}
