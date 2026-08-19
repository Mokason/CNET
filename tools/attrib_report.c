/* Reads a CNET_ATTRIB sidecar and prints the per-(proposer, signature)
   scorecard. Report-only inspector; changes nothing on disk.

   Reading the columns:
     trust  low  => this proposer supplies unusable labels for this signature
     yield  low  => the student cannot learn this signature even with good
                    labels; look at the recipe or the domain, not the proposer
   The two are independent on purpose — that separation is the whole point of
   the layer. A single blended number cannot tell those cases apart.

   usage: attrib_report <path-to.stats> */

#include <stdio.h>
#include <stdlib.h>

#include "../include/attribution.h"

int main(int argc, char **argv) {
    AttribLedger *L;
    size_t i;
    if (argc < 2) {
        fprintf(stderr, "usage: attrib_report <path-to.stats>\n");
        return 2;
    }
    L = (AttribLedger *)malloc(sizeof *L);
    if (!L) { fprintf(stderr, "alloc failed\n"); return 1; }
    attrib_ledger_init(L);
    if (attrib_ledger_load(L, argv[1]) != 0) {
        fprintf(stderr, "refused: %s\n", argv[1]);
        free(L);
        return 1;
    }
    printf("%-18s %-26s %5s %5s %5s %5s  %5s %5s  %s\n",
           "PROPOSER", "GOAL SIGNATURE", "adm", "prop", "sys", "blam",
           "trust", "yield", "note");
    for (i = 0; i < L->count; ++i) {
        const AttribRecord *r = &L->keys[i];
        char sig[64];
        const char *note = "";
        snprintf(sig, sizeof sig, "fam%d/w%lu/c%lu/%s",
                 (int)r->goal.family,
                 (unsigned long)r->goal.field_width,
                 (unsigned long)r->goal.field_count,
                 r->goal.tag[0] ? r->goal.tag : "-");
        /* Failures spread across several recipe fingerprints point at the
           domain; repeated failure under a single one points at the recipe. */
        if (r->recipe_fp_count > 1) note = "domain-suspect";
        else if (r->recipe_fp_count == 1 && r->system_fault > 0)
            note = "recipe-suspect";
        if (r->unclassified > 0) note = "HAS UNCLASSIFIED ATOMS";
        printf("%-18s %-26s %5lu %5lu %5lu %5lu  %5.3f %5.3f  %s\n",
               r->proposer[0] ? r->proposer : "-", sig,
               (unsigned long)r->admitted,
               (unsigned long)r->proposer_fault,
               (unsigned long)r->system_fault,
               (unsigned long)r->blameless,
               attrib_proposer_trust(r), attrib_signature_yield(r), note);
        if (r->admitted > 0)
            printf("%-18s   admitted: card_sum=%lu worst_margin=%.4f "
                   "proven=%lu sampled=%lu\n", "",
                   (unsigned long)r->admitted_card_sum,
                   r->admitted_margin_min,
                   (unsigned long)r->admitted_proof,
                   (unsigned long)r->admitted_sampled);
    }
    if (L->count == 0)
        printf("(no keys recorded)\n");
    if (L->dropped_events)
        printf("\nWARNING: %lu events dropped - observation was degraded.\n",
               (unsigned long)L->dropped_events);
    free(L);
    return 0;
}
