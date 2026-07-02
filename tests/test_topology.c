/* test_topology — exact checks for the contract-graph topology audit.
 *
 * Known graph: a triangle T1->T2->T3->T1 (primitives A,B,C, one component with
 * exactly one cycle) plus an isolated U1->U2 primitive (Z). So:
 *   V=4, E=3, components(betti0)=2, cycle-rank(betti1)=E-V+C=3-4+2=1.
 * Bridges should be suggested to connect the U-island to the T-component.
 * A duplicate of A (A2) must then surface as a by-signature dedup candidate. */
#include <stdio.h>
#include <string.h>
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/topology.h"

/* committed frozen library, for the live smoke test (guarded) */
#if __has_include("../include/generated.h")
#include "../include/generated.h"
#define HAVE_COMMITTED 1
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; \
    printf("FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)

static Port P(const char *tag) {
    Port p; memset(&p, 0, sizeof p);
    p.family = PORT_ONEHOT; p.field_width = 1; p.field_count = 4;
    port_set_tag(&p, tag);
    return p;
}

static void mk(BinaryTransformNetwork *b, Port in, Port out) {
    btn_init(b, 4, 4, 4, 8, 0.1, 1u);
    btn_set_ports(b, in, out);
}

static int has_consume_sig_substr(const TopoReport *r, const char *needle) {
    for (size_t i = 0; i < r->suggestion_count; i++)
        if (strstr(r->suggestions[i].consume_sig, needle)) return 1;
    return 0;
}

int main(void) {
    Port T1 = P("t1"), T2 = P("t2"), T3 = P("t3"), U1 = P("u1"), U2 = P("u2");

    BinaryTransformNetwork A, B, C, Z, A2;
    mk(&A, T1, T2);   /* T1 -> T2 */
    mk(&B, T2, T3);   /* T2 -> T3 */
    mk(&C, T3, T1);   /* T3 -> T1  (closes the triangle) */
    mk(&Z, U1, U2);   /* U1 -> U2  (isolated island) */

    PrimitiveRegistry reg; registry_init(&reg);
    registry_add(&reg, &A, "A");
    registry_add(&reg, &B, "B");
    registry_add(&reg, &C, "C");
    registry_add(&reg, &Z, "Z");

    TopoReport r;
    CHECK(topology_analyze(&reg, 0, &r) == 0, "analyze ok");
    CHECK(r.node_count == 4, "V == 4");
    CHECK(r.edge_count == 3, "E == 3 (triangle)");
    CHECK(r.betti0 == 2, "betti0 == 2 (triangle + island)");
    CHECK(r.betti1 == 1, "betti1 == 1 (one cycle: E-V+C = 3-4+2)");
    CHECK(r.largest_component_size == 3, "largest component == 3 (the triangle)");

    /* A,B,C share a component; Z is alone. */
    CHECK(r.node_component[0] == r.node_component[1] &&
          r.node_component[1] == r.node_component[2], "A,B,C same component");
    CHECK(r.node_component[3] != r.node_component[0], "Z in its own component");

    /* "what to mint next": a bridge consuming the island's output (U2) to wire it
       into the T-component must be suggested. */
    CHECK(r.suggestion_count > 0, "at least one bridge suggested");
    CHECK(has_consume_sig_substr(&r, "/u2"), "a bridge consumes U2 (joins the island)");

    topology_print_report(&r);

    /* certified_only excludes uncertified entries (none are certified here). */
    TopoReport rc;
    CHECK(topology_analyze(&reg, 1, &rc) == 0, "analyze certified-only ok");
    CHECK(rc.node_count == 0, "certified_only: no nodes (none certified)");

    /* dedup: A2 has the identical T1->T2 signature as A. */
    mk(&A2, T1, T2);
    registry_add(&reg, &A2, "A2");
    TopoReport r2;
    CHECK(topology_analyze(&reg, 0, &r2) == 0, "re-analyze ok");
    int found_dup = 0;
    for (size_t i = 0; i < r2.dedup_count; i++)
        if ((strcmp(r2.dedup[i].name_a, "A") == 0 && strcmp(r2.dedup[i].name_b, "A2") == 0) ||
            (strcmp(r2.dedup[i].name_a, "A2") == 0 && strcmp(r2.dedup[i].name_b, "A") == 0))
            found_dup = 1;
    CHECK(found_dup, "dedup candidate A == A2 found");

    registry_free(&reg);
    btn_free(&A); btn_free(&B); btn_free(&C); btn_free(&Z); btn_free(&A2);

    /* ---- smoke test on the LIVE committed frozen primitive library ---- */
#ifdef HAVE_COMMITTED
    {
        static const char *names[] = {
            "hex_value", "increment", "combine", "split",
            "dec_value", "dec_to_symbol", "dec_full_add", "dec_swap_ab", "dec_add_unit"
        };
        size_t total = sizeof(names) / sizeof(names[0]);
        BinaryTransformNetwork bins[16]; PrimitiveRegistry live; TopoReport lr;
        size_t loaded = 0;
        registry_init(&live);
        for (size_t i = 0; i < total; i++)
            if (btn_init_committed(&bins[loaded], names[i]) == 0) {
                registry_add(&live, &bins[loaded], names[i]); loaded++;
            }
        CHECK(loaded >= 2, "loaded committed primitives");
        CHECK(topology_analyze(&live, 0, &lr) == 0, "live registry audit ok");
        CHECK(lr.node_count == loaded, "live node count matches loaded");
        CHECK(lr.betti0 >= 1 && lr.betti0 <= loaded, "live betti0 is sane");
        CHECK(lr.betti1 >= 0, "live betti1 non-negative");
        printf("  live library: V=%zu betti0=%zu betti1=%ld\n", lr.node_count, lr.betti0, lr.betti1);
        registry_free(&live);
        for (size_t i = 0; i < loaded; i++) btn_free(&bins[i]);
    }
#else
    printf("  (live smoke test skipped: include/generated.h absent)\n");
#endif

    printf("\ntest_topology: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
