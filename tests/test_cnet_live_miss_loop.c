#include "cnet_live_miss.h"
#include "cnet_core_serve.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
static void check(int ok, const char *n) {
    checks++;
    printf("  %-56s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

int main(void) {
    const char *miss = "/tmp/cnet_live_miss_unit.jsonl";
    char tag[32];
    unsigned in_n = 0, out_n = 0;
    float lut[16];
    char doms[4][CNET_LIVE_DOM_NAME];
    int i, n;

    unlink(miss);
    printf("== live miss typed unit ==\n");
    check(cnet_live_parse_tag_n("foo 3", tag, sizeof tag, &in_n) == 0 &&
              strcmp(tag, "foo") == 0 && in_n == 3,
          "parse TAG n");
    check(cnet_live_parse_teach("teach foo 3 9", tag, sizeof tag, &in_n, &out_n) == 0 &&
              in_n == 3 && out_n == 9,
          "parse teach");
    check(cnet_live_parse_teach("bar 2 = 7", tag, sizeof tag, &in_n, &out_n) == 0 &&
              strcmp(tag, "bar") == 0 && out_n == 7,
          "parse TAG n = m");
    for (i = 0; i < 16; ++i)
        check(cnet_live_miss_append(miss, "domx", (unsigned)i, 1, (unsigned)((i + 3) & 15)) == 0,
              i == 0 ? "append pairs" : "append");
    /* only first check name matters; rest still count */
    check(cnet_live_miss_domain_pairs(miss, "domx", lut) == 16, "domain 16/16");
    check((unsigned)(lut[0] + 0.5f) == 3 && (unsigned)(lut[1] + 0.5f) == 4,
          "lut values");
    n = cnet_live_miss_complete_domains(miss, doms, 4);
    check(n == 1 && strcmp(doms[0], "domx") == 0, "complete_domains");
    printf("CNET_LIVE_MISS_UNIT checks=%d fails=%d\n", checks, fails);
    return fails ? 1 : 0;
}
