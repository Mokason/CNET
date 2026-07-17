/* CNB base inspector: counts, certify-on-load verification, tag audit,
 * cross-unit overlap analysis (read-only mining-prefetch of exemplar inputs),
 * and (optionally) fidelity vs a second base.
 *
 * Usage: cnb_audit <base.cnb> [flags|other.cnb]
 *   --count         Load + print unit/blob/tag/oracle counts only (ops fast path)
 *   --no-registry   Skip certify-on-load. Runs tag audit + overlap analysis.
 *   --no-overlap    With full audit, skip cross-unit overlap (still does tags)
 *
 * With two bases: for every unit name present in BOTH, compare behavior
 * digests — equality means the two bases certify byte-identical behavior for
 * that skill (the fidelity check: re-mine into a fresh base, compare). */

#include <stdio.h>
#include <string.h>

#include "../include/base.h"

static int is_flag(const char *s) {
    return s && s[0] == '-' && s[1] == '-';
}

int main(int argc, char **argv) {
    CnetBase a, b;
    PrimitiveRegistry reg;
    size_t skipped = 0, i, j;
    int do_registry = 1;
    int count_only = 0;
    int do_overlap = 1;
    const char *other = NULL;
    int ai;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <base.cnb> [--count|--no-registry|--no-overlap|other.cnb]\n",
                argv[0]);
        return 2;
    }

    for (ai = 2; ai < argc; ai++) {
        if (strcmp(argv[ai], "--count") == 0) {
            count_only = 1;
            do_registry = 0;
            do_overlap = 0;
        } else if (strcmp(argv[ai], "--no-registry") == 0) {
            do_registry = 0;
        } else if (strcmp(argv[ai], "--no-overlap") == 0) {
            do_overlap = 0;
        } else if (!is_flag(argv[ai]) && !other) {
            other = argv[ai];
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[ai]);
            return 2;
        }
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

    if (count_only) {
        cnb_free(&a);
        return 0;
    }

    if (do_registry) {
        registry_init(&reg);
        if (cnb_load_registry(&a, &reg, &skipped) != 0) {
            fprintf(stderr, "registry load failed\n");
            cnb_free(&a);
            return 1;
        }
        printf("certify-on-load: %lu certified, %lu SKIPPED (failed replay)\n",
               (unsigned long)reg.count, (unsigned long)skipped);
        registry_free(&reg);
    } else {
        printf("certify-on-load: SKIPPED (--no-registry)\n");
    }

    cnb_tag_audit(&a, stdout);

    if (do_overlap)
        cnb_analyze_cross_unit_overlap(&a, stdout);

    /* Fidelity comparison only if a real second base arg is given. */
    if (other) {
        size_t both = 0, match = 0;
        cnb_init(&b);
        if (cnb_load(&b, other) != 0) {
            fprintf(stderr, "cannot load %s\n", other);
            cnb_free(&a);
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
        if (match != both) {
            cnb_free(&a);
            return 1;
        }
    }

    cnb_free(&a);
    return 0;
}
