/* RED marker: CORE_BUS_UNTAGGED_RED
 *
 * A certified CORE bus brick is addressable only by its explicit domain tag.
 * Symbolic input such as "2+2" must never select the first installed brick.
 * This gate is deliberately model-free so it can run in the fast edit loop.
 */
#include "cnet_core_bus.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void fill_lut(float lut[16], unsigned delta) {
    unsigned i;
    for (i = 0; i < 16; ++i) lut[i] = (float)((i + delta) & 15u);
}

static void expect_abstain(CnetCoreBus *bus, const char *turn,
                           const char *name) {
    CnetCoreBusResult result;
    int rc = cnet_core_bus_result(bus, turn, &result);
    check(rc == 1 && result.abstained == 1 && result.proved == 0 &&
              result.claimed_cert == 0 && result.brick[0] == '\0',
          name);
}

int main(void) {
    CnetCoreBus bus;
    CnetCoreBusResult result;

    memset(&bus, 0, sizeof bus);
    bus.n_bricks = 1;
    bus.bricks[0].live = 1;
    bus.bricks[0].certified = 1;
    snprintf(bus.bricks[0].name, sizeof bus.bricks[0].name, "alpha_v1");
    snprintf(bus.bricks[0].domain_tag, sizeof bus.bricks[0].domain_tag, "alpha");
    fill_lut(bus.bricks[0].lut_table, 1);

    memset(&result, 0, sizeof result);
    check(cnet_core_bus_result(&bus, "alpha 2", &result) == 0 &&
              result.proved == 1 && result.claimed_cert == 1 &&
              strcmp(result.brick, "alpha_v1") == 0,
          "parked brick answers an explicitly addressed turn");
    expect_abstain(&bus, "2+2", "parked brick refuses symbolic unaddressed turn");
    expect_abstain(&bus, "7", "parked brick refuses bare digit turn");

    memset(&bus, 0, sizeof bus);
    bus.state = CNET_CORE_BUS_CERTIFIED;
    bus.certified = 1;
    bus.student_live = 1;
    bus.lut_live = 1;
    snprintf(bus.name, sizeof bus.name, "active_v1");
    snprintf(bus.domain_tag, sizeof bus.domain_tag, "active");
    fill_lut(bus.lut_table, 2);

    memset(&result, 0, sizeof result);
    check(cnet_core_bus_result(&bus, "active 3", &result) == 0 &&
              result.proved == 1 && result.claimed_cert == 1 &&
              strcmp(result.brick, "active_v1") == 0,
          "active brick answers an explicitly addressed turn");
    expect_abstain(&bus, "3+4", "active brick refuses symbolic unaddressed turn");
    expect_abstain(&bus, "5", "active brick refuses bare digit turn");

    if (failures) {
        printf("CORE_BUS_UNTAGGED_RED checks=%d fails=%d\n", checks, failures);
        return 1;
    }
    printf("CORE_BUS_UNTAGGED_PASS checks=%d fails=0 "
           "unaddressed_claims_cert=0 broader_claims=WITHHELD\n",
           checks);
    return 0;
}
