/* test_topology_json — locks the load-bearing invariants of the live topology
 * audit + its JSON artifact (not every byte). Builds the committed frozen
 * library, analyzes it, writes the JSON, and re-reads it to confirm structure.
 *
 * betti0/betti1 are GOLDEN values for the CURRENT committed library (2 islands:
 * hex/byte + decimal). If the library legitimately changes (e.g. a bridge
 * primitive is minted), update these -- the change failing the test loudly is
 * the point of a golden summary check. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/topology.h"

#if __has_include("../include/generated.h")
#include "../include/generated.h"
#define HAVE_COMMITTED 1
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; printf("FAIL: %s\n", (msg)); } } while (0)

static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[1 << 16];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = 0;
    fclose(f);
    return buf;
}
static int count_sub(const char *hay, const char *needle) {
    int c = 0; size_t nl = strlen(needle);
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p += nl) c++;
    return c;
}

int main(void) {
#ifndef HAVE_COMMITTED
    printf("SKIP test_topology_json: include/generated.h absent (run `make freeze`).\n");
    return 0;
#else
    static const char *names[] = {
        "hex_value", "increment", "combine", "split",
        "dec_value", "dec_to_symbol", "dec_full_add", "dec_swap_ab", "dec_add_unit"
    };
    size_t total = sizeof(names) / sizeof(names[0]);
    BinaryTransformNetwork bins[16]; PrimitiveRegistry reg; TopoReport r;
    size_t loaded = 0;
    registry_init(&reg);
    for (size_t i = 0; i < total; i++)
        if (btn_init_committed(&bins[loaded], names[i]) == 0) {
            registry_add(&reg, &bins[loaded], names[i]); loaded++;
        }

    CHECK(topology_analyze(&reg, 0, &r) == 0, "analyze ok");
    /* report-level invariants */
    CHECK(r.node_count == 9, "primitive_count == 9");
    CHECK(r.betti0 == 2, "betti0 == 2 (hex/byte + decimal islands)");
    CHECK(r.betti1 == 5, "betti1 == 5 (golden for current committed library)");
    CHECK(r.suggestion_count > 0, "mint_candidates_count > 0");

    /* JSON-artifact invariants (parsed loosely, not byte-for-byte) */
    const char *path = "artifacts/topology/test_topology_json.json";
    CHECK(topology_write_json(&r, path) == 0, "json written");
    char *js = read_all(path);
    CHECK(js != NULL, "json readable");
    if (js) {
        CHECK(strstr(js, "\"node_count\": 9") != NULL, "json node_count == 9");
        CHECK(strstr(js, "\"betti0\": 2") != NULL, "json betti0 == 2");
        CHECK(strstr(js, "\"betti1\": 5") != NULL, "json betti1 == 5");
        CHECK(strstr(js, "\"mint_candidates\"") != NULL, "json has mint_candidates");
        CHECK(count_sub(js, "\"consume\"") > 0, "json mint_candidates non-empty");
        CHECK(strstr(js, "\"dedup_candidates\"") != NULL, "json dedup field present");
        CHECK(strstr(js, "not automatic evolution") != NULL,
              "json note states candidates are non-authoritative");
    }

    registry_free(&reg);
    for (size_t i = 0; i < loaded; i++) btn_free(&bins[i]);

    printf("\ntest_topology_json: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
#endif
}
