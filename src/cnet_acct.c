#include "../include/cnet_acct.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static CnetAcct g_acct;

void cnet_acct_reset(void) { memset(&g_acct, 0, sizeof g_acct); }

void cnet_acct_get(CnetAcct *out) {
    if (out) *out = g_acct;
}

void cnet_acct_add_tier_a(uint64_t steps) {
    g_acct.tier_a_hits++;
    g_acct.activated_steps += steps ? steps : 1;
}
void cnet_acct_add_hard(uint64_t steps) {
    g_acct.hard_expert_hits++;
    g_acct.tier_a_hits++;
    g_acct.activated_steps += steps ? steps : 1;
}
void cnet_acct_add_tier_b(void) { g_acct.tier_b_hits++; }
void cnet_acct_add_tier_c(void) { g_acct.tier_c_hits++; }
void cnet_acct_add_teacher(void) {
    g_acct.teacher_forwards++;
    g_acct.tier_c_hits++;
}
void cnet_acct_add_gap(void) { g_acct.gap_notes++; }
void cnet_acct_add_abstain(void) { g_acct.abstains++; }
void cnet_acct_add_error(void) { g_acct.errors++; }
void cnet_acct_add_adapter_pass(void) { g_acct.adapter_tick_pass++; }
void cnet_acct_add_adapter_reject(void) { g_acct.adapter_tick_reject++; }

int cnet_acct_dump(const char *path) {
    const char *p = path;
    FILE *f;
    long long ts;
    if (!p || !p[0]) p = getenv("CNET_ACCT_LOG");
    if (!p || !p[0]) p = "logs/cnet_acct.jsonl";
    f = fopen(p, "a");
    if (!f) return -1;
    ts = (long long)time(NULL);
    fprintf(f,
            "{\"ts\":%lld,\"tier_a\":%llu,\"hard\":%llu,\"tier_b\":%llu,"
            "\"tier_c\":%llu,\"teacher\":%llu,\"gaps\":%llu,\"abstain\":%llu,"
            "\"err\":%llu,\"steps\":%llu,\"adapter_pass\":%llu,"
            "\"adapter_reject\":%llu}\n",
            ts, (unsigned long long)g_acct.tier_a_hits,
            (unsigned long long)g_acct.hard_expert_hits,
            (unsigned long long)g_acct.tier_b_hits,
            (unsigned long long)g_acct.tier_c_hits,
            (unsigned long long)g_acct.teacher_forwards,
            (unsigned long long)g_acct.gap_notes,
            (unsigned long long)g_acct.abstains,
            (unsigned long long)g_acct.errors,
            (unsigned long long)g_acct.activated_steps,
            (unsigned long long)g_acct.adapter_tick_pass,
            (unsigned long long)g_acct.adapter_tick_reject);
    fclose(f);
    return 0;
}
