#include "cnet_live_miss.h"
#include "cnet_core_serve.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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

    /* Self-improve harvest: chain turns become typed_miss, not dead telemetry. */
    {
        const char *hm = "/tmp/cnet_live_miss_harvest.jsonl";
        int h;
        unlink(hm);
        h = cnet_live_miss_harvest_turn(hm, "q1_add16 3 then missing_dom 7");
        check(h >= 2, "harvest_chain_count");
        check(cnet_live_miss_domain_pairs(hm, "q1_add16", lut) >= 0, "harvest_add_domain");
        /* in-only rows: domain_pairs counts only out-known; harvest is has_out=0 */
        {
            FILE *f = fopen(hm, "r");
            char line[512];
            int saw_add = 0, saw_miss = 0, saw_typed = 0;
            check(f != NULL, "harvest_file");
            if (f) {
                while (fgets(line, sizeof line, f)) {
                    if (strstr(line, "typed_miss")) saw_typed = 1;
                    if (strstr(line, "q1_add16") && strstr(line, "\"in\":3"))
                        saw_add = 1;
                    if (strstr(line, "missing_dom") && strstr(line, "\"in\":7"))
                        saw_miss = 1;
                }
                fclose(f);
            }
            check(saw_typed && saw_add && saw_miss, "harvest_typed_rows");
        }
        h = cnet_live_miss_harvest_turn(hm, "teach growdom 4 9");
        check(h >= 1, "harvest_teach");
        check(cnet_live_miss_domain_pairs(hm, "growdom", lut) == 1 &&
                  (unsigned)(lut[4] + 0.5f) == 9,
              "harvest_teach_out");
        unlink("/tmp/cnet_live_goals");
        check(cnet_live_miss_queue_goal("/tmp/cnet_live_goals", "q1_xor16", 3) ==
                      0 ||
                  /* mkdir may be needed */
                  (mkdir("/tmp/cnet_live_goals", 0755) == 0 &&
                   cnet_live_miss_queue_goal("/tmp/cnet_live_goals", "q1_xor16",
                                             3) == 0),
              "queue_goal");
        {
            FILE *gf = fopen("/tmp/cnet_live_goals/pending_goals.txt", "r");
            char gl[128];
            int okg = 0;
            if (gf && fgets(gl, sizeof gl, gf))
                okg = strstr(gl, "prove q1_xor16 at 3") != NULL;
            if (gf) fclose(gf);
            check(okg, "queue_goal_text");
        }
    }

    printf("CNET_LIVE_MISS_UNIT checks=%d fails=%d\n", checks, fails);
    printf("self_improve_harvest=1 typed_not_dead_end=1 residual_auto_cert=0\n");
    return fails ? 1 : 0;
}
