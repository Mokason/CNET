/* CNB base inspector: counts, certify-on-load verification, tag audit, and
 * (optionally) unit-by-unit behavior-digest comparison against a second base.
 *
 * Usage: cnb_audit <base.cnb> [other.cnb]
 * With two bases: for every unit name present in BOTH, compare behavior
 * digests — equality means the two bases certify byte-identical behavior for
 * that skill (the fidelity check: re-mine into a fresh base, compare). */

#include <stdio.h>
#include <string.h>

#include "../include/base.h"

int main(int argc, char **argv) {
    CnetBase a, b;
    PrimitiveRegistry reg;
    size_t skipped = 0, i, j;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <base.cnb> [other.cnb]\n", argv[0]);
        return 2;
    }

    cnb_init(&a);
    if (cnb_load(&a, argv[1]) != 0) {
        fprintf(stderr, "cannot load %s (missing or seal refused)\n", argv[1]);
        return 1;
    }
    printf("base %s: units=%lu blobs=%lu tags=%lu oracles=%lu stats=%lu\n",
           argv[1], (unsigned long)a.unit_count, (unsigned long)a.blob_count,
           (unsigned long)a.tag_count, (unsigned long)a.oracle_count,
           (unsigned long)a.stats_count);

    registry_init(&reg);
    if (cnb_load_registry(&a, &reg, &skipped) != 0) {
        fprintf(stderr, "registry load failed\n");
        return 1;
    }
    printf("certify-on-load: %lu certified, %lu SKIPPED (failed replay)\n",
           (unsigned long)reg.count, (unsigned long)skipped);
    registry_free(&reg);

    cnb_tag_audit(&a, stdout);

    if (argc > 2) {
        size_t both = 0, match = 0;
        cnb_init(&b);
        if (cnb_load(&b, argv[2]) != 0) {
            fprintf(stderr, "cannot load %s\n", argv[2]);
            return 1;
        }
        for (i = 0; i < b.unit_count; ++i) {
            for (j = 0; j < a.unit_count; ++j) {
                if (strcmp(a.units[j].name, b.units[i].name) == 0) {
                    int eq = (a.units[j].behavior_digest ==
                              b.units[i].behavior_digest);
                    both++;
                    match += (size_t)eq;
                    printf("  fidelity %-24s %s\n", b.units[i].name,
                           eq ? "behavior-digest IDENTICAL" : "MISMATCH");
                    break;
                }
            }
        }
        printf("fidelity: %lu/%lu shared units identical\n",
               (unsigned long)match, (unsigned long)both);
        cnb_free(&b);
        if (match != both) { cnb_free(&a); return 1; }
    }

    cnb_free(&a);
    return 0;
}
