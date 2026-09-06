/* CORE_SERVE_AUTHORITY_RED: raw tables and partial inputs cannot certify. */
#include "cnet_core_serve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
int main(void) {
    char dir[] = "/tmp/cnet-lut-authority-XXXXXX", path[256];
    CnetServeBank bank;
    CnetServeResult result;
    int failed = 0;
    const char *bad[] = {"", "1,2", "nan", "inf", "-1", "16", "1.5"};
    if (!mkdtemp(dir)) return 2;
    snprintf(path, sizeof path, "%s/audit.lut", dir);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        FILE *f = fopen(path, "w");
        if (!f) return 2;
        fprintf(f, "tag=audit\nlut=%s\n", bad[i]); fclose(f);
        cnet_serve_bank_load_dir(&bank, dir);
        if (bank.n) { printf("accepted malformed table %zu\n", i); failed++; }
    }
    float table[16];
    for (unsigned i = 0; i < 16; i++) table[i] = (float)i;
    if (cnet_serve_save_lut(dir, "audit", "raw_table", table) != 0) return 2;
    cnet_serve_bank_load_dir(&bank, dir);
    cnet_serve_result(&bank, "audit 3", &result);
    if (result.claimed_cert || result.proved) { puts("raw table claimed proof"); failed++; }
    const char *queries[] = {"audit -3", "audit 3.5", "audit 3 trailing", "audit words 3", "audit 16"};
    for (size_t i = 0; i < sizeof queries / sizeof queries[0]; i++) {
        if (cnet_serve_result(&bank, queries[i], &result) == 0 || !result.abstained) {
            printf("accepted partial input %s\n", queries[i]); failed++;
        }
    }
    remove(path); rmdir(dir);
    printf("CORE_SERVE_AUTHORITY_%s failures=%d\n", failed ? "RED" : "PASS", failed);
    return failed ? 1 : 0;
}
