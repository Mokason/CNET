#include "cnet_evolve_dir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    CnetEvolveDirection F;
    int fails = 0;
    FILE *f = fopen("/tmp/evdir_test.conf", "w");
    if (!f) return 1;
    fprintf(f, "allow_live_miss=1\nallow_factory=1\nallow_goals=1\n");
    fprintf(f, "max_new_per_tick=2\n");
    fprintf(f, "deny_domains=junk,tmp\n");
    fprintf(f, "prefer_domains=live_dom,user_pref\n");
    fprintf(f, "factory=mytag,0,blk.0.attn_q.weight\n");
    fprintf(f, "factory=other,1,blk.0.attn_k.weight\n");
    fprintf(f, "goal=prove mytag at 1\n");
    fclose(f);
    setenv("CNET_EVOLVE_DIRECTION", "/tmp/evdir_test.conf", 1);
    if (cnet_evolve_dir_load(&F, ".") != 0) fails++;
    if (F.n_factory != 2) { printf("nf=%d\n", F.n_factory); fails++; }
    if (F.max_new_per_tick != 2) fails++;
    if (F.n_goals != 1) fails++;
    if (cnet_evolve_dir_domain_ok(&F, "junk") != 0) fails++;
    if (cnet_evolve_dir_domain_ok(&F, "live_dom") != 1) fails++;
    if (cnet_evolve_dir_domain_ok(&F, "unknown") != 0) fails++;
    printf("loaded=%s factory=%d goals=%d prefer=%d deny=%d\n", F.loaded_from,
           F.n_factory, F.n_goals, F.n_prefer, F.n_deny);
    printf("CNET_EVOLVE_DIR_%s fails=%d\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
