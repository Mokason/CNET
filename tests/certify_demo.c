/*
 * End-to-end certification demonstration.
 *
 * (1) Loads the frozen primitives AND their nn_demo-emitted contracts,
 *     certifies each against its own spec, and prints the verdict table.
 * (2) Imposter theater: increment claims hex_value's contract --
 *     registry_add_certified refuses (signature gate); a tampered
 *     hex_value contract is denied (behavior gate).
 * (3) Plans hex_digit -> incremented value with require_certified set:
 *     the returned chain is certified end-to-end; executes 16/16.
 * (4) Emits the contract of combine(hex_value, hex_value) from the proven
 *     plan, distills the chunk from the same plan, and certifies the
 *     chunk against the emitted contract -- the loop closes.
 *
 * Requires weight + contract files from ./nn_demo. Run from the repo
 * root (make certify).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"
#include "../include/contract/contract.h"

#include <stdio.h>
#include <string.h>

static Port P(PortFamily family, size_t field_width, size_t field_count) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    return p;
}

static int bits_to_int(const double *values, size_t n) {
    int value = 0;
    size_t i;

    for (i = 0; i < n; ++i) {
        value = (value << 1) | (values[i] >= 0.5 ? 1 : 0);
    }
    return value;
}

int main(void) {
    static const char digits[] = "0123456789ABCDEF";

    /* ---- primitives ---- */
    BinaryTransformNetwork hexval  = {0};
    BinaryTransformNetwork incr    = {0};
    BinaryTransformNetwork combine = {0};
    BinaryTransformNetwork split   = {0};

    /* ---- contracts (loaded from disk) ---- */
    Contract hexval_contract  = {0};
    Contract incr_contract    = {0};
    Contract combine_contract = {0};
    Contract split_contract   = {0};

    PrimitiveRegistry reg;
    int rc = 0;

    /* ------------------------------------------------------------------ */
    /* Load frozen weights                                                  */
    /* ------------------------------------------------------------------ */
    if (btn_load(&hexval,  "hex_value_weights.txt")  != 0 ||
        btn_load(&incr,    "increment_weights.txt")  != 0 ||
        btn_load(&combine, "combine_weights.txt")    != 0 ||
        btn_load(&split,   "split_weights.txt")      != 0) {
        fprintf(stderr, "FAIL: could not load frozen primitives "
                        "(run ./nn_demo first).\n");
        return 1;
    }

    /* ---- load contracts ---- */
    if (contract_load(&hexval_contract,  "hex_value_contract.txt")  != 0 ||
        contract_load(&incr_contract,    "increment_contract.txt")  != 0 ||
        contract_load(&combine_contract, "combine_contract.txt")    != 0 ||
        contract_load(&split_contract,   "split_contract.txt")      != 0) {
        fprintf(stderr, "FAIL: could not load contracts "
                        "(run ./nn_demo first).\n");
        btn_free(&hexval);
        btn_free(&incr);
        btn_free(&combine);
        btn_free(&split);
        return 1;
    }

    /* ================================================================== */
    /* Part 1: certify each primitive against its own contract             */
    /* ================================================================== */
    printf("=== Part 1: certification table ===\n\n");
    {
        struct { BinaryTransformNetwork *btn; Contract *c;
                 const char *pname; const char *cname; } checks[4] = {
            { &hexval,  &hexval_contract,  "hex_value", "hex_value_contract"  },
            { &incr,    &incr_contract,    "increment", "increment_contract"  },
            { &combine, &combine_contract, "combine",   "combine_contract"    },
            { &split,   &split_contract,   "split",     "split_contract"      }
        };
        size_t k;

        for (k = 0; k < 4; ++k) {
            CertifyReport rep = {0};
            int verdict = btn_certify(checks[k].btn, checks[k].c, &rep);
            printf("  %-12s vs %-24s: %s (%lu/%lu)\n",
                   checks[k].pname, checks[k].cname,
                   verdict == 0 ? "CERTIFIED" : "DENIED",
                   (unsigned long)rep.passed,
                   (unsigned long)rep.exemplars);
            if (verdict != 0) {
                fprintf(stderr, "FAIL: %s did not certify against %s.\n",
                        checks[k].pname, checks[k].cname);
                rc = 1;
            }
        }
    }
    if (rc != 0) {
        goto cleanup;
    }
    printf("\n");

    /* ================================================================== */
    /* Part 2: imposter theater                                            */
    /* ================================================================== */
    printf("=== Part 2: imposter theater ===\n\n");

    registry_init(&reg);

    /* (a) signature gate: increment tries to wear hex_value's contract */
    {
        size_t before = reg.count;
        int refused = registry_add_certified(&reg, &incr, "imposter",
                                             &hexval_contract);
        if (refused != -1 || reg.count != before) {
            fprintf(stderr, "FAIL: imposter was not refused (signature gate).\n");
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }
        printf("  imposter refused (signature gate): "
               "increment cannot wear hex_value's contract\n");
    }

    /* (b) behavior gate: tampered hex_value contract */
    {
        Contract tampered = {0};
        CertifyReport rep = {0};
        int denied;

        if (contract_load(&tampered, "hex_value_contract.txt") != 0) {
            fprintf(stderr, "FAIL: could not load hex_value_contract.txt "
                            "for tampering.\n");
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }
        /* flip one output value to break the behavior spec */
        tampered.outputs[5] = 1.0 - tampered.outputs[5];

        denied = btn_certify(&hexval, &tampered, &rep);
        if (denied != -1) {
            fprintf(stderr, "FAIL: tampered contract was not denied "
                            "(behavior gate).\n");
            rc = 1;
            contract_free(&tampered);
            registry_free(&reg);
            goto cleanup;
        }
        printf("  tampered spec denied (behavior gate): "
               "%lu/%lu exemplars pass\n",
               (unsigned long)rep.passed,
               (unsigned long)rep.exemplars);
        contract_free(&tampered);
    }
    printf("\n");

    /* ================================================================== */
    /* Part 3: certified end-to-end planning and execution                 */
    /* ================================================================== */
    printf("=== Part 3: certified end-to-end plan ===\n\n");
    {
        RoutePlan plan;
        memset(&plan, 0, sizeof plan);
        int failures = 0;
        int i;

        /* reg.count is 0 after the refused imposter -- add the real ones */
        if (registry_add_certified(&reg, &hexval, "hex_value",
                                   &hexval_contract) != 0) {
            fprintf(stderr, "FAIL: hex_value certified registration failed.\n");
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }
        if (registry_add_certified(&reg, &incr, "increment",
                                   &incr_contract) != 0) {
            fprintf(stderr, "FAIL: increment certified registration failed.\n");
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }

        reg.require_certified = 1;

        if (route_plan(&reg, P(PORT_ONEHOT, 16, 1),
                       P(PORT_BINARY_MSB, 5, 1), &plan) != 0) {
            fprintf(stderr, "FAIL: planner found no certified route.\n");
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }
        if (plan.length != 2) {
            fprintf(stderr, "FAIL: expected 2-hop chain, got %lu hops.\n",
                    (unsigned long)plan.length);
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }

        for (i = 0; i < 16; ++i) {
            double input[16] = {0};
            double output[5] = {0};
            int got;

            input[i] = 1.0;
            if (route_execute(&plan, input, 16, output, 5) != 0) {
                fprintf(stderr, "FAIL: execution failed for '%c'\n", digits[i]);
                ++failures;
                continue;
            }
            got = bits_to_int(output, 5);
            if (got != i + 1) {
                fprintf(stderr, "FAIL: %c -> %d, expected %d\n",
                        digits[i], got, i + 1);
                ++failures;
            }
        }
        if (failures != 0) {
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }
        printf("  certified end-to-end plan executed 16/16\n\n");
    }

    /* ================================================================== */
    /* Part 4: emit contract from DAG, distill chunk, certify the chunk    */
    /* ================================================================== */
    printf("=== Part 4: contract emission + chunk certification ===\n\n");
    {
        double hi_vec[16] = {0};
        double lo_vec[16] = {0};
        DagSource sources[2];
        DagPlan plan = {0};
        Contract emitted = {0};
        BinaryTransformNetwork chunk = {0};
        ConsolidateReport crep = {0};
        CertifyReport vrep = {0};

        sources[0].type   = P(PORT_ONEHOT, 16, 1);
        sources[0].values = hi_vec;
        sources[1].type   = P(PORT_ONEHOT, 16, 1);
        sources[1].values = lo_vec;

        /* turn the knob OFF so plain combine is visible */
        reg.require_certified = 0;
        if (registry_add(&reg, &combine, "combine") != 0) {
            fprintf(stderr, "FAIL: could not register combine.\n");
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }

        if (dag_plan(&reg, sources, 2,
                     P(PORT_BINARY_MSB, 8, 1), &plan) != 0) {
            fprintf(stderr, "FAIL: dag_plan found no plan.\n");
            rc = 1;
            registry_free(&reg);
            goto cleanup;
        }
        if (plan.root == NULL || plan.root->btn != &combine) {
            fprintf(stderr, "FAIL: expected combine at the DAG root.\n");
            rc = 1;
            dag_free(&plan);
            registry_free(&reg);
            goto cleanup;
        }

        /* emit the contract from the plan */
        if (contract_from_dag(&plan, sources, 2,
                              "hex_pair_to_byte", 4096, &emitted) != 0) {
            fprintf(stderr, "FAIL: contract_from_dag refused.\n");
            rc = 1;
            dag_free(&plan);
            registry_free(&reg);
            goto cleanup;
        }

        /* distill the chunk from the same plan */
        if (consolidate_dag(&plan, sources, 2, NULL, &chunk, &crep) != 0) {
            fprintf(stderr, "FAIL: consolidate_dag refused.\n");
            rc = 1;
            contract_free(&emitted);
            dag_free(&plan);
            registry_free(&reg);
            goto cleanup;
        }
        dag_free(&plan);

        /* certify the chunk against the emitted contract */
        if (btn_certify(&chunk, &emitted, &vrep) != 0) {
            fprintf(stderr,
                    "FAIL: chunk not certified against emitted contract "
                    "(%lu/%lu).\n",
                    (unsigned long)vrep.passed,
                    (unsigned long)vrep.exemplars);
            rc = 1;
            contract_free(&emitted);
            btn_free(&chunk);
            registry_free(&reg);
            goto cleanup;
        }
        printf("  chunk certified against its teacher's emitted contract "
               "(%lu/%lu)\n\n",
               (unsigned long)vrep.passed,
               (unsigned long)vrep.exemplars);

        contract_free(&emitted);
        btn_free(&chunk);
    }

    registry_free(&reg);

    /* ------------------------------------------------------------------ */
cleanup:
    contract_free(&hexval_contract);
    contract_free(&incr_contract);
    contract_free(&combine_contract);
    contract_free(&split_contract);
    btn_free(&hexval);
    btn_free(&incr);
    btn_free(&combine);
    btn_free(&split);

    if (rc == 0) {
        printf("CERTIFY PASS: tags are earned, imposters are refused, "
               "and plans can be certified end-to-end.\n");
        return 0;
    }
    printf("CERTIFY FAIL.\n");
    return 1;
}

