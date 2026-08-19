/*
 * Second domain end-to-end: decimal digit arithmetic.
 *
 * The domain-generality claim: the core (nn/router/contract/property/
 * consolidate) is untouched -- every capability below arrives as new
 * primitives, tags, contracts and laws, exercised through the existing
 * machinery only.
 *
 *   Act 1  trains the decimal primitives, gates each on an EXACT exhaustive
 *          domain evaluation (an undertrained primitive breaks DAGs, so
 *          anything below 100% refuses to freeze), saves weights and emits
 *          exemplar contracts.
 *   Act 2  the planner discovers dec_full_add(dec_value(a), dec_value(b), c)
 *          from heterogeneous tagged sources and runs the full 200-member
 *          domain.
 *   Act 3  cross-domain refusal: hex nibbles are representation-identical
 *          to decimal digits (BINARY_MSB 4), but the dec_digit tag keeps
 *          hex sources out of decimal adders.
 *   Act 4  ripple-carry: two 2-digit numbers added by hand-built strict
 *          DAGs chaining carry_out -> carry_in. Plans are trees with
 *          single-use sources, so a multi-output circuit is a PROGRAM over
 *          frozen primitives, not a planner discovery -- that is a finding,
 *          not a bug.
 *   Act 5  consolidates the Act 2 plan into a chunk (dec_add_unit), emits
 *          the teacher's contract, certifies everything and replans with
 *          require_certified: the evidence-seeded chunk wins.
 *   Act 6  equational laws: dec_to_symbol(dec_value(s)) = identity, and
 *          commutativity add(a,b,c) = add(b,a,c) via a swap primitive.
 *
 * Act 3 loads hex_value_weights.txt (run ./nn_demo first). Run from the
 * repo root (make decimal).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"
#include "../include/contract/contract.h"
#include "../include/property.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- local helpers (independent copies; mirrors the other demos) ---- */

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL && port_set_tag(&p, tag) != 0) {
        fprintf(stderr, "FAIL: bad tag '%s'.\n", tag);
        exit(EXIT_FAILURE);
    }
    return p;
}

static int bits_to_int(const double *bits, size_t n) {
    int value = 0;
    size_t i;
    for (i = 0; i < n; ++i) {
        value = value * 2 + (bits[i] > 0.5 ? 1 : 0);
    }
    return value;
}

/* Hard 0/1 bits, most significant first. */
static void int_to_bits(int value, double *bits, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        bits[i] = (double)((value >> (n - 1 - i)) & 1);
    }
}

static void int_to_onehot(int index, double *vec, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        vec[i] = 0.0;
    }
    vec[index] = 1.0;
}

static void print_node(const DagNode *node, int depth) {
    int i;
    for (i = 0; i < depth; ++i) {
        printf("  ");
    }
    if (node->kind == DAG_SOURCE) {
        printf("source[%d]\n", node->source_index);
    } else {
        size_t c;
        printf("%s", node->name != NULL ? node->name : "?");
        if (node->btn != NULL && node->btn->output_port_count > 1) {
            printf("[out=%d]", node->output_index);
        }
        printf("\n");
        for (c = 0; c < node->child_count; ++c) {
            print_node(node->children[c], depth + 1);
        }
    }
}

/* The training table IS the spec: persist it as a first-class contract
   beside the weights (local copy of nn_demo's emitter, but failure is
   fatal here -- a decimal primitive without its contract cannot certify
   in Act 5). Targets are soft (0.9/0.1); canonicalize before saving. */
static int emit_contract(const char *name,
                         const BinaryTransformNetwork *btn,
                         const double *inputs, const double *targets,
                         size_t exemplars, const char *path) {
    Contract c;
    double *canon = NULL;
    size_t out_total = 0;
    size_t i, p;
    int ok = 1;

    for (p = 0; p < btn->output_port_count; ++p) {
        out_total += btn->output_ports[p].field_width *
                     btn->output_ports[p].field_count;
    }
    if (out_total == 0 || exemplars == 0) {
        return -1;
    }

    canon = (double *)malloc(exemplars * out_total * sizeof(double));
    if (canon == NULL) {
        return -1;
    }

    for (i = 0; ok && i < exemplars; ++i) {
        size_t offset = 0;
        for (p = 0; ok && p < btn->output_port_count; ++p) {
            size_t tot = btn->output_ports[p].field_width *
                         btn->output_ports[p].field_count;
            if (port_canonicalize(btn->output_ports[p],
                                  targets + i * out_total + offset,
                                  canon  + i * out_total + offset) != 0) {
                ok = 0;
            }
            offset += tot;
        }
    }

    if (!ok ||
        contract_init_borrowed(&c, name, btn, inputs, canon,
                               exemplars) != 0 ||
        contract_save(&c, path) != 0) {
        free(canon);
        return -1;
    }

    free(canon);
    return 0;
}

/* Freeze gate: a primitive may be saved only if, over its WHOLE training
   domain, every raw output segment is in-domain (port_validate, the same
   bar the executors score) AND canonicalizes to exactly the target row.
   This is what "frozen and trusted" means; anything less poisons every
   plan built on top. */
static int freeze_gate(BinaryTransformNetwork *btn,
                       const double *inputs, const double *targets,
                       size_t n, const char *label) {
    size_t in_total = btn->input_count;
    size_t out_total = btn->output_count;
    size_t misses = 0;
    size_t i, p, k;
    double got[64];
    double want[64];

    if (out_total > 64) {
        fprintf(stderr, "FAIL: %s output too wide for the gate buffer.\n",
                label);
        return -1;
    }

    for (i = 0; i < n; ++i) {
        const double *raw = btn_forward(btn, inputs + i * in_total);
        const double *target_row = targets + i * out_total;
        size_t offset = 0;
        int sample_ok = 1;

        for (p = 0; p < btn->output_port_count; ++p) {
            Port port = btn->output_ports[p];
            size_t tot = port.field_width * port.field_count;

            if (!port_validate(port, raw + offset) ||
                port_canonicalize(port, raw + offset, got) != 0 ||
                port_canonicalize(port, target_row + offset, want) != 0) {
                sample_ok = 0;
            } else {
                for (k = 0; k < tot; ++k) {
                    if (got[k] != want[k]) {
                        sample_ok = 0;
                    }
                }
            }
            offset += tot;
        }
        if (!sample_ok) {
            ++misses;
        }
    }

    printf("%s domain exact: %lu/%lu\n", label,
           (unsigned long)(n - misses), (unsigned long)n);
    if (misses != 0) {
        fprintf(stderr, "FAIL: %s did not master its domain; "
                        "refusing to freeze.\n", label);
        return -1;
    }
    return 0;
}

/* ---- decimal training data ---- */

static void build_dec_value_data(double inputs[10][10], double targets[10][4]) {
    int digit;
    int bit;
    for (digit = 0; digit < 10; ++digit) {
        int_to_onehot(digit, inputs[digit], 10);
        for (bit = 0; bit < 4; ++bit) {
            targets[digit][bit] =
                ((digit >> (3 - bit)) & 1) ? 0.9 : 0.1;
        }
    }
}

static void build_dec_to_symbol_data(double inputs[10][4], double targets[10][10]) {
    int digit;
    int k;
    for (digit = 0; digit < 10; ++digit) {
        int_to_bits(digit, inputs[digit], 4);
        for (k = 0; k < 10; ++k) {
            targets[digit][k] = (k == digit) ? 0.9 : 0.1;
        }
    }
}

/* dec_full_add: (digit a, digit b, carry_in) -> (sum digit, carry_out).
   Input layout a[4] b[4] cin[1]; output layout sum[4] cout[1].
   200 samples indexed (a*10 + b)*2 + cin. */
static void build_dec_full_add_data(double inputs[200][9], double targets[200][5]) {
    int a, b, cin;
    int bit;
    for (a = 0; a < 10; ++a) {
        for (b = 0; b < 10; ++b) {
            for (cin = 0; cin < 2; ++cin) {
                int idx = (a * 10 + b) * 2 + cin;
                int total = a + b + cin;
                int sum = total % 10;
                int cout = total / 10;

                for (bit = 0; bit < 4; ++bit) {
                    inputs[idx][bit] = (double)((a >> (3 - bit)) & 1);
                    inputs[idx][4 + bit] = (double)((b >> (3 - bit)) & 1);
                }
                inputs[idx][8] = (double)cin;

                for (bit = 0; bit < 4; ++bit) {
                    targets[idx][bit] =
                        ((sum >> (3 - bit)) & 1) ? 0.9 : 0.1;
                }
                targets[idx][4] = cout ? 0.9 : 0.1;
            }
        }
    }
}

/* dec_swap_ab: (a, b, c) -> (b, a, c) over one-hot digit symbols + carry.
   A pure permutation; it exists so commutativity is expressible as a law
   (property chains hand off WHOLE port sequences positionally). */
static void build_dec_swap_data(double inputs[200][21], double targets[200][21]) {
    int a, b, cin;
    int k;
    for (a = 0; a < 10; ++a) {
        for (b = 0; b < 10; ++b) {
            for (cin = 0; cin < 2; ++cin) {
                int idx = (a * 10 + b) * 2 + cin;

                int_to_onehot(a, &inputs[idx][0], 10);
                int_to_onehot(b, &inputs[idx][10], 10);
                inputs[idx][20] = (double)cin;

                for (k = 0; k < 10; ++k) {
                    targets[idx][k]      = (k == b) ? 0.9 : 0.1;
                    targets[idx][10 + k] = (k == a) ? 0.9 : 0.1;
                }
                targets[idx][20] = cin ? 0.9 : 0.1;
            }
        }
    }
}

int main(void) {
    static double dv_inputs[10][10], dv_targets[10][4];
    static double ds_inputs[10][4],  ds_targets[10][10];
    static double fa_inputs[200][9], fa_targets[200][5];
    static double sw_inputs[200][21], sw_targets[200][21];

    BinaryTransformNetwork dec_value   = {0};
    BinaryTransformNetwork dec_fa      = {0};
    BinaryTransformNetwork dec_to_sym  = {0};
    BinaryTransformNetwork dec_swap    = {0};
    BinaryTransformNetwork chunk       = {0};
    BinaryTransformNetwork hex_value   = {0};

    static double a_sym[10], b_sym[10], cin_v[1];
    DagSource add_sources[3];
    DagPlan add_plan = {0};
    int add_planned = 0;

    Contract c_value = {0}, c_fa = {0}, c_chunk = {0};
    int c_value_loaded = 0, c_fa_loaded = 0, c_chunk_loaded = 0;

    double loss;
    int rc = EXIT_FAILURE;

    build_dec_value_data(dv_inputs, dv_targets);
    build_dec_to_symbol_data(ds_inputs, ds_targets);
    build_dec_full_add_data(fa_inputs, fa_targets);
    build_dec_swap_data(sw_inputs, sw_targets);

    /* ================================================================== */
    printf("=== Act 1: train, gate, freeze the decimal primitives ===\n\n");
    /* ================================================================== */

    /* dec_value: one-hot digit symbol -> 4-bit digit value. */
    if (btn_init(&dec_value, 10, 4, 1, 32, 0.7, 211u) != 0) {
        fprintf(stderr, "FAIL: could not initialize dec_value.\n");
        goto cleanup;
    }
    if (btn_set_ports(&dec_value,
                      PT(PORT_ONEHOT, 10, 1, "dec_symbol"),
                      PT(PORT_BINARY_MSB, 4, 1, "dec_digit")) != 0) {
        fprintf(stderr, "FAIL: dec_value port contract mismatch.\n");
        goto cleanup;
    }
    loss = btn_train_dynamic(&dec_value, &dv_inputs[0][0], &dv_targets[0][0],
                             10, 50000, 1000, 0.003, 0.01);
    printf("dec_value: hidden %lu, final loss %.6f\n",
           (unsigned long)dec_value.hidden_count, loss);
    if (freeze_gate(&dec_value, &dv_inputs[0][0], &dv_targets[0][0],
                    10, "dec_value") != 0) {
        goto cleanup;
    }
    if (btn_save(&dec_value, "dec_value_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not persist dec_value.\n");
        goto cleanup;
    }
    remove("dec_value_stats.txt");
    if (emit_contract("dec_value", &dec_value, &dv_inputs[0][0],
                      &dv_targets[0][0], 10, "dec_value_contract.txt") != 0) {
        fprintf(stderr, "FAIL: could not emit dec_value contract.\n");
        goto cleanup;
    }

    /* dec_full_add: (digit, digit, carry) -> (sum digit, carry out).
       The mod-10/carry boundary is genuinely nonlinear and the chunk
       lesson generalizes: a too-narrow early hidden layer saturates on a
       low-rank approximation that growth cannot repair (init 9 + eager
       growth capped out at 194/200; init 16 with patient growth masters
       the domain without growing at all).

       Train with btn_train_dynamic_spec: the 200 rows ARE the complete
       finite domain freeze_gate certifies against. btn_train_dynamic
       holds out sample_count/5 when N>=64, so ~40 adder cells never see
       a gradient and the gate fails (~189/200 with seed 31). Same class
       of defect b9a7d53 fixed for consolidation. */
    if (btn_init(&dec_fa, 9, 5, 16, 128, 0.8, 31u) != 0) {
        fprintf(stderr, "FAIL: could not initialize dec_full_add.\n");
        goto cleanup;
    }
    {
        Port fa_in[3];
        Port fa_out[2];
        fa_in[0] = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        fa_in[1] = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        fa_in[2] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        fa_out[0] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
        fa_out[1] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        if (btn_set_io_ports(&dec_fa, fa_in, 3, fa_out, 2) != 0) {
            fprintf(stderr, "FAIL: dec_full_add port contract mismatch.\n");
            goto cleanup;
        }
    }
    loss = btn_train_dynamic_spec(&dec_fa, &fa_inputs[0][0], &fa_targets[0][0],
                                  200, 300000, 1000, 0.0008, 0.01);
    printf("dec_full_add: hidden %lu, final loss %.6f\n",
           (unsigned long)dec_fa.hidden_count, loss);
    if (freeze_gate(&dec_fa, &fa_inputs[0][0], &fa_targets[0][0],
                    200, "dec_full_add") != 0) {
        goto cleanup;
    }
    if (btn_save(&dec_fa, "dec_full_add_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not persist dec_full_add.\n");
        goto cleanup;
    }
    remove("dec_full_add_stats.txt");
    if (emit_contract("dec_full_add", &dec_fa, &fa_inputs[0][0],
                      &fa_targets[0][0], 200,
                      "dec_full_add_contract.txt") != 0) {
        fprintf(stderr, "FAIL: could not emit dec_full_add contract.\n");
        goto cleanup;
    }

    /* dec_to_symbol: 4-bit digit value -> one-hot digit symbol. */
    if (btn_init(&dec_to_sym, 4, 10, 4, 64, 0.7, 223u) != 0) {
        fprintf(stderr, "FAIL: could not initialize dec_to_symbol.\n");
        goto cleanup;
    }
    if (btn_set_ports(&dec_to_sym,
                      PT(PORT_BINARY_MSB, 4, 1, "dec_digit"),
                      PT(PORT_ONEHOT, 10, 1, "dec_symbol")) != 0) {
        fprintf(stderr, "FAIL: dec_to_symbol port contract mismatch.\n");
        goto cleanup;
    }
    loss = btn_train_dynamic(&dec_to_sym, &ds_inputs[0][0], &ds_targets[0][0],
                             10, 80000, 1000, 0.003, 0.01);
    printf("dec_to_symbol: hidden %lu, final loss %.6f\n",
           (unsigned long)dec_to_sym.hidden_count, loss);
    if (freeze_gate(&dec_to_sym, &ds_inputs[0][0], &ds_targets[0][0],
                    10, "dec_to_symbol") != 0) {
        goto cleanup;
    }
    if (btn_save(&dec_to_sym, "dec_to_symbol_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not persist dec_to_symbol.\n");
        goto cleanup;
    }
    remove("dec_to_symbol_stats.txt");
    if (emit_contract("dec_to_symbol", &dec_to_sym, &ds_inputs[0][0],
                      &ds_targets[0][0], 10,
                      "dec_to_symbol_contract.txt") != 0) {
        fprintf(stderr, "FAIL: could not emit dec_to_symbol contract.\n");
        goto cleanup;
    }

    /* dec_swap_ab: (a, b, c) -> (b, a, c); per-unit permutation. */
    if (btn_init(&dec_swap, 21, 21, 21, 96, 0.8, 227u) != 0) {
        fprintf(stderr, "FAIL: could not initialize dec_swap_ab.\n");
        goto cleanup;
    }
    {
        Port sw_in[3];
        Port sw_out[3];
        sw_in[0] = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        sw_in[1] = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        sw_in[2] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        sw_out[0] = sw_in[0];
        sw_out[1] = sw_in[1];
        sw_out[2] = sw_in[2];
        if (btn_set_io_ports(&dec_swap, sw_in, 3, sw_out, 3) != 0) {
            fprintf(stderr, "FAIL: dec_swap_ab port contract mismatch.\n");
            goto cleanup;
        }
    }
    /* Full 200-row permutation domain — train every row (see dec_full_add). */
    loss = btn_train_dynamic_spec(&dec_swap, &sw_inputs[0][0], &sw_targets[0][0],
                                  200, 150000, 1000, 0.0008, 0.03);
    printf("dec_swap_ab: hidden %lu, final loss %.6f\n",
           (unsigned long)dec_swap.hidden_count, loss);
    if (freeze_gate(&dec_swap, &sw_inputs[0][0], &sw_targets[0][0],
                    200, "dec_swap_ab") != 0) {
        goto cleanup;
    }
    if (btn_save(&dec_swap, "dec_swap_ab_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not persist dec_swap_ab.\n");
        goto cleanup;
    }
    remove("dec_swap_ab_stats.txt");
    if (emit_contract("dec_swap_ab", &dec_swap, &sw_inputs[0][0],
                      &sw_targets[0][0], 200,
                      "dec_swap_ab_contract.txt") != 0) {
        fprintf(stderr, "FAIL: could not emit dec_swap_ab contract.\n");
        goto cleanup;
    }

    /* ================================================================== */
    printf("\n=== Act 2: the planner discovers the decimal adder ===\n\n");
    /* ================================================================== */
    {
        PrimitiveRegistry reg;
        double out4[4];
        int a, b, c;
        int failures = 0;

        registry_init(&reg);
        if (registry_add(&reg, &dec_value, "dec_value") != 0 ||
            registry_add(&reg, &dec_fa, "dec_full_add") != 0) {
            fprintf(stderr, "FAIL: could not register decimal primitives.\n");
            registry_free(&reg);
            goto cleanup;
        }

        add_sources[0].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        add_sources[0].values = a_sym;
        add_sources[1].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        add_sources[1].values = b_sym;
        add_sources[2].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        add_sources[2].values = cin_v;

        /* The goal must carry the dec_sum tag: an untagged BINARY_MSB 4
           goal would be satisfied by dec_value's dec_digit output alone. */
        if (dag_plan(&reg, add_sources, 3,
                     PT(PORT_BINARY_MSB, 4, 1, "dec_sum"), &add_plan) != 0) {
            fprintf(stderr, "FAIL: no plan for the dec_sum goal.\n");
            registry_free(&reg);
            goto cleanup;
        }
        add_planned = 1;
        printf("discovered plan:\n");
        print_node(add_plan.root, 1);

        if (add_plan.root->kind != DAG_PRIMITIVE ||
            add_plan.root->btn != &dec_fa ||
            add_plan.root->child_count != 3 ||
            add_plan.root->children[0]->kind != DAG_PRIMITIVE ||
            add_plan.root->children[0]->btn != &dec_value ||
            add_plan.root->children[1]->kind != DAG_PRIMITIVE ||
            add_plan.root->children[1]->btn != &dec_value ||
            add_plan.root->children[2]->kind != DAG_SOURCE ||
            add_plan.root->children[2]->source_index != 2) {
            fprintf(stderr, "FAIL: unexpected plan shape.\n");
            registry_free(&reg);
            goto cleanup;
        }

        for (a = 0; a < 10; ++a) {
            for (b = 0; b < 10; ++b) {
                for (c = 0; c < 2; ++c) {
                    int_to_onehot(a, a_sym, 10);
                    int_to_onehot(b, b_sym, 10);
                    cin_v[0] = (double)c;
                    if (dag_execute(&add_plan, add_sources, 3, out4, 4) != 0 ||
                        bits_to_int(out4, 4) != (a + b + c) % 10) {
                        ++failures;
                    }
                }
            }
        }
        registry_free(&reg);
        if (failures != 0) {
            fprintf(stderr, "FAIL: discovered adder misfired %d/200.\n",
                    failures);
            goto cleanup;
        }
        printf("\nexecuted the full domain: 200/200 correct "
               "(sum = (a + b + cin) mod 10)\n");
    }

    /* ================================================================== */
    printf("\n=== Act 3: the dec_digit tag refuses hex nibbles ===\n\n");
    /* ================================================================== */
    {
        PrimitiveRegistry reg;
        static double hex_a[16], hex_b[16];
        DagSource hex_sources[3];
        DagPlan refused = {0};
        RoutePlan route = {0};

        if (btn_load(&hex_value, "hex_value_weights.txt") != 0) {
            fprintf(stderr, "FAIL: could not load hex_value "
                            "(run ./nn_demo first).\n");
            goto cleanup;
        }

        registry_init(&reg);
        if (registry_add(&reg, &dec_value, "dec_value") != 0 ||
            registry_add(&reg, &dec_fa, "dec_full_add") != 0 ||
            registry_add(&reg, &hex_value, "hex_value") != 0) {
            fprintf(stderr, "FAIL: could not build the mixed registry.\n");
            registry_free(&reg);
            goto cleanup;
        }

        /* hex_value emits BINARY_MSB 4 tagged nibble_value -- the same
           representation as dec_digit. Only the tag stands between a hex
           nibble and a decimal adder. */
        hex_sources[0].type = PT(PORT_ONEHOT, 16, 1, "hex_digit");
        hex_sources[0].values = hex_a;
        hex_sources[1].type = PT(PORT_ONEHOT, 16, 1, "hex_digit");
        hex_sources[1].values = hex_b;
        hex_sources[2].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        hex_sources[2].values = cin_v;

        if (dag_plan(&reg, hex_sources, 3,
                     PT(PORT_BINARY_MSB, 4, 1, "dec_sum"), &refused) == 0) {
            fprintf(stderr, "FAIL: hex sources planned into a decimal "
                            "adder.\n");
            dag_free(&refused);
            registry_free(&reg);
            goto cleanup;
        }
        printf("dag_plan(hex_digit sources -> dec_sum): refused\n");

        if (route_plan(&reg, PT(PORT_ONEHOT, 16, 1, "hex_digit"),
                       PT(PORT_BINARY_MSB, 4, 1, "dec_digit"),
                       &route) == 0) {
            fprintf(stderr, "FAIL: routed a hex digit into dec_digit.\n");
            registry_free(&reg);
            goto cleanup;
        }
        printf("route_plan(hex_digit -> dec_digit): refused\n");

        if (route_plan(&reg, PT(PORT_ONEHOT, 10, 1, "dec_symbol"),
                       PT(PORT_BINARY_MSB, 4, 1, "dec_digit"),
                       &route) != 0 ||
            route.length != 1 || route.steps[0] != &dec_value) {
            fprintf(stderr, "FAIL: the legitimate decimal route is "
                            "missing.\n");
            registry_free(&reg);
            goto cleanup;
        }
        printf("route_plan(dec_symbol -> dec_digit): dec_value (1 hop)\n");
        registry_free(&reg);
    }

    /* ================================================================== */
    printf("\n=== Act 4: ripple-carry as a program over primitives ===\n\n");
    /* ================================================================== */
    {
        static double a0_v[4], a1_v[4], b0_v[4], b1_v[4], rc_cin[1];
        DagSource rsrc[5];
        DagNode s_a0, s_a1, s_b0, s_b1, s_cin;
        DagNode ones_sum, ones_carry, tens_sum, tens_carry;
        DagPlan p_sum0 = {0}, p_sum1 = {0}, p_cout = {0};
        static const int cases[][3] = {
            {47, 85, 0}, {99, 99, 1}, {5, 7, 0}, {60, 40, 0},
        };
        size_t n_cases = sizeof(cases) / sizeof(cases[0]);
        size_t i;

        rsrc[0].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        rsrc[0].values = a0_v;
        rsrc[1].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        rsrc[1].values = a1_v;
        rsrc[2].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        rsrc[2].values = b0_v;
        rsrc[3].type = PT(PORT_BINARY_MSB, 4, 1, "dec_digit");
        rsrc[3].values = b1_v;
        rsrc[4].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        rsrc[4].values = rc_cin;

        memset(&s_a0, 0, sizeof s_a0);
        s_a0.kind = DAG_SOURCE;
        s_a0.source_index = 0;
        memset(&s_a1, 0, sizeof s_a1);
        s_a1.kind = DAG_SOURCE;
        s_a1.source_index = 1;
        memset(&s_b0, 0, sizeof s_b0);
        s_b0.kind = DAG_SOURCE;
        s_b0.source_index = 2;
        memset(&s_b1, 0, sizeof s_b1);
        s_b1.kind = DAG_SOURCE;
        s_b1.source_index = 3;
        memset(&s_cin, 0, sizeof s_cin);
        s_cin.kind = DAG_SOURCE;
        s_cin.source_index = 4;

        /* ones place, sum projection */
        memset(&ones_sum, 0, sizeof ones_sum);
        ones_sum.kind = DAG_PRIMITIVE;
        ones_sum.btn = &dec_fa;
        ones_sum.name = "dec_full_add";
        ones_sum.output_index = 0;
        ones_sum.children[0] = &s_a0;
        ones_sum.children[1] = &s_b0;
        ones_sum.children[2] = &s_cin;
        ones_sum.child_count = 3;

        /* ones place again, carry projection: carry_out -> carry_in is the
           handoff the matching dec_carry tags exist for */
        ones_carry = ones_sum;
        ones_carry.output_index = 1;

        /* tens place over the rippled carry */
        memset(&tens_sum, 0, sizeof tens_sum);
        tens_sum.kind = DAG_PRIMITIVE;
        tens_sum.btn = &dec_fa;
        tens_sum.name = "dec_full_add";
        tens_sum.output_index = 0;
        tens_sum.children[0] = &s_a1;
        tens_sum.children[1] = &s_b1;
        tens_sum.children[2] = &ones_carry;
        tens_sum.child_count = 3;

        tens_carry = tens_sum;
        tens_carry.output_index = 1;

        p_sum0.root = &ones_sum;
        p_sum0.strict = 1;
        p_sum1.root = &tens_sum;
        p_sum1.strict = 1;
        p_cout.root = &tens_carry;
        p_cout.strict = 1;

        for (i = 0; i < n_cases; ++i) {
            int a = cases[i][0];
            int b = cases[i][1];
            int cin = cases[i][2];
            double s0[4], s1[4], co[1];
            int result;

            int_to_bits(a % 10, a0_v, 4);
            int_to_bits(a / 10, a1_v, 4);
            int_to_bits(b % 10, b0_v, 4);
            int_to_bits(b / 10, b1_v, 4);
            rc_cin[0] = (double)cin;

            if (dag_execute(&p_sum0, rsrc, 5, s0, 4) != 0 ||
                dag_execute(&p_sum1, rsrc, 5, s1, 4) != 0 ||
                dag_execute(&p_cout, rsrc, 5, co, 1) != 0) {
                fprintf(stderr, "FAIL: strict ripple execution aborted.\n");
                goto cleanup;
            }
            result = bits_to_int(co, 1) * 100 +
                     bits_to_int(s1, 4) * 10 +
                     bits_to_int(s0, 4);
            printf("%2d + %2d + %d = %3d %s\n", a, b, cin, result,
                   result == a + b + cin ? "ok" : "WRONG");
            if (result != a + b + cin) {
                fprintf(stderr, "FAIL: ripple-carry add is wrong.\n");
                goto cleanup;
            }
        }
        printf("\n(the exhaustive 20000-case sweep lives in test_decimal)\n");
    }

    /* ================================================================== */
    printf("\n=== Act 5: consolidate, certify, replan ===\n\n");
    /* ================================================================== */
    {
        ConsolidateReport crep;
        Contract emitted = {0};
        PrimitiveRegistry reg;
        DagPlan replanned = {0};
        double out4[4];
        int spot_a[3] = {7, 9, 0};
        int spot_b[3] = {8, 9, 0};
        int spot_c[3] = {1, 1, 0};
        int i;

        if (consolidate_dag(&add_plan, add_sources, 3, NULL,
                            &chunk, &crep) != 0) {
            fprintf(stderr, "FAIL: consolidation refused "
                            "(samples %lu, aborts %lu, verified %lu).\n",
                    (unsigned long)crep.samples,
                    (unsigned long)crep.teacher_aborts,
                    (unsigned long)crep.verified);
            goto cleanup;
        }
        printf("dec_add_unit distilled: %lu samples (%lu teacher aborts), "
               "verified %lu/%lu, hidden %lu\n",
               (unsigned long)crep.samples,
               (unsigned long)crep.teacher_aborts,
               (unsigned long)crep.verified,
               (unsigned long)crep.samples,
               (unsigned long)chunk.hidden_count);
        if (crep.teacher_aborts != 0 || crep.verified != 200) {
            fprintf(stderr, "FAIL: expected a clean 200/200 distillation.\n");
            goto cleanup;
        }
        if (btn_save(&chunk, "dec_add_unit_weights.txt") != 0) {
            fprintf(stderr, "FAIL: could not persist dec_add_unit.\n");
            goto cleanup;
        }
        remove("dec_add_unit_stats.txt");

        /* The teacher's contract: emitted from the SAME proven plan. */
        if (contract_from_dag(&add_plan, add_sources, 3, "dec_add_unit",
                              4096, &emitted) != 0 ||
            contract_save(&emitted, "dec_add_unit_contract.txt") != 0) {
            fprintf(stderr, "FAIL: could not emit the teacher contract.\n");
            contract_free(&emitted);
            goto cleanup;
        }
        contract_free(&emitted);

        /* Reload every contract from disk and build a certified-only
           registry: tags are EARNED here, not asserted. */
        if (contract_load(&c_value, "dec_value_contract.txt") != 0) {
            fprintf(stderr, "FAIL: could not reload dec_value contract.\n");
            goto cleanup;
        }
        c_value_loaded = 1;
        if (contract_load(&c_fa, "dec_full_add_contract.txt") != 0) {
            fprintf(stderr, "FAIL: could not reload dec_full_add "
                            "contract.\n");
            goto cleanup;
        }
        c_fa_loaded = 1;
        if (contract_load(&c_chunk, "dec_add_unit_contract.txt") != 0) {
            fprintf(stderr, "FAIL: could not reload dec_add_unit "
                            "contract.\n");
            goto cleanup;
        }
        c_chunk_loaded = 1;

        registry_init(&reg);
        if (registry_add_certified(&reg, &dec_value, "dec_value",
                                   &c_value) != 0 ||
            registry_add_certified(&reg, &dec_fa, "dec_full_add",
                                   &c_fa) != 0 ||
            registry_add_certified(&reg, &chunk, "dec_add_unit",
                                   &c_chunk) != 0) {
            fprintf(stderr, "FAIL: a decimal primitive failed "
                            "certification.\n");
            registry_free(&reg);
            goto cleanup;
        }
        printf("dec_value, dec_full_add, dec_add_unit: CERTIFIED\n");
        reg.require_certified = 1;

        /* The chunk's verification-seeded evidence (200/0) outweighs the
           tree's earned evidence: one step at ~0.995 beats three. */
        if (dag_plan(&reg, add_sources, 3,
                     PT(PORT_BINARY_MSB, 4, 1, "dec_sum"),
                     &replanned) != 0) {
            fprintf(stderr, "FAIL: no certified replan.\n");
            registry_free(&reg);
            goto cleanup;
        }
        printf("\nreplanned (require_certified):\n");
        print_node(replanned.root, 1);
        if (replanned.root->kind != DAG_PRIMITIVE ||
            replanned.root->btn != &chunk ||
            replanned.root->children[0]->kind != DAG_SOURCE ||
            replanned.root->children[1]->kind != DAG_SOURCE ||
            replanned.root->children[2]->kind != DAG_SOURCE) {
            fprintf(stderr, "FAIL: the chunk did not win the replan.\n");
            dag_free(&replanned);
            registry_free(&reg);
            goto cleanup;
        }

        for (i = 0; i < 3; ++i) {
            int_to_onehot(spot_a[i], a_sym, 10);
            int_to_onehot(spot_b[i], b_sym, 10);
            cin_v[0] = (double)spot_c[i];
            if (dag_execute(&replanned, add_sources, 3, out4, 4) != 0 ||
                bits_to_int(out4, 4) !=
                    (spot_a[i] + spot_b[i] + spot_c[i]) % 10) {
                fprintf(stderr, "FAIL: the chunk misfired on %d+%d+%d.\n",
                        spot_a[i], spot_b[i], spot_c[i]);
                dag_free(&replanned);
                registry_free(&reg);
                goto cleanup;
            }
        }
        printf("chunk spot checks: 7+8+1 -> 6, 9+9+1 -> 9, 0+0+0 -> 0\n");
        dag_free(&replanned);
        registry_free(&reg);
    }

    /* ================================================================== */
    printf("\n=== Act 6: decimal laws ===\n\n");
    /* ================================================================== */
    {
        Property law_round, law_comm, loaded;
        PropertyReport prep;
        PrimitiveRegistry reg;

        registry_init(&reg);
        if (registry_add(&reg, &dec_value, "dec_value") != 0 ||
            registry_add(&reg, &dec_to_sym, "dec_to_symbol") != 0 ||
            registry_add(&reg, &dec_swap, "dec_swap_ab") != 0 ||
            registry_add(&reg, &chunk, "dec_add_unit") != 0) {
            fprintf(stderr, "FAIL: could not build the law registry.\n");
            registry_free(&reg);
            goto cleanup;
        }

        /* Law 1: dec_to_symbol(dec_value(s)) = identity on digit symbols. */
        memset(&law_round, 0, sizeof law_round);
        strcpy(law_round.name, "dec_value_roundtrip");
        law_round.sources[0] = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        law_round.source_count = 1;
        strcpy(law_round.lhs[0], "dec_value");
        strcpy(law_round.lhs[1], "dec_to_symbol");
        law_round.lhs_len = 2;
        law_round.rhs_len = 0;

        if (property_save(&law_round, "dec_value_roundtrip_property.txt") != 0) {
            fprintf(stderr, "FAIL: could not save the roundtrip law.\n");
            registry_free(&reg);
            goto cleanup;
        }
        memset(&loaded, 0, sizeof loaded);
        memset(&prep, 0, sizeof prep);
        if (property_load(&loaded, "dec_value_roundtrip_property.txt") != 0 ||
            property_check(&loaded, &reg, 4096, &prep) != 0 ||
            prep.inputs != 10) {
            fprintf(stderr, "FAIL: dec_value_roundtrip did not hold "
                            "(inputs=%lu held=%lu violated=%lu).\n",
                    (unsigned long)prep.inputs,
                    (unsigned long)prep.held,
                    (unsigned long)prep.violated);
            registry_free(&reg);
            goto cleanup;
        }
        printf("dec_value_roundtrip : HOLDS (%lu/%lu)\n",
               (unsigned long)prep.held, (unsigned long)prep.inputs);

        /* Law 2: add(a, b, c) = add(b, a, c). Property chains hand off the
           whole port sequence, so commutativity is LHS [dec_add_unit] vs
           RHS [dec_swap_ab, dec_add_unit] over (symbol, symbol, carry). */
        memset(&law_comm, 0, sizeof law_comm);
        strcpy(law_comm.name, "dec_add_commutes");
        law_comm.sources[0] = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        law_comm.sources[1] = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
        law_comm.sources[2] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
        law_comm.source_count = 3;
        strcpy(law_comm.lhs[0], "dec_add_unit");
        law_comm.lhs_len = 1;
        strcpy(law_comm.rhs[0], "dec_swap_ab");
        strcpy(law_comm.rhs[1], "dec_add_unit");
        law_comm.rhs_len = 2;

        if (property_save(&law_comm, "dec_add_commutes_property.txt") != 0) {
            fprintf(stderr, "FAIL: could not save the commutativity law.\n");
            registry_free(&reg);
            goto cleanup;
        }
        memset(&loaded, 0, sizeof loaded);
        memset(&prep, 0, sizeof prep);
        if (property_load(&loaded, "dec_add_commutes_property.txt") != 0 ||
            property_check(&loaded, &reg, 4096, &prep) != 0 ||
            prep.inputs != 200) {
            fprintf(stderr, "FAIL: dec_add_commutes did not hold "
                            "(inputs=%lu held=%lu violated=%lu).\n",
                    (unsigned long)prep.inputs,
                    (unsigned long)prep.held,
                    (unsigned long)prep.violated);
            registry_free(&reg);
            goto cleanup;
        }
        printf("dec_add_commutes    : HOLDS (%lu/%lu)\n",
               (unsigned long)prep.held, (unsigned long)prep.inputs);
        registry_free(&reg);
    }

    printf("\nAll decimal acts passed.\n");
    rc = EXIT_SUCCESS;

cleanup:
    if (add_planned) {
        dag_free(&add_plan);
    }
    if (c_value_loaded) {
        contract_free(&c_value);
    }
    if (c_fa_loaded) {
        contract_free(&c_fa);
    }
    if (c_chunk_loaded) {
        contract_free(&c_chunk);
    }
    btn_free(&dec_value);
    btn_free(&dec_fa);
    btn_free(&dec_to_sym);
    btn_free(&dec_swap);
    btn_free(&chunk);
    btn_free(&hex_value);
    return rc;
}

