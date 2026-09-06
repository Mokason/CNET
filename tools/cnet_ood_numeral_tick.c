/* Harvest arith numeral gaps from miss_log → propose TSV. Never CERT. */
#include "cnet_ood_skill.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *miss = NULL, *propose = NULL, *overlay = NULL;
    const char *pk;
    int i, n;
    char missbuf[1024], propbuf[1024], ovbuf[1024];

    pk = getenv("CNET_PACKS_ROOT");
    if (pk && pk[0]) {
        snprintf(missbuf, sizeof missbuf, "%s/miss_log.jsonl", pk);
        snprintf(propbuf, sizeof propbuf, "%s/en_numerals_propose.tsv", pk);
        snprintf(ovbuf, sizeof ovbuf, "%s/en_numerals.tsv", pk);
        miss = missbuf;
        propose = propbuf;
        overlay = ovbuf;
    }
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--miss") && i + 1 < argc)
            miss = argv[++i];
        else if (!strcmp(argv[i], "--propose") && i + 1 < argc)
            propose = argv[++i];
        else if (!strcmp(argv[i], "--overlay") && i + 1 < argc)
            overlay = argv[++i];
        else if (!strcmp(argv[i], "--help")) {
            fprintf(stderr,
                    "cnet_ood_numeral_tick [--miss jsonl] [--propose tsv] "
                    "[--overlay tsv]\n");
            return 0;
        }
    }
    if (!miss || !propose) {
        fprintf(stderr, "cnet_ood_numeral_tick: need --miss and --propose "
                        "(or CNET_PACKS_ROOT)\n");
        return 2;
    }
    n = cnet_ood_numeral_tick(miss, propose, overlay);
    if (n < 0) {
        printf("numeral_tick new=-1\n");
        return 1;
    }
    printf("numeral_tick new=%d propose=%s auto_cert=0\n", n, propose);
    printf("CNET_NUMERAL_TICK_PASS\n");
    return 0;
}
