/* test_frozen_library — regression guard for the committed frozen primitive
 * loader. Its ONLY job: every committed primitive must load via
 * btn_init_committed (which goes through btn_init_frozen), expose valid ports
 * and nonzero dimensions, and free without crashing.
 *
 * This exists because btn_init_frozen once passed learning_rate=0.0 to btn_init,
 * which rejects lr<=0 -> EVERY committed primitive silently failed to load and
 * the whole `make freeze` / btn_init_committed path was dead. This test makes
 * that failure mode impossible to reintroduce unnoticed. */
#include <stdio.h>
#include <string.h>
#include "../include/nn.h"
#include "../include/contract/contract.h"   /* generated.h references Contract */

#if __has_include("../include/generated.h")
#include "../include/generated.h"
#define HAVE_COMMITTED 1
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(c, msg) do { if (c) g_pass++; else { g_fail++; printf("FAIL: %s\n", (msg)); } } while (0)

int main(void) {
#ifndef HAVE_COMMITTED
    printf("SKIP test_frozen_library: include/generated.h absent (run `make freeze`).\n");
    return 0;
#else
    static const char *names[] = {
        "hex_value", "increment", "combine", "split",
        "dec_value", "dec_to_symbol", "dec_full_add", "dec_swap_ab", "dec_add_unit"
    };
    size_t n = sizeof(names) / sizeof(names[0]);

    for (size_t i = 0; i < n; i++) {
        BinaryTransformNetwork b;
        char m[160];
        int rc = btn_init_committed(&b, names[i]);
        snprintf(m, sizeof m, "%s loads (btn_init_committed == 0)", names[i]);
        CHECK(rc == 0, m);
        if (rc != 0) continue;

        snprintf(m, sizeof m, "%s input_count > 0", names[i]);   CHECK(b.input_count  > 0, m);
        snprintf(m, sizeof m, "%s output_count > 0", names[i]);  CHECK(b.output_count > 0, m);
        snprintf(m, sizeof m, "%s hidden_count > 0", names[i]);  CHECK(b.hidden_count > 0, m);
        snprintf(m, sizeof m, "%s has >=1 input port", names[i]);  CHECK(b.input_port_count  >= 1, m);
        snprintf(m, sizeof m, "%s has >=1 output port", names[i]); CHECK(b.output_port_count >= 1, m);

        size_t insum = 0, outsum = 0;
        for (size_t p = 0; p < b.input_port_count; p++)
            insum += b.input_ports[p].field_width * b.input_ports[p].field_count;
        for (size_t p = 0; p < b.output_port_count; p++)
            outsum += b.output_ports[p].field_width * b.output_ports[p].field_count;
        snprintf(m, sizeof m, "%s input ports sum to input_count", names[i]);   CHECK(insum  == b.input_count,  m);
        snprintf(m, sizeof m, "%s output ports sum to output_count", names[i]);  CHECK(outsum == b.output_count, m);

        btn_free(&b);   /* must not crash */
    }

    /* the loader must be real: an unknown name must fail */
    BinaryTransformNetwork bad;
    CHECK(btn_init_committed(&bad, "does_not_exist") != 0, "unknown committed name fails");

    printf("\ntest_frozen_library: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
#endif
}
