/*
 * End-to-end property (equational law) demonstration.
 *
 * (1) Authors the two round-trip laws in code, persists them as
 *     property files, reloads them, and checks both against the real
 *     frozen primitives:
 *         split(combine(hi, lo)) = identity   (256 nibble pairs)
 *         combine(split(b))      = identity   (256 bytes)
 * (2) The regression story: a second combine instance gets one weight
 *     perturbed in memory and is registered in a fresh registry; the
 *     first law now reports real violations -- a broken retrain cannot
 *     hide from the algebra, even with no exemplar contract in sight.
 *
 * Requires weight files from ./nn_demo. Run from the repo root
 * (make property).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/property.h"

#include <stdio.h>
#include <string.h>

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL) {
        port_set_tag(&p, tag);
    }
    return p;
}

int main(void) {
    BinaryTransformNetwork combine  = {0};
    BinaryTransformNetwork split    = {0};
    BinaryTransformNetwork combine2 = {0};

    PrimitiveRegistry reg;
    PrimitiveRegistry reg2;

    Property law1, law2;
    Property loaded1, loaded2;
    PropertyReport rep;

    int rc = 0;

    /* ------------------------------------------------------------------ */
    /* Load frozen primitives                                               */
    /* ------------------------------------------------------------------ */
    if (btn_load(&combine, "combine_weights.txt") != 0 ||
        btn_load(&split,   "split_weights.txt")   != 0) {
        fprintf(stderr, "FAIL: could not load frozen primitives "
                        "(run ./nn_demo first).\n");
        return 1;
    }

    registry_init(&reg);
    registry_add(&reg, &combine, "combine");
    registry_add(&reg, &split,   "split");

    /* ================================================================== */
    /* Part 1: author + persist + reload laws                              */
    /* ================================================================== */
    printf("=== Part 1: author and persist laws ===\n\n");

    /* Law 1: split(combine(hi, lo)) = identity on nibble pairs */
    memset(&law1, 0, sizeof law1);
    strcpy(law1.name, "split_inverts_combine");
    law1.sources[0] = PT(PORT_BINARY_MSB, 4, 1, "nibble_value");
    law1.sources[1] = PT(PORT_BINARY_MSB, 4, 1, "nibble_value");
    law1.source_count = 2;
    strcpy(law1.lhs[0], "combine");
    strcpy(law1.lhs[1], "split");
    law1.lhs_len = 2;
    law1.rhs_len = 0;

    if (property_save(&law1, "split_inverts_combine_property.txt") != 0) {
        fprintf(stderr, "FAIL: property_save(split_inverts_combine) failed.\n");
        rc = 1;
        goto cleanup;
    }
    printf("  split_inverts_combine_property.txt written\n");

    /* Law 2: combine(split(b)) = identity on bytes */
    memset(&law2, 0, sizeof law2);
    strcpy(law2.name, "combine_inverts_split");
    law2.sources[0] = PT(PORT_BINARY_MSB, 8, 1, "byte_value");
    law2.source_count = 1;
    strcpy(law2.lhs[0], "split");
    strcpy(law2.lhs[1], "combine");
    law2.lhs_len = 2;
    law2.rhs_len = 0;

    if (property_save(&law2, "combine_inverts_split_property.txt") != 0) {
        fprintf(stderr, "FAIL: property_save(combine_inverts_split) failed.\n");
        rc = 1;
        goto cleanup;
    }
    printf("  combine_inverts_split_property.txt written\n\n");

    /* ================================================================== */
    /* Part 2: reload and check both laws against real primitives           */
    /* ================================================================== */
    printf("=== Part 2: reload laws and check against real primitives ===\n\n");

    memset(&loaded1, 0, sizeof loaded1);
    if (property_load(&loaded1, "split_inverts_combine_property.txt") != 0) {
        fprintf(stderr, "FAIL: property_load(split_inverts_combine) failed.\n");
        rc = 1;
        goto cleanup;
    }

    memset(&loaded2, 0, sizeof loaded2);
    if (property_load(&loaded2, "combine_inverts_split_property.txt") != 0) {
        fprintf(stderr, "FAIL: property_load(combine_inverts_split) failed.\n");
        rc = 1;
        goto cleanup;
    }

    /* Check law 1: split_inverts_combine */
    memset(&rep, 0, sizeof rep);
    if (property_check(&loaded1, &reg, 4096, &rep) != 0 || rep.inputs != 256) {
        fprintf(stderr, "FAIL: split_inverts_combine did not hold "
                        "(inputs=%lu held=%lu violated=%lu).\n",
                (unsigned long)rep.inputs,
                (unsigned long)rep.held,
                (unsigned long)rep.violated);
        rc = 1;
        goto cleanup;
    }
    printf("  split_inverts_combine : HOLDS (%lu/%lu)\n",
           (unsigned long)rep.held, (unsigned long)rep.inputs);

    /* Check law 2: combine_inverts_split */
    memset(&rep, 0, sizeof rep);
    if (property_check(&loaded2, &reg, 4096, &rep) != 0 || rep.inputs != 256) {
        fprintf(stderr, "FAIL: combine_inverts_split did not hold "
                        "(inputs=%lu held=%lu violated=%lu).\n",
                (unsigned long)rep.inputs,
                (unsigned long)rep.held,
                (unsigned long)rep.violated);
        rc = 1;
        goto cleanup;
    }
    printf("  combine_inverts_split : HOLDS (%lu/%lu)\n\n",
           (unsigned long)rep.held, (unsigned long)rep.inputs);

    /* ================================================================== */
    /* Part 3: regression story -- a perturbed combine cannot hide          */
    /* ================================================================== */
    printf("=== Part 3: regression story -- a broken retrain is caught ===\n\n");

    if (btn_load(&combine2, "combine_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load combine2 "
                        "(run ./nn_demo first).\n");
        rc = 1;
        goto cleanup;
    }

    /* +10.0: must be large enough to push outputs past the canonicalization
       margin -- a small nudge gets rounded away and produces no violation. */
    combine2.hidden_output_weights[0] += 10.0;

    registry_init(&reg2);
    registry_add(&reg2, &combine2, "combine");
    registry_add(&reg2, &split,    "split");

    memset(&rep, 0, sizeof rep);
    if (property_check(&loaded1, &reg2, 4096, &rep) != -1 || rep.violated < 1) {
        fprintf(stderr, "FAIL: perturbed combine did not violate the law "
                        "(held=%lu violated=%lu). "
                        "Try a larger perturbation.\n",
                (unsigned long)rep.held,
                (unsigned long)rep.violated);
        rc = 1;
        registry_free(&reg2);
        goto cleanup;
    }
    printf("  perturbed combine     : VIOLATED (held %lu/256) "
           "-- a broken retrain cannot hide from the algebra\n\n",
           (unsigned long)rep.held);

    registry_free(&reg2);

    /* ------------------------------------------------------------------ */
cleanup:
    registry_free(&reg);
    btn_free(&combine);
    btn_free(&split);
    btn_free(&combine2);

    if (rc == 0) {
        printf("PROPERTY PASS: the algebra holds, and a broken implementation "
               "is caught by a law, not by luck.\n");
        return 0;
    }
    printf("PROPERTY FAIL.\n");
    return 1;
}
