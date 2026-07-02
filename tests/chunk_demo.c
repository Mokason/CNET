/*
 * End-to-end chunk consolidation demonstration.
 *
 * Two showcases over the real frozen primitives:
 *   1. The proven route hex_value -> increment is distilled into a single
 *      "hex_increment" chunk (16 enumerated inputs, verified 16/16). The
 *      verification outcome seeds the chunk's evidence, so route_plan -- with
 *      no planner changes -- now prefers the 1-step chunk over the 2-step
 *      chain it was distilled from.
 *   2. The flagship DAG combine(hex_value(hi), hex_value(lo)) is distilled
 *      into a two-input "hex_pair_to_byte" chunk (256 enumerated pairs).
 *      dag_plan then roots the replanned DAG at the chunk, fed directly by
 *      the sources: search depth 2 -> 1, three forwards -> one.
 *   3. The ladder: once split earns deployment evidence, the planner routes
 *      THROUGH the byte chunk inside a deeper plan -- split(hex_pair_to_byte)
 *      -- and that plan consolidates again into "hi_nibble_from_hex_pair",
 *      a chunk of a chunk.
 *
 * The chunks are saved as regular weight files + stats sidecars -- the next
 * session can load them like any other frozen primitive.
 *
 * Requires weight files from ./nn_demo. Run from the repo root (make chunk).
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"

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

static double route_score(const RoutePlan *plan) {
    double score = 1.0;
    size_t s;

    for (s = 0; s < plan->length; ++s) {
        score *= btn_reliability(plan->steps[s]);
    }
    return score;
}

static double dag_score(const DagNode *node) {
    double score = 1.0;
    size_t s;

    if (node == NULL || node->kind == DAG_SOURCE) {
        return 1.0;
    }
    score = btn_reliability(node->btn);
    for (s = 0; s < node->child_count; ++s) {
        score *= dag_score(node->children[s]);
    }
    return score;
}

static void print_node(const DagNode *node, int indent) {
    int i;

    for (i = 0; i < indent; ++i) {
        printf("  ");
    }
    if (node->kind == DAG_SOURCE) {
        printf("source[%d]\n", node->source_index);
    } else {
        size_t s;
        printf("%s\n", node->name);
        for (s = 0; s < node->child_count; ++s) {
            print_node(node->children[s], indent + 1);
        }
    }
}

static void print_report(const ConsolidateReport *rep) {
    printf("  distilled: %lu samples (%lu teacher aborts), verified %lu/%lu, "
           "loss %.6f\n",
           (unsigned long)rep->samples, (unsigned long)rep->teacher_aborts,
           (unsigned long)rep->verified,
           (unsigned long)(rep->verified + rep->missed), rep->final_loss);
}

static int demo_route_chunk(PrimitiveRegistry *reg,
                            BinaryTransformNetwork *chunk) {
    static const char digits[] = "0123456789ABCDEF";
    RoutePlan chain, replan;
    ConsolidateReport rep;
    int failures = 0;
    int i;
    size_t s;

    printf("=== route consolidation: hex digit -> incremented value ===\n\n");

    if (route_plan(reg, P(PORT_ONEHOT, 16, 1),
                   P(PORT_BINARY_MSB, 5, 1), &chain) != 0) {
        fprintf(stderr, "FAIL: planner found no teacher chain.\n");
        return 1;
    }
    printf("teacher chain (%lu hops, score %.4f):",
           (unsigned long)chain.length, route_score(&chain));
    for (s = 0; s < chain.length; ++s) {
        printf(" %s", chain.names[s]);
    }
    printf("\n");

    if (consolidate_route(&chain, NULL, chunk, &rep) != 0) {
        fprintf(stderr, "FAIL: consolidation refused.\n");
        print_report(&rep);
        return 1;
    }
    print_report(&rep);

    /* The chunk is an ordinary primitive: weight file + stats sidecar.
       Prior runs' accumulated evidence still describes these weights
       (distillation is deterministic), so restore it when present. */
    if (btn_save(chunk, "hex_increment_weights.txt") != 0) {
        fprintf(stderr, "WARN: could not save hex_increment weights.\n");
    }
    (void)btn_load_stats(chunk, "hex_increment_stats.txt");

    registry_add(reg, chunk, "hex_increment");
    if (route_plan(reg, P(PORT_ONEHOT, 16, 1),
                   P(PORT_BINARY_MSB, 5, 1), &replan) != 0) {
        fprintf(stderr, "FAIL: replanning found no route.\n");
        return 1;
    }
    printf("replanned (%lu hop, score %.4f):",
           (unsigned long)replan.length, route_score(&replan));
    for (s = 0; s < replan.length; ++s) {
        printf(" %s", replan.names[s]);
    }
    printf("\n");
    if (replan.length != 1 || replan.steps[0] != chunk) {
        fprintf(stderr, "FAIL: planner did not pick the chunk.\n");
        return 1;
    }

    printf("\nexecuting the chunk (one forward where the chain took two):\n");
    for (i = 0; i < 16; ++i) {
        double input[16] = {0};
        double output[5] = {0};
        int got;

        input[i] = 1.0;
        if (route_execute(&replan, input, 16, output, 5) != 0) {
            fprintf(stderr, "FAIL: execution failed for '%c'\n", digits[i]);
            ++failures;
            continue;
        }
        got = bits_to_int(output, 5);
        if (got != i + 1) {
            printf("  %c -> %2d | expected %2d <-- MISMATCH\n",
                   digits[i], got, i + 1);
            ++failures;
        }
    }
    if (failures == 0) {
        printf("  all 16 digits -> value+1 OK\n");
    }

    printf("\nchunk evidence (lifetime): %lu/%lu (successes/failures)\n",
           chunk->output_successes, chunk->output_failures);
    if (btn_save_stats(chunk, "hex_increment_stats.txt") != 0) {
        fprintf(stderr, "WARN: could not checkpoint chunk stats.\n");
    }
    return failures == 0 ? 0 : 1;
}

static int demo_dag_chunk(PrimitiveRegistry *reg,
                          BinaryTransformNetwork *chunk) {
    static const char *pairs[] = {"41", "FF", "00", "10", "A5", "DE"};
    static const char digits[] = "0123456789ABCDEF";
    const size_t n_pairs = sizeof(pairs) / sizeof(pairs[0]);
    double hi_vec[16], lo_vec[16];
    DagSource sources[2];
    DagPlan teacher = {0};
    DagPlan replan = {0};
    ConsolidateReport rep;
    int failures = 0;
    size_t k;
    int i;

    printf("\n=== DAG consolidation: two hex digits -> byte ===\n\n");

    for (i = 0; i < 16; ++i) {
        hi_vec[i] = 0.0;
        lo_vec[i] = 0.0;
    }
    sources[0].type = P(PORT_ONEHOT, 16, 1);
    sources[0].values = hi_vec;
    sources[1].type = P(PORT_ONEHOT, 16, 1);
    sources[1].values = lo_vec;

    if (dag_plan(reg, sources, 2, P(PORT_BINARY_MSB, 8, 1), &teacher) != 0) {
        fprintf(stderr, "FAIL: planner found no teacher DAG.\n");
        return 1;
    }
    printf("teacher DAG (score %.4f):\n", dag_score(teacher.root));
    print_node(teacher.root, 1);

    if (consolidate_dag(&teacher, sources, 2, NULL, chunk, &rep) != 0) {
        fprintf(stderr, "FAIL: DAG consolidation refused.\n");
        print_report(&rep);
        dag_free(&teacher);
        return 1;
    }
    print_report(&rep);
    dag_free(&teacher);

    if (btn_save(chunk, "hex_pair_to_byte_weights.txt") != 0) {
        fprintf(stderr, "WARN: could not save hex_pair_to_byte weights.\n");
    }
    (void)btn_load_stats(chunk, "hex_pair_to_byte_stats.txt");

    registry_add(reg, chunk, "hex_pair_to_byte");
    if (dag_plan(reg, sources, 2, P(PORT_BINARY_MSB, 8, 1), &replan) != 0) {
        fprintf(stderr, "FAIL: replanning found no DAG.\n");
        return 1;
    }
    printf("replanned DAG (score %.4f):\n", dag_score(replan.root));
    print_node(replan.root, 1);
    if (replan.root == NULL || replan.root->btn != chunk) {
        fprintf(stderr, "FAIL: planner did not root at the chunk.\n");
        dag_free(&replan);
        return 1;
    }

    printf("\nexecuting the chunk (one forward where the DAG took three):\n");
    for (k = 0; k < n_pairs; ++k) {
        double output[8] = {0};
        int hi = pairs[k][0] >= 'A' ? pairs[k][0] - 'A' + 10
                                    : pairs[k][0] - '0';
        int lo = pairs[k][1] >= 'A' ? pairs[k][1] - 'A' + 10
                                    : pairs[k][1] - '0';
        int got;
        int expected = hi * 16 + lo;

        for (i = 0; i < 16; ++i) {
            hi_vec[i] = (i == hi) ? 1.0 : 0.0;
            lo_vec[i] = (i == lo) ? 1.0 : 0.0;
        }
        if (dag_execute(&replan, sources, 2, output, 8) != 0) {
            fprintf(stderr, "FAIL: execution failed for pair %s\n", pairs[k]);
            ++failures;
            continue;
        }
        got = bits_to_int(output, 8);
        printf("  %c%c -> 0x%02X (%3d) | expected 0x%02X (%3d) %s\n",
               digits[hi], digits[lo], got, got, expected, expected,
               got == expected ? "OK" : "<-- MISMATCH");
        if (got != expected) {
            ++failures;
        }
    }
    dag_free(&replan);

    printf("\nchunk evidence (lifetime): %lu/%lu (successes/failures)\n",
           chunk->output_successes, chunk->output_failures);
    if (btn_save_stats(chunk, "hex_pair_to_byte_stats.txt") != 0) {
        fprintf(stderr, "WARN: could not checkpoint chunk stats.\n");
    }
    return failures == 0 ? 0 : 1;
}

/* Act 3, the ladder: with deployment evidence, the planner routes THROUGH
   the byte chunk inside a deeper plan, and that plan consolidates again. */
static int demo_deep_chunk(PrimitiveRegistry *reg,
                           BinaryTransformNetwork *hexval,
                           BinaryTransformNetwork *split,
                           BinaryTransformNetwork *byte_chunk,
                           BinaryTransformNetwork *chunk) {
    static const char digits[] = "0123456789ABCDEF";
    double hi_vec[16], lo_vec[16];
    DagSource sources[2];
    DagPlan plan = {0};
    ConsolidateReport rep;
    int failures = 0;
    int i;

    printf("\n=== deeper plan: chunk as interior member, then chunk of chunk "
           "===\n\n");

    for (i = 0; i < 16; ++i) {
        hi_vec[i] = 0.0;
        lo_vec[i] = 0.0;
    }
    sources[0].type = P(PORT_ONEHOT, 16, 1);
    sources[0].values = hi_vec;
    sources[1].type = P(PORT_ONEHOT, 16, 1);
    sources[1].values = lo_vec;

    /* With no evidence for split, the deep path scores 0.5 * ~0.99 < 0.5:
       the planner honestly prefers the direct primitive. */
    if (dag_plan(reg, sources, 2, P(PORT_BINARY_MSB, 4, 1), &plan) != 0) {
        fprintf(stderr, "FAIL: no plan for the nibble goal.\n");
        return 1;
    }
    printf("goal nibble, split fresh (score %.4f):\n", dag_score(plan.root));
    print_node(plan.root, 1);
    if (plan.root->btn != hexval) {
        fprintf(stderr, "FAIL: expected the direct hex_value plan.\n");
        dag_free(&plan);
        return 1;
    }
    dag_free(&plan);

    /* Deployment evidence: run the deep plan by hand 16 times. split and
       the byte chunk earn evidence; the direct path earns none. */
    {
        DagNode s0, s1, chunk_node, split_node;
        DagPlan deep;
        double out[4];

        memset(&s0, 0, sizeof s0);
        s0.kind = DAG_SOURCE;
        s0.source_index = 0;
        memset(&s1, 0, sizeof s1);
        s1.kind = DAG_SOURCE;
        s1.source_index = 1;
        memset(&chunk_node, 0, sizeof chunk_node);
        chunk_node.kind = DAG_PRIMITIVE;
        chunk_node.btn = byte_chunk;
        chunk_node.name = "hex_pair_to_byte";
        chunk_node.children[0] = &s0;
        chunk_node.children[1] = &s1;
        chunk_node.child_count = 2;
        memset(&split_node, 0, sizeof split_node);
        split_node.kind = DAG_PRIMITIVE;
        split_node.btn = split;
        split_node.name = "split";
        split_node.output_index = 0;  /* hi nibble */
        split_node.children[0] = &chunk_node;
        split_node.child_count = 1;
        memset(&deep, 0, sizeof deep);
        deep.root = &split_node;

        for (i = 0; i < 16; ++i) {
            int hi = i;
            int lo = 15 - i;
            memset(hi_vec, 0, sizeof hi_vec);
            memset(lo_vec, 0, sizeof lo_vec);
            hi_vec[hi] = 1.0;
            lo_vec[lo] = 1.0;
            if (dag_execute(&deep, sources, 2, out, 4) != 0 ||
                bits_to_int(out, 4) != hi) {
                ++failures;
            }
        }
        if (failures != 0) {
            fprintf(stderr, "FAIL: deep plan misfired %d/16.\n", failures);
            return 1;
        }
        printf("\nran split(hex_pair_to_byte) by hand 16x: all correct; "
               "split evidence %lu/%lu\n",
               split->output_successes, split->output_failures);
    }

    /* Evidence flips the planner: the chunk becomes an INTERIOR member. */
    if (dag_plan(reg, sources, 2, P(PORT_BINARY_MSB, 4, 1), &plan) != 0) {
        fprintf(stderr, "FAIL: no replan for the nibble goal.\n");
        return 1;
    }
    printf("\nreplanned with evidence (score %.4f):\n", dag_score(plan.root));
    print_node(plan.root, 1);
    if (plan.root->btn != split ||
        plan.root->children[0]->btn != byte_chunk) {
        fprintf(stderr, "FAIL: planner did not route through the chunk.\n");
        dag_free(&plan);
        return 1;
    }

    /* Consolidate the deeper plan: a chunk distilled from a plan that
       already contains a chunk. */
    if (consolidate_dag(&plan, sources, 2, NULL, chunk, &rep) != 0) {
        fprintf(stderr, "FAIL: chunk-of-chunk consolidation refused.\n");
        print_report(&rep);
        dag_free(&plan);
        return 1;
    }
    print_report(&rep);
    dag_free(&plan);

    if (btn_save(chunk, "hi_nibble_from_hex_pair_weights.txt") != 0) {
        fprintf(stderr, "WARN: could not save hi_nibble weights.\n");
    }
    (void)btn_load_stats(chunk, "hi_nibble_from_hex_pair_stats.txt");

    registry_add(reg, chunk, "hi_nibble_from_hex_pair");
    if (dag_plan(reg, sources, 2, P(PORT_BINARY_MSB, 4, 1), &plan) != 0) {
        fprintf(stderr, "FAIL: no replan after chunk-of-chunk.\n");
        return 1;
    }
    printf("replanned again (score %.4f):\n", dag_score(plan.root));
    print_node(plan.root, 1);
    if (plan.root->btn != chunk) {
        fprintf(stderr, "FAIL: planner did not pick the chunk of chunk.\n");
        dag_free(&plan);
        return 1;
    }

    printf("\nexecuting the chunk of chunk (one forward where the ladder "
           "took two-then-four):\n");
    for (i = 0; i < 4; ++i) {
        static const int his[] = {0x4, 0xF, 0x0, 0xA};
        static const int los[] = {0x1, 0xF, 0x0, 0x5};
        double out[4];
        int got;

        memset(hi_vec, 0, sizeof hi_vec);
        memset(lo_vec, 0, sizeof lo_vec);
        hi_vec[his[i]] = 1.0;
        lo_vec[los[i]] = 1.0;
        if (dag_execute(&plan, sources, 2, out, 4) != 0) {
            fprintf(stderr, "FAIL: execution failed for %c%c\n",
                    digits[his[i]], digits[los[i]]);
            ++failures;
            continue;
        }
        got = bits_to_int(out, 4);
        printf("  %c%c -> hi nibble %c | expected %c %s\n",
               digits[his[i]], digits[los[i]], digits[got], digits[his[i]],
               got == his[i] ? "OK" : "<-- MISMATCH");
        if (got != his[i]) {
            ++failures;
        }
    }
    dag_free(&plan);

    printf("\nchunk evidence (lifetime): %lu/%lu (successes/failures)\n",
           chunk->output_successes, chunk->output_failures);
    if (btn_save_stats(chunk, "hi_nibble_from_hex_pair_stats.txt") != 0) {
        fprintf(stderr, "WARN: could not checkpoint chunk stats.\n");
    }
    return failures == 0 ? 0 : 1;
}

int main(void) {
    BinaryTransformNetwork hexval = {0};
    BinaryTransformNetwork incr = {0};
    BinaryTransformNetwork combine = {0};
    BinaryTransformNetwork split = {0};
    BinaryTransformNetwork route_chunk = {0};
    BinaryTransformNetwork dag_chunk = {0};
    BinaryTransformNetwork deep_chunk = {0};
    PrimitiveRegistry reg;
    int rc = 0;

    if (btn_load(&hexval, "hex_value_weights.txt") != 0 ||
        btn_load(&incr, "increment_weights.txt") != 0 ||
        btn_load(&combine, "combine_weights.txt") != 0 ||
        btn_load(&split, "split_weights.txt") != 0) {
        fprintf(stderr, "FAIL: could not load frozen primitives (run ./nn_demo "
                        "first).\n");
        return 1;
    }
    (void)btn_load_stats(&hexval, "hex_value_stats.txt");
    (void)btn_load_stats(&incr, "increment_stats.txt");

    registry_init(&reg);
    registry_add(&reg, &hexval, "hex_value");
    registry_add(&reg, &incr, "increment");
    registry_add(&reg, &combine, "combine");
    registry_add(&reg, &split, "split");

    rc |= demo_route_chunk(&reg, &route_chunk);
    rc |= demo_dag_chunk(&reg, &dag_chunk);
    rc |= demo_deep_chunk(&reg, &hexval, &split, &dag_chunk, &deep_chunk);

    registry_free(&reg);
    btn_free(&route_chunk);
    btn_free(&dag_chunk);
    btn_free(&deep_chunk);
    btn_free(&hexval);
    btn_free(&incr);
    btn_free(&combine);
    btn_free(&split);

    if (rc == 0) {
        printf("\nCHUNK PASS: proven plans distilled into single primitives "
               "(including a chunk of a chunk); the planner now reaches all "
               "three goals in one step.\n");
        return 0;
    }
    printf("\nCHUNK FAIL.\n");
    return 1;
}
