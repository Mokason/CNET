/*
 * test_heal_mismatch.c -- regression gate for the registry_heal
 * contract/BTN dimension-mismatch memory-safety bug.
 *
 * registry_heal (contract.c) uses btn->input_count / btn->output_count as
 * the row stride when memcpy'ing contract->inputs / contract->outputs into
 * freshly allocated training buffers.  The contract's exemplar tables are
 * laid out with in_total = ports_total(c->input_ports) and out_total =
 * ports_total(c->output_ports).  When a wrong contract is passed (in_total
 * != btn->input_count or out_total != btn->output_count) the memcpy reads
 * past the end of the contract buffer (over-read) or too few doubles
 * (under-read / data corruption), and the malloc'd destination may also be
 * oversized or undersized.
 *
 * This test asserts registry_heal REFUSES the mismatched contract safely
 * (returns -1 or 0 without mutating state) and leaves the registry entry
 * and retrain queue fully intact.  Two mismatch cases are covered:
 *
 *   1. Mismatched INPUT  width: btn input_count=8, contract in_total=4.
 *   2. Mismatched OUTPUT width: btn output_count=2, contract out_total=1.
 *
 * Build (normal):
 *   make heal_mismatch
 * Build (ASan/UBSan):
 *   make heal_mismatch_san
 *
 * Success marker (printed on pass): HEAL_MISMATCH_PASS
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ------------------------------------------------------------- */

/* Build a btn with ONEHOT(input_w) x BINARY_MSB(output_w) port signature. */
static int make_btn(BinaryTransformNetwork *btn, size_t input_w,
                    size_t output_w) {
    Port in_port, out_port;
    if (btn_init(btn, input_w, output_w, 6, 12, 0.5, 42u) != 0) return -1;
    memset(&in_port, 0, sizeof in_port);
    in_port.family   = PORT_ONEHOT;
    in_port.field_width = input_w;
    in_port.field_count = 1;
    memset(&out_port, 0, sizeof out_port);
    out_port.family   = PORT_BINARY_MSB;
    out_port.field_width = output_w;
    out_port.field_count = 1;
    return btn_set_ports(btn, in_port, out_port);
}

/* We need to track the exemplar buffers to free them after contract_free. */
static double *g_in_buf  = NULL;
static double *g_out_buf = NULL;

/* Build a contract whose port signature has input total = c_in_w and
 * output total = c_out_w.  The exemplar tables are laid out to match the
 * CONTRACT's ports (not the btn's).  This is the key: the contract is
 * internally consistent but its dimensions differ from the btn. */
static int make_contract_tracked(Contract *c, size_t c_in_w, size_t c_out_w,
                                 size_t n_exemplars) {
    BinaryTransformNetwork fake;
    double *in_buf, *out_buf;
    size_t i;
    int rc;

    if (btn_init(&fake, c_in_w, c_out_w, 4, 8, 0.5, 1u) != 0) return -1;
    {
        Port in_port, out_port;
        memset(&in_port, 0, sizeof in_port);
        in_port.family   = PORT_ONEHOT;
        in_port.field_width = c_in_w;
        in_port.field_count = 1;
        memset(&out_port, 0, sizeof out_port);
        out_port.family   = PORT_BINARY_MSB;
        out_port.field_width = c_out_w;
        out_port.field_count = 1;
        btn_set_ports(&fake, in_port, out_port);
    }

    in_buf  = calloc(n_exemplars * c_in_w, sizeof(double));
    out_buf = calloc(n_exemplars * c_out_w, sizeof(double));
    if (in_buf == NULL || out_buf == NULL) {
        free(in_buf); free(out_buf); btn_free(&fake); return -1;
    }
    for (i = 0; i < n_exemplars; ++i) {
        in_buf[i * c_in_w + (i % c_in_w)] = 1.0;
        if (c_out_w > 0)
            out_buf[i * c_out_w + (i % c_out_w)] = 1.0;
    }

    rc = contract_init_borrowed(c, "mismatch_contract", &fake,
                                 in_buf, out_buf, n_exemplars);
    btn_free(&fake);
    if (rc != 0) { free(in_buf); free(out_buf); return -1; }
    g_in_buf  = in_buf;
    g_out_buf = out_buf;
    return 0;
}

/* Build a retrain queue owned by the registry entry, with 1 labeled sample
 * sized to the BTN (not the contract).  Returns 0 on success. */
static int attach_queue(PrimitiveRegistry *reg, size_t idx,
                        size_t btn_in, size_t btn_out) {
    RetrainQueue *q = calloc(1, sizeof(RetrainQueue));
    if (q == NULL) return -1;
    q->input_count  = btn_in;
    q->output_count = btn_out;
    q->labeled_cap  = 8;
    q->labeled_count = 1;
    q->labeled_inputs  = calloc(btn_in, sizeof(double));
    q->labeled_targets = calloc(btn_out, sizeof(double));
    if (q->labeled_inputs == NULL || q->labeled_targets == NULL) {
        free(q->labeled_inputs); free(q->labeled_targets); free(q);
        return -1;
    }
    /* Write a known pattern so we can verify it survives the refused heal. */
    q->labeled_inputs[0]  = 1.0;
    q->labeled_targets[0] = 1.0;
    reg->entries[idx].queue = q;
    return 0;
}

/* ---- test cases ---------------------------------------------------------- */

static int test_input_width_mismatch(void) {
    BinaryTransformNetwork btn;
    PrimitiveRegistry reg;
    Contract c;
    int rc;
    size_t pre_count;

    printf("-- test_input_width_mismatch: btn_in=8, contract_in=4 --\n");

    /* btn: 8 inputs, 2 outputs.  Contract: 4 inputs, 2 outputs.
     * The input dimensions differ (8 != 4); output dimensions match. */
    if (make_btn(&btn, 8, 2) != 0) { printf("FAIL: btn init\n"); return 1; }
    if (make_contract_tracked(&c, 4, 2, 2) != 0) {
        printf("FAIL: contract init\n"); return 1;
    }

    registry_init(&reg);
    if (registry_add(&reg, &btn, "test_prim") != 0) {
        printf("FAIL: registry_add\n"); return 1;
    }
    reg.entries[0].state = PRIM_RESET;
    if (attach_queue(&reg, 0, btn.input_count, btn.output_count) != 0) {
        printf("FAIL: attach_queue\n"); return 1;
    }

    pre_count = reg.entries[0].queue->labeled_count;

    /* registry_heal must REFUSE the mismatched contract. */
    rc = registry_heal(&reg, "test_prim", &c, 100);
    printf("   registry_heal returned %d\n", rc);

    /* Assert: heal refused (did not return 1 = success). */
    if (rc == 1) {
        printf("FAIL: registry_heal ACCEPTED mismatched input width (rc=1)\n");
        goto fail;
    }

    /* Assert: state is still PRIM_RESET (unchanged). */
    if (reg.entries[0].state != PRIM_RESET) {
        printf("FAIL: state mutated to %d (expected PRIM_RESET=%d)\n",
               reg.entries[0].state, PRIM_RESET);
        goto fail;
    }

    /* Assert: queue is intact (not freed, labeled_count unchanged). */
    if (reg.entries[0].queue == NULL) {
        printf("FAIL: queue was freed by refused heal\n");
        goto fail;
    }
    if (reg.entries[0].queue->labeled_count != pre_count) {
        printf("FAIL: labeled_count changed from %zu to %zu\n",
               pre_count, reg.entries[0].queue->labeled_count);
        goto fail;
    }
    /* Assert: labeled data is intact. */
    if (reg.entries[0].queue->labeled_inputs[0] != 1.0 ||
        reg.entries[0].queue->labeled_targets[0] != 1.0) {
        printf("FAIL: labeled data was corrupted\n");
        goto fail;
    }

    printf("   PASS: heal refused, state/queue intact\n");
    /* cleanup: registry_free owns and frees the queue; btn_free frees
     * the btn's internal allocations (registry_add borrows the btn ptr). */
    registry_free(&reg);
    btn_free(&btn);
    contract_free(&c);
    free(g_in_buf); g_in_buf = NULL;
    free(g_out_buf); g_out_buf = NULL;
    return 0;

fail:
    registry_free(&reg);
    btn_free(&btn);
    contract_free(&c);
    free(g_in_buf); g_in_buf = NULL;
    free(g_out_buf); g_out_buf = NULL;
    return 1;
}

static int test_output_width_mismatch(void) {
    BinaryTransformNetwork btn;
    PrimitiveRegistry reg;
    Contract c;
    int rc;
    size_t pre_count;

    printf("-- test_output_width_mismatch: btn_out=2, contract_out=1 --\n");

    /* btn: 4 inputs, 2 outputs.  Contract: 4 inputs, 1 output.
     * Input dimensions match; output dimensions differ (2 != 1). */
    if (make_btn(&btn, 4, 2) != 0) { printf("FAIL: btn init\n"); return 1; }
    if (make_contract_tracked(&c, 4, 1, 2) != 0) {
        printf("FAIL: contract init\n"); return 1;
    }

    registry_init(&reg);
    if (registry_add(&reg, &btn, "test_prim") != 0) {
        printf("FAIL: registry_add\n"); return 1;
    }
    reg.entries[0].state = PRIM_RESET;
    if (attach_queue(&reg, 0, btn.input_count, btn.output_count) != 0) {
        printf("FAIL: attach_queue\n"); return 1;
    }

    pre_count = reg.entries[0].queue->labeled_count;

    rc = registry_heal(&reg, "test_prim", &c, 100);
    printf("   registry_heal returned %d\n", rc);

    if (rc == 1) {
        printf("FAIL: registry_heal ACCEPTED mismatched output width (rc=1)\n");
        goto fail;
    }
    if (reg.entries[0].state != PRIM_RESET) {
        printf("FAIL: state mutated to %d (expected PRIM_RESET=%d)\n",
               reg.entries[0].state, PRIM_RESET);
        goto fail;
    }
    if (reg.entries[0].queue == NULL) {
        printf("FAIL: queue was freed by refused heal\n");
        goto fail;
    }
    if (reg.entries[0].queue->labeled_count != pre_count) {
        printf("FAIL: labeled_count changed from %zu to %zu\n",
               pre_count, reg.entries[0].queue->labeled_count);
        goto fail;
    }
    if (reg.entries[0].queue->labeled_inputs[0] != 1.0 ||
        reg.entries[0].queue->labeled_targets[0] != 1.0) {
        printf("FAIL: labeled data was corrupted\n");
        goto fail;
    }

    printf("   PASS: heal refused, state/queue intact\n");
    registry_free(&reg);
    btn_free(&btn);
    contract_free(&c);
    free(g_in_buf); g_in_buf = NULL;
    free(g_out_buf); g_out_buf = NULL;
    return 0;

fail:
    registry_free(&reg);
    btn_free(&btn);
    contract_free(&c);
    free(g_in_buf); g_in_buf = NULL;
    free(g_out_buf); g_out_buf = NULL;
    return 1;
}

/* Sanity: a MATCHING contract should not error (positive control -- ensures
 * our refusal is due to the mismatch, not a broken test harness). */
static int test_matching_no_error(void) {
    BinaryTransformNetwork btn;
    PrimitiveRegistry reg;
    Contract c;
    int rc;

    printf("-- test_matching_no_error: btn 4x2, contract 4x2 --\n");

    if (make_btn(&btn, 4, 2) != 0) { printf("FAIL: btn init\n"); return 1; }
    if (make_contract_tracked(&c, 4, 2, 2) != 0) {
        printf("FAIL: contract init\n"); return 1;
    }

    registry_init(&reg);
    if (registry_add(&reg, &btn, "test_prim") != 0) {
        printf("FAIL: registry_add\n"); return 1;
    }
    reg.entries[0].state = PRIM_RESET;
    if (attach_queue(&reg, 0, btn.input_count, btn.output_count) != 0) {
        printf("FAIL: attach_queue\n"); return 1;
    }

    rc = registry_heal(&reg, "test_prim", &c, 100);
    printf("   registry_heal returned %d\n", rc);

    /* On a matching contract, heal may return 1 (certified) or 0 (trained
     * but certification failed, stays RESET).  It must NOT return -1
     * (error).  We accept rc >= 0 as "did not crash/refuse with error". */
    if (rc < 0) {
        printf("FAIL: registry_heal returned error %d on MATCHING contract\n", rc);
        goto fail;
    }
    printf("   PASS: matching contract did not error (rc=%d)\n", rc);

    /* cleanup: registry_free owns and frees the queue (or NULL if heal
     * succeeded and cleared it).  btn_free releases the btn's internal
     * allocations. */
    registry_free(&reg);
    btn_free(&btn);
    contract_free(&c);
    free(g_in_buf); g_in_buf = NULL;
    free(g_out_buf); g_out_buf = NULL;
    return 0;

fail:
    registry_free(&reg);
    btn_free(&btn);
    contract_free(&c);
    free(g_in_buf); g_in_buf = NULL;
    free(g_out_buf); g_out_buf = NULL;
    return 1;
}

int main(void) {
    int fails = 0;

    printf("== registry_heal signature mismatch regression test ==\n");
    fails += test_input_width_mismatch();
    fails += test_output_width_mismatch();
    fails += test_matching_no_error();

    if (fails != 0) {
        printf("\nHEAL_MISMATCH_FAIL: %d sub-test(s) failed\n", fails);
        return 1;
    }
    printf("\nHEAL_MISMATCH_PASS\n");
    return 0;
}