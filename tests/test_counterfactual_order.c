/* Counterfactual ORDER_ONLY ranker gate.
 * make counterfactual_order → CF_ORDER_PASS
 */
#include <stdio.h>
#include <string.h>

#include "../include/counterfactual_order.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    CnetCfOrderCandidate c[4];
    size_t order[4], n = 0;

    printf("== counterfactual ORDER_ONLY ==\n");

    memset(c, 0, sizeof c);
    c[0].name = "low";  c[0].reliability = 0.5; c[0].cf_score = 0.99; c[0].certified = 1;
    c[1].name = "high"; c[1].reliability = 0.9; c[1].cf_score = 0.10; c[1].certified = 1;
    c[2].name = "unc";  c[2].reliability = 0.99; c[2].cf_score = 1.0; c[2].certified = 0;
    c[3].name = "tieB"; c[3].reliability = 0.8; c[3].cf_score = 0.2; c[3].certified = 1;

    check(cnet_cf_order_rank(c, 4, 0, order, &n) == 0 && n == 3,
          "uncertified excluded; 3 certified ranked");
    check(order[0] == 1, "reliability primary (high first) without CF");
    check(strcmp(c[order[0]].name, "high") == 0, "winner is high-reliability");

    /* equal reliability: CF secondary */
    c[0].reliability = 0.8; c[0].cf_score = 0.9;
    c[3].reliability = 0.8; c[3].cf_score = 0.2;
    c[1].reliability = 0.5;
    check(cnet_cf_order_rank(c, 4, 1, order, &n) == 0 && n == 3,
          "CF-enabled rank");
    check(order[0] == 0 && strcmp(c[order[0]].name, "low") == 0,
          "CF breaks reliability tie (higher cf wins)");

    /* without CF, name-stable among equal reliability */
    check(cnet_cf_order_rank(c, 4, 0, order, &n) == 0, "rank without CF");
    check(order[0] == 0 || order[0] == 3, "equal-rel first is one of the ties");
    /* name asc: "low" < "tieB" so low first */
    check(order[0] == 0, "name-stable tie-break without CF (low before tieB)");

    printf("CF_ORDER_PASS checks=%d\n", checks);
    return failures ? 1 : 0;
}
