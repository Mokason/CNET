#include "cnet_obsidian_learn.h"
#include "cnet_live_miss.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
    const char *vault = "/tmp/cnet_obsidian_test_vault";
    const char *miss = "/tmp/cnet_obsidian_test_miss.jsonl";
    const char *bricks = "/tmp/cnet_obsidian_test_bricks";
    CnetObsidianLearnReport rep;
    float lut[16];
    int pairs, fails = 0;
    char path[256];
    FILE *f;

    if (system("rm -rf /tmp/cnet_obsidian_test_vault /tmp/cnet_obsidian_test_bricks") != 0) {}
    mkdir(vault, 0755);
    mkdir(bricks, 0755);
    unlink(miss);
    snprintf(path, sizeof path, "%s/Learn Me.md", vault);
    f = fopen(path, "w");
    if (!f) return 1;
    fprintf(f, "---\ncnet_domain: ob_demo\n---\n\n# Demo\n\n");
    fprintf(f, "```cnet-domain\n");
    fprintf(f, "pairs=0:1,1:2,2:3,3:4,4:5,5:6,6:7,7:8,8:9,9:10,10:11,11:12,12:13,13:14,14:15,15:0\n");
    fprintf(f, "```\n\n");
    fprintf(f, "cnet_goal: prove ob_demo at 3\n");
    fprintf(f, "teach extra_dom 0 5\n");
    fclose(f);

    if (cnet_obsidian_learn(vault, miss, bricks, 50, &rep) != 0) fails++;
    if (rep.files_hit < 1) fails++;
    if (rep.domains_complete_emitted < 1) fails++;
    pairs = cnet_live_miss_domain_pairs(miss, "ob_demo", lut);
    if (pairs != 16) fails++;
    if ((unsigned)(lut[0] + 0.5f) != 1) fails++;
    /* goal queued */
    {
        char gp[256];
        snprintf(gp, sizeof gp, "%s/pending_goals.txt", bricks);
        if (access(gp, R_OK) != 0) fails++;
    }
    printf("vault=%s scanned=%d hit=%d teaches=%d domains=%d goals=%d pairs=%d\n",
           rep.vault, rep.files_scanned, rep.files_hit, rep.teaches,
           rep.domains_complete_emitted, rep.goals_queued, pairs);
    printf("CNET_OBSIDIAN_LEARN_%s\n", fails ? "FAIL" : "PASS"); printf("fails=%d\n", fails);
    return fails ? 1 : 0;
}
