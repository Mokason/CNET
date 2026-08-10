/* Anytime-valid bake-off CLI — product wire for director head promotion.
 * Usage:
 *   cnet_av_bakeoff --wins-a N --wins-b M --n K [--alpha 0.05]
 *   cnet_av_bakeoff --sim-p 0.7 --max 5000 [--alpha 0.05]
 * Exit 0 always; prints ASI_AV_BAKEOFF_{PROMOTE|LOSE|WITHHELD|PASS}
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cnet_asi_improve.h"

static void usage(void) {
    fprintf(stderr,
            "usage:\n"
            "  %s --wins-a A --wins-b B --n N [--alpha a]\n"
            "  %s --sim-p P --max M [--alpha a]\n",
            "cnet_av_bakeoff", "cnet_av_bakeoff");
}

int main(int argc, char **argv) {
    int wa = -1, wb = -1, n = -1, max_steps = 5000, i;
    double alpha = 0.05, sim_p = -1.0;
    int dec;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wins-a") && i + 1 < argc)
            wa = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wins-b") && i + 1 < argc)
            wb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--n") && i + 1 < argc)
            n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--alpha") && i + 1 < argc)
            alpha = atof(argv[++i]);
        else if (!strcmp(argv[i], "--sim-p") && i + 1 < argc)
            sim_p = atof(argv[++i]);
        else if (!strcmp(argv[i], "--max") && i + 1 < argc)
            max_steps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage();
            return 0;
        }
    }

    if (sim_p >= 0.0) {
        wa = wb = n = 0;
        for (i = 0; i < max_steps; i++) {
            /* deterministic pseudo: i%10 < p*10 */
            if ((i % 10) < (int)(sim_p * 10.0 + 1e-9))
                wa++;
            else
                wb++;
            n++;
            dec = cnet_asi_av_compare(wa, wb, n, alpha);
            if (dec != 0) break;
        }
        printf("sim p=%.3f stop n=%d wa=%d wb=%d dec=%d label=%s\n", sim_p, n, wa, wb,
               dec, dec > 0 ? "PROMOTE" : dec < 0 ? "LOSE" : "WITHHELD");
        if (dec > 0)
            printf("ASI_AV_BAKEOFF_PROMOTE\n");
        else if (dec < 0)
            printf("ASI_AV_BAKEOFF_LOSE\n");
        else
            printf("ASI_AV_BAKEOFF_WITHHELD\n");
        printf("ASI_AV_BAKEOFF_PASS\n");
        return 0;
    }

    if (wa < 0 || wb < 0 || n < 0) {
        usage();
        return 2;
    }
    dec = cnet_asi_av_compare(wa, wb, n, alpha);
    printf("wins_a=%d wins_b=%d n=%d alpha=%.4f dec=%d label=%s\n", wa, wb, n, alpha,
           dec, dec > 0 ? "PROMOTE" : dec < 0 ? "LOSE" : "WITHHELD");
    if (dec > 0)
        printf("ASI_AV_BAKEOFF_PROMOTE\n");
    else if (dec < 0)
        printf("ASI_AV_BAKEOFF_LOSE\n");
    else
        printf("ASI_AV_BAKEOFF_WITHHELD\n");
    printf("ASI_AV_BAKEOFF_PASS\n");
    return 0;
}
