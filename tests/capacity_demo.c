/*
 * Chunk capacity, the hierarchical answer.
 *
 *   Part 1  dec_add2 planned as a circuit over the frozen, certified
 *           dec_full_adder_unit -- two executions, carry chained, found
 *           with require_certified set (certified end-to-end).
 *   Part 2  exhaustive strict verification: all 20,000 two-digit
 *           additions, TWO forward passes each (the raw primitive
 *           circuit needed six).
 *   Part 3  (--flat <width> <epochs> [shuffled]) ONE flat-student
 *           distillation attempt at a study-chosen config. Success ->
 *           save + certify + 1-execution replan; refusal -> the honest
 *           report row. No argv -> skipped with a pointer to make study.
 *
 * Requires dec_full_adder_unit_*.txt from ./circuit_demo. Run from the
 * repo root (make capacity).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"
#include "../include/contract/contract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        printf("%s\n", node->name != NULL ? node->name : "?");
        for (c = 0; c < node->child_count; ++c) {
            print_node(node->children[c], depth + 1);
        }
    }
}

int main(int argc, char **argv) {
    BinaryTransformNetwork unit = {0};
    Contract c_unit = {0};
    PrimitiveRegistry reg;
    static double a0_v[10], b0_v[10], a1_v[10], b1_v[10], cin_v[1];
    DagSource src[5];
    Port goals[3];
    CircuitPlan cp = {0};
    DagNode *ones;
    DagNode *tens;
    int rc = EXIT_FAILURE;
    int reg_live = 0;

    if (btn_load(&unit, "dec_full_adder_unit_weights.txt") != 0 ||
        contract_load(&c_unit, "dec_full_adder_unit_contract.txt") != 0) {
        fprintf(stderr, "FAIL: could not load dec_full_adder_unit "
                        "(run ./circuit_demo first).\n");
        btn_free(&unit);
        return EXIT_FAILURE;
    }

    /* ================================================================== */
    printf("=== Part 1: dec_add2 as a certified 2-chunk circuit ===\n\n");
    /* ================================================================== */
    registry_init(&reg);
    reg_live = 1;
    if (registry_add_certified(&reg, &unit, "dec_full_adder_unit",
                               &c_unit) != 0) {
        fprintf(stderr, "FAIL: the unit did not certify.\n");
        goto cleanup;
    }
    reg.require_certified = 1;

    src[0].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
    src[0].values = a0_v;
    src[1].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
    src[1].values = b0_v;
    src[2].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
    src[2].values = a1_v;
    src[3].type = PT(PORT_ONEHOT, 10, 1, "dec_symbol");
    src[3].values = b1_v;
    src[4].type = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");
    src[4].values = cin_v;
    goals[0] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
    goals[1] = PT(PORT_BINARY_MSB, 4, 1, "dec_sum");
    goals[2] = PT(PORT_BINARY_MSB, 1, 1, "dec_carry");

    if (dag_plan_circuit(&reg, src, 5, goals, 3, &cp) != 0) {
        fprintf(stderr, "FAIL: no 2-chunk circuit.\n");
        goto cleanup;
    }
    ones = (cp.roots[0]->children[2]->kind == DAG_SOURCE)
               ? cp.roots[0] : cp.roots[1];
    tens = (ones == cp.roots[0]) ? cp.roots[1] : cp.roots[0];
    if (cp.roots[0] == cp.roots[1] || ones->btn != &unit ||
        tens->btn != &unit ||
        ones->children[2]->kind != DAG_SOURCE ||
        tens->children[2] != ones ||
        cp.roots[2] != tens || cp.root_ports[2] != 1) {
        fprintf(stderr, "FAIL: unexpected circuit shape.\n");
        goto cleanup;
    }
    printf("certified circuit (TWO executions of the frozen unit):\n");
    print_node(tens, 1);

    /* ================================================================== */
    printf("\n=== Part 2: all 20,000 additions, strict, 2 forwards each "
           "===\n\n");
    /* ================================================================== */
    {
        long wrong = 0;
        int a, b, c;

        cp.strict = 1;
        for (a = 0; a < 100; ++a) {
            for (b = 0; b < 100; ++b) {
                for (c = 0; c < 2; ++c) {
                    double out[9];

                    int_to_onehot(a % 10, a0_v, 10);
                    int_to_onehot(b % 10, b0_v, 10);
                    int_to_onehot(a / 10, a1_v, 10);
                    int_to_onehot(b / 10, b1_v, 10);
                    cin_v[0] = (double)c;
                    if (dag_execute_circuit(&cp, src, 5, out, 9) != 0 ||
                        bits_to_int(out + 8, 1) * 100 +
                        bits_to_int(out + 4, 4) * 10 +
                        bits_to_int(out, 4) != a + b + c) {
                        ++wrong;
                    }
                }
            }
        }
        if (wrong != 0) {
            fprintf(stderr, "FAIL: chunked dec_add2 wrong on %ld/20000.\n",
                    wrong);
            goto cleanup;
        }
        printf("20000/20000 exact. 2 forward passes per addition "
               "(~6656 MACs); the raw\nprimitive circuit needed 6 "
               "executions, a flat w=256 student would cost 12800.\n");
    }

    /* ================================================================== */
    printf("\n=== Part 3: the flat attempt ===\n\n");
    /* ================================================================== */
    if (argc >= 4 && strcmp(argv[1], "--flat") == 0) {
        size_t width = strtoul(argv[2], NULL, 10);
        size_t epochs = strtoul(argv[3], NULL, 10);
        int shuffled = argc >= 5 && strcmp(argv[4], "shuffled") == 0;
        ConsolidateConfig cfg;
        ConsolidateReport rep;
        BinaryTransformNetwork flat = {0};

        consolidate_config_defaults(&cfg);
        cfg.max_samples = 20000;
        cfg.initial_hidden = width;
        cfg.max_hidden = width;       /* fixed width: never grows */
        cfg.max_epochs = epochs;
        cfg.growth_window = epochs;
        cfg.target_loss = 0.0008;
#ifdef CNET_HAVE_SHUFFLED_TRAINER
        cfg.use_shuffled_trainer = shuffled;
#else
        if (shuffled) {
            fprintf(stderr, "FAIL: shuffled trainer not built in.\n");
            goto cleanup;
        }
#endif
        printf("distilling flat dec_add2: width %lu, %lu epochs%s...\n",
               (unsigned long)width, (unsigned long)epochs,
               shuffled ? ", shuffled trainer" : "");
        memset(&rep, 0, sizeof rep);
        if (consolidate_circuit(&cp, src, 5, &cfg, &flat, &rep) == 0) {
            Contract emitted = {0};
            CertifyReport crep;

            printf("flat dec_add2 verified %lu/%lu -- saving and "
                   "certifying.\n",
                   (unsigned long)rep.verified, (unsigned long)rep.samples);
            if (btn_save(&flat, "dec_add2_weights.txt") != 0 ||
                contract_from_circuit(&cp, src, 5, "dec_add2", 20000,
                                      &emitted) != 0 ||
                contract_save(&emitted, "dec_add2_contract.txt") != 0) {
                fprintf(stderr, "FAIL: could not persist dec_add2.\n");
                contract_free(&emitted);
                btn_free(&flat);
                goto cleanup;
            }
            remove("dec_add2_stats.txt");
            if (btn_certify(&flat, &emitted, &crep) != 0 ||
                crep.passed != 20000) {
                fprintf(stderr, "FAIL: dec_add2 did not certify.\n");
                contract_free(&emitted);
                btn_free(&flat);
                goto cleanup;
            }
            printf("dec_add2: CERTIFIED (20000/20000).\n");
            contract_free(&emitted);

            /* The 1-execution replan. */
            {
                PrimitiveRegistry reg2;
                Contract c_flat = {0};
                CircuitPlan one = {0};

                registry_init(&reg2);
                if (contract_load(&c_flat, "dec_add2_contract.txt") != 0 ||
                    registry_add_certified(&reg2, &unit,
                                           "dec_full_adder_unit",
                                           &c_unit) != 0 ||
                    registry_add_certified(&reg2, &flat, "dec_add2",
                                           &c_flat) != 0) {
                    fprintf(stderr, "FAIL: replan registry.\n");
                    contract_free(&c_flat);
                    registry_free(&reg2);
                    btn_free(&flat);
                    goto cleanup;
                }
                reg2.require_certified = 1;
                if (dag_plan_circuit(&reg2, src, 5, goals, 3, &one) == 0 &&
                    one.roots[0]->btn == &flat) {
                    printf("certified replan: ONE execution.\n");
                } else {
                    printf("replan kept the 2-chunk circuit (evidence "
                           "decides; both are certified).\n");
                }
                circuit_free(&one);
                contract_free(&c_flat);
                registry_free(&reg2);
            }
            btn_free(&flat);
        } else {
            printf("flat dec_add2 REFUSED, honestly: %lu samples, "
                   "%lu verified, %lu missed (loss %.6f).\n"
                   "The hierarchical circuit above remains the answer: "
                   "capacity scales by\ncomposing certified chunks, not "
                   "by fattening students.\n",
                   (unsigned long)rep.samples,
                   (unsigned long)rep.verified,
                   (unsigned long)rep.missed,
                   rep.final_loss);
        }
    } else {
        printf("skipped (pass --flat <width> <epochs> [shuffled]; pick "
               "the config\nfrom make study).\n");
    }

    printf("\nAll capacity demo parts passed.\n");
    rc = EXIT_SUCCESS;

cleanup:
    circuit_free(&cp);
    if (reg_live) {
        registry_free(&reg);
    }
    contract_free(&c_unit);
    btn_free(&unit);
    return rc;
}

