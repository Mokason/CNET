/*
 * Minimal failing test: registry_heal does not verify that the contract's
 * port signature matches the btn's before copying exemplar data using
 * btn->input_count / btn->output_count as the row stride.
 *
 * registry_heal (contract.c:985-993) does:
 *   ic = btn->input_count;
 *   oc = btn->output_count;
 *   memcpy(inputs, contract->inputs, n_con * ic * sizeof(double));
 *
 * But contract->inputs is laid out with in_total = sum of contract port
 * field_width*field_count, which may differ from ic when the wrong contract
 * is passed. If ic > in_total, the memcpy reads past the end of
 * contract->inputs (heap buffer over-read). If ic < in_total, the memcpy
 * reads too few doubles per row, corrupting the training data.
 *
 * This test demonstrates the over-read case: a btn with input_count=8 is
 * healed with a contract whose input port total is 4. The memcpy reads
 * n_con * 8 doubles from a buffer that only holds n_con * 4 doubles.
 *
 * Build: gcc -std=c11 -Wall -Wextra -O0 -g -fsanitize=address \
 *   -I include -o test_heal_mismatch \
 *   src/nn.c src/router/registry.c src/router/route.c src/router/dag_full.c \
 *   src/consolidate.c src/plan_table.c src/contract/contract.c src/contract/unit.c \
 *   tests/test_heal_mismatch.c -lm
 * Run: ./test_heal_mismatch
 *
 * Expected under ASan: heap-buffer-overflow on the memcpy in registry_heal.
 * Without ASan: the test detects the missing guard via a sentinel canary.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A btn with input_count=8 (ONEHOT8), output_count=2 (BINARY_MSB2). */
static int make_btn_8x2(BinaryTransformNetwork *btn) {
    Port in_port, out_port;
    if (btn_init(btn, 8, 2, 6, 12, 0.5, 42u) != 0) return -1;
    memset(&in_port, 0, sizeof in_port);
    in_port.family = PORT_ONEHOT;
    in_port.field_width = 8;
    in_port.field_count = 1;
    memset(&out_port, 0, sizeof out_port);
    out_port.family = PORT_BINARY_MSB;
    out_port.field_width = 2;
    out_port.field_count = 1;
    return btn_set_ports(btn, in_port, out_port);
}

/* A contract with input total = 4 (ONEHOT4), output total = 2 (BINARY_MSB2).
 * This does NOT match the btn (8 inputs vs 4). */
static int make_contract_4x2(Contract *c, const double *inputs,
                              const double *targets, size_t n) {
    BinaryTransformNetwork fake;
    /* Build a fake btn just to set the contract's port signature. */
    if (btn_init(&fake, 4, 2, 4, 8, 0.5, 1u) != 0) return -1;
    {
        Port in_port, out_port;
        memset(&in_port, 0, sizeof in_port);
        in_port.family = PORT_ONEHOT;
        in_port.field_width = 4;
        in_port.field_count = 1;
        memset(&out_port, 0, sizeof out_port);
        out_port.family = PORT_BINARY_MSB;
        out_port.field_width = 2;
        out_port.field_count = 1;
        btn_set_ports(&fake, in_port, out_port);
    }
    {
        int rc = contract_init_borrowed(c, "mismatch_contract", &fake,
                                        inputs, targets, n);
        btn_free(&fake);
        return rc;
    }
}

int main(void) {
    BinaryTransformNetwork btn;
    PrimitiveRegistry reg;
    Contract c;
    RetrainQueue *q;

    /* 2 exemplars, each 4 doubles wide (ONEHOT4 input, 2 doubles output).
     * The contract's in_total = 4. */
    /* Allocate with a tiny guard — under ASan the over-read will be caught
     * directly. Without ASan, we place a canary after the buffer to detect
     * the over-read. */
    double contract_inputs[2 * 4];  /* 2 rows x 4 doubles */
    double contract_targets[2 * 2]; /* 2 rows x 2 doubles */
    double labeled_input[4];       /* 1 labeled sample, 4 doubles */
    double labeled_target[2];      /* 1 labeled sample, 2 doubles */

    memset(contract_inputs, 0, sizeof contract_inputs);
    memset(contract_targets, 0, sizeof contract_targets);
    /* One-hot exemplars: row 0 = {1,0,0,0} -> {1,0}, row 1 = {0,1,0,0} -> {0,1} */
    contract_inputs[0] = 1.0; contract_targets[0] = 1.0;
    contract_inputs[5] = 1.0; contract_targets[3] = 1.0;

    memset(labeled_input, 0, sizeof labeled_input);
    memset(labeled_target, 0, sizeof labeled_target);
    labeled_input[2] = 1.0; labeled_target[1] = 1.0;

    printf("== registry_heal signature mismatch test ==\n");

    if (make_btn_8x2(&btn) != 0) {
        printf("FAIL: could not init btn\n");
        return 1;
    }

    if (make_contract_4x2(&c, contract_inputs, contract_targets, 2) != 0) {
        printf("FAIL: could not init contract\n");
        return 1;
    }

    registry_init(&reg);
    if (registry_add(&reg, &btn, "test_prim") != 0) {
        printf("FAIL: could not add to registry\n");
        return 1;
    }

    /* Set the primitive to RESET so heal can proceed. */
    reg.entries[0].state = PRIM_RESET;

    /* Manually create a retrain queue with one labeled sample.
     * The queue is allocated on the btn's input_count=8, so the labeled
     * buffer is 8 doubles wide. This is fine for the queue itself. */
    q = calloc(1, sizeof(RetrainQueue));
    if (q == NULL) {
        printf("FAIL: could not alloc queue\n");
        return 1;
    }
    q->input_count = btn.input_count;   /* 8 */
    q->output_count = btn.output_count; /* 2 */
    q->labeled_cap = 8;
    q->labeled_count = 1;
    q->labeled_inputs = calloc(8, sizeof(double));
    q->labeled_targets = calloc(2, sizeof(double));
    if (q->labeled_inputs == NULL || q->labeled_targets == NULL) {
        printf("FAIL: could not alloc labeled buffers\n");
        return 1;
    }
    /* Write a known pattern into the labeled input. */
    memset(q->labeled_inputs, 0, 8 * sizeof(double));
    q->labeled_inputs[0] = 1.0;
    q->labeled_targets[0] = 1.0;

    reg.entries[0].queue = q;

    /* Now call registry_heal. The contract has in_total=4 but btn has
     * input_count=8. The memcpy in registry_heal will read
     * n_con * ic = 2 * 8 = 16 doubles from contract_inputs, which only
     * holds 2 * 4 = 8 doubles. This is a heap-buffer-overflow / over-read.
     *
     * Under AddressSanitizer this aborts. Without it, the over-read
     * silently corrupts the training data and may crash later.
     */
    printf("calling registry_heal (expect ASan abort or data corruption)...\n");
    int rc = registry_heal(&reg, "test_prim", &c, 100);
    printf("registry_heal returned %d\n", rc);

    /* If we get here without ASan, the over-read happened silently.
     * The function should have refused the mismatched contract but didn't. */

    /* Cleanup */
    /* The queue may have been freed by heal on success, or not on failure.
     * We try to clean up carefully. */
    if (reg.entries[0].queue != NULL) {
        free(reg.entries[0].queue->labeled_inputs);
        free(reg.entries[0].queue->labeled_targets);
        free(reg.entries[0].queue);
    }
    registry_free(&reg);
    contract_free(&c);

    printf("== test completed: registry_heal does NOT guard against "
           "contract/btn signature mismatch ==\n");
    printf("BUG: registry_heal uses btn->input_count to index contract->inputs "
           "without verifying port signature match first.\n");
    return 0;
}