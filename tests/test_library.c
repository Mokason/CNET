/*
 * Tests for library_evolve: the DreamCoder-style library-learning loop.
 * Builds tiny synthetic primitives inline (the make-test idiom), so the
 * suite is self-contained, fast, and deterministic.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/consolidate.h"
#include "../include/contract/contract.h"
#include "../include/property.h"
#include "../include/library.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, desc) do {                          \
    if (cond) { printf("  ok   %s\n", (desc)); }        \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL) port_set_tag(&p, tag);
    return p;
}

static void msb2(int i, double *o) { o[0] = (double)((i >> 1) & 1); o[1] = (double)(i & 1); }
static void msb3(int i, double *o) { o[0] = (double)((i >> 2) & 1); o[1] = (double)((i >> 1) & 1); o[2] = (double)(i & 1); }

/* dec: ONEHOT4 -> BINARY_MSB2, i -> i. */
static int make_route_dec(BinaryTransformNetwork *b) {
    double in[4][4] = {{0}}; double tg[4][2]; int i;
    if (btn_init(b, 4, 2, 1, 16, 0.8, 11u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 4, 1, NULL),
                      PT(PORT_BINARY_MSB, 2, 1, NULL)) != 0) return -1;
    for (i = 0; i < 4; ++i) { in[i][i] = 1.0; msb2(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* inc: BINARY_MSB2 -> BINARY_MSB3, i -> i+1 (widens, so the goal port differs
   from the intermediate and only the two-hop chain reaches it). */
static int make_route_inc(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][3]; int i;
    if (btn_init(b, 2, 3, 1, 16, 0.8, 13u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, NULL),
                      PT(PORT_BINARY_MSB, 3, 1, NULL)) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb3(i + 1, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static void test_route_invention(void) {
    BinaryTransformNetwork dec = {0}, inc = {0};
    PrimitiveRegistry reg;
    LibraryTask task;
    LibraryReport rep1, rep2;
    printf("route invention + fixed point:\n");
    CHECK(make_route_dec(&dec) == 0 && make_route_inc(&inc) == 0,
          "base primitives train");
    registry_init(&reg);
    registry_add(&reg, &dec, "dec");
    registry_add(&reg, &inc, "inc");

    memset(&task, 0, sizeof task);
    task.name = "chunk_inc4";
    task.sources[0] = PT(PORT_ONEHOT, 4, 1, NULL);
    task.n_sources = 1;
    task.goal = PT(PORT_BINARY_MSB, 3, 1, NULL);

    CHECK(library_evolve(&reg, &task, 1, NULL, 0, NULL, 4, &rep1) == 0, "evolve runs");
    CHECK(rep1.chunk_count == 1, "one chunk invented");
    CHECK(rep1.rolled_back == 0, "no rollback");
    CHECK(strcmp(rep1.names[0], "chunk_inc4") == 0, "chunk named");
    CHECK(rep1.iterations_run == 2, "converged in 2 passes");
    CHECK(reg.count == 3, "registry grew to 3");

    {   /* the chunk computes index+1 */
        RoutePlan p; double in[4] = {0}; double out[3] = {0}; int got;
        CHECK(route_plan(&reg, PT(PORT_ONEHOT, 4, 1, NULL),
                         PT(PORT_BINARY_MSB, 3, 1, NULL), &p) == 0 && p.length == 1,
              "replans to the chunk");
        in[2] = 1.0;
        CHECK(route_execute(&p, in, 4, out, 3) == 0, "chunk executes");
        got = (out[0] > 0.5 ? 4 : 0) + (out[1] > 0.5 ? 2 : 0) + (out[2] > 0.5 ? 1 : 0);
        CHECK(got == 3, "one-hot 2 -> 3");
    }

    CHECK(library_evolve(&reg, &task, 1, NULL, 0, NULL, 4, &rep2) == 0, "second evolve runs");
    CHECK(rep2.chunk_count == 0, "no re-invention");
    CHECK(rep2.iterations_run == 1, "fixed point in one pass");

    registry_free(&reg);
    library_report_free(&rep1);
    library_report_free(&rep2);
    /* registry only borrows the base nets; free them last */
    btn_free(&dec);
    btn_free(&inc);
}

/* dec2: ONEHOT2 "bsym" -> BINARY_MSB1 "bit", i -> i. */
static int make_decoder2(BinaryTransformNetwork *b) {
    double in[2][2] = {{1.0, 0.0}, {0.0, 1.0}};
    double tg[2][1] = {{0.0}, {1.0}};
    if (btn_init(b, 2, 1, 1, 8, 0.8, 17u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 2, 1, "bsym"),
                      PT(PORT_BINARY_MSB, 1, 1, "bit")) != 0) return -1;
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 2,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* comb: [BINARY_MSB1 "bit", BINARY_MSB1 "bit"] -> BINARY_MSB2 "pair",
   (hi, lo) -> hi*2 + lo. */
static int make_combiner(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][2]; Port in_ports[2]; int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 19u) != 0) return -1;
    in_ports[0] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    in_ports[1] = PT(PORT_BINARY_MSB, 1, 1, "bit");
    if (btn_set_input_ports(b, in_ports, 2,
                            PT(PORT_BINARY_MSB, 2, 1, "pair")) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb2(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static void test_dag_invention(void) {
    BinaryTransformNetwork d2 = {0}, comb = {0};
    PrimitiveRegistry reg;
    LibraryTask task;
    LibraryReport rep;
    printf("dag invention:\n");
    CHECK(make_decoder2(&d2) == 0 && make_combiner(&comb) == 0,
          "base primitives train");
    registry_init(&reg);
    registry_add(&reg, &d2, "dec2");
    registry_add(&reg, &comb, "comb");

    memset(&task, 0, sizeof task);
    task.name = "chunk_pair";
    task.sources[0] = PT(PORT_ONEHOT, 2, 1, "bsym");
    task.sources[1] = PT(PORT_ONEHOT, 2, 1, "bsym");
    task.n_sources = 2;
    task.goal = PT(PORT_BINARY_MSB, 2, 1, "pair");

    CHECK(library_evolve(&reg, &task, 1, NULL, 0, NULL, 4, &rep) == 0, "evolve runs");
    CHECK(rep.chunk_count == 1, "one dag chunk invented");
    CHECK(strcmp(rep.names[0], "chunk_pair") == 0, "chunk named");
    CHECK(reg.count == 3, "registry grew to 3");
    CHECK(rep.teacher_mac_estimate[0] > 0, "dag: teacher_mac populated");
    CHECK(rep.student_mac_estimate[0] == btn_cost(rep.chunks[0]), "dag: student_mac == btn_cost(chunk)");
    CHECK(rep.compute_beneficial[0] == (rep.student_mac_estimate[0] < rep.teacher_mac_estimate[0]),
          "dag: compute_beneficial self-consistent");

    registry_free(&reg);
    library_report_free(&rep);
    btn_free(&d2);
    btn_free(&comb);
}

/* idp: BINARY_MSB2 -> BINARY_MSB2, identity. */
static int make_idp(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][2]; int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 21u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, NULL),
                      PT(PORT_BINARY_MSB, 2, 1, NULL)) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb2(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* flp: BINARY_MSB2 -> BINARY_MSB2, i -> (i+1)%4. */
static int make_flp(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][2]; int i;
    if (btn_init(b, 2, 2, 1, 16, 0.8, 23u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_BINARY_MSB, 2, 1, NULL),
                      PT(PORT_BINARY_MSB, 2, 1, NULL)) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2(i, in[i]); msb2((i + 1) % 4, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static void test_law_guard_rollback(void) {
    BinaryTransformNetwork dec = {0}, inc = {0}, idp = {0}, flp = {0};
    PrimitiveRegistry reg;
    LibraryTask task;
    LibraryReport rep;
    Property law;
    PropertyReport pr;
    size_t i; int present = 0;
    printf("law-guard rollback:\n");
    CHECK(make_route_dec(&dec) == 0 && make_route_inc(&inc) == 0 &&
          make_idp(&idp) == 0 && make_flp(&flp) == 0, "primitives train");
    registry_init(&reg);
    registry_add(&reg, &dec, "dec");
    registry_add(&reg, &inc, "inc");
    registry_add(&reg, &idp, "idp");
    registry_add(&reg, &flp, "flp");

    /* An intentionally-false law (idp == flp over BINARY_MSB2), unrelated to
       chunk_inc4: this is a MECHANISM test -- any violated law must block
       registration and roll the chunk back -- not a semantic test of the chunk. */
    memset(&law, 0, sizeof law);
    snprintf(law.name, sizeof law.name, "%s", "idp_eq_flp");
    law.sources[0] = PT(PORT_BINARY_MSB, 2, 1, NULL);
    law.source_count = 1;
    snprintf(law.lhs[0], sizeof law.lhs[0], "%s", "idp");
    law.lhs_len = 1;
    snprintf(law.rhs[0], sizeof law.rhs[0], "%s", "flp");
    law.rhs_len = 1;
    CHECK(property_check(&law, &reg, 4096, &pr) != 0 && pr.violated > 0,
          "false law is violated as authored");

    memset(&task, 0, sizeof task);
    task.name = "chunk_inc4";
    task.sources[0] = PT(PORT_ONEHOT, 4, 1, NULL);
    task.n_sources = 1;
    task.goal = PT(PORT_BINARY_MSB, 3, 1, NULL);

    CHECK(library_evolve(&reg, &task, 1, &law, 1, NULL, 4, &rep) == 0, "evolve runs");
    CHECK(rep.chunk_count == 0, "chunk rolled back, none kept");
    CHECK(rep.rolled_back == 1, "one rollback recorded");
    CHECK(reg.count == 4, "registry restored to base four");
    for (i = 0; i < reg.count; ++i) {
        if (strcmp(reg.entries[i].name, "chunk_inc4") == 0) present = 1;
    }
    CHECK(!present, "chunk absent from registry");
    CHECK(rep.iterations_run == 1, "fixed point in one pass (nothing added)");

    registry_free(&reg);
    library_report_free(&rep);
    btn_free(&dec); btn_free(&inc); btn_free(&idp); btn_free(&flp);
}

static void test_registry_remove_last(void) {
    BinaryTransformNetwork a = {0}, b = {0};
    PrimitiveRegistry reg;
    printf("registry_remove_last:\n");
    /* Tiny nets; only pointer identity matters for the registry test. */
    if (btn_init(&a, 2, 2, 1, 8, 0.8, 1u) != 0 ||
        btn_init(&b, 2, 2, 1, 8, 0.8, 2u) != 0) {
        printf("  FAIL btn_init\n"); ++failures; return;
    }
    registry_init(&reg);
    registry_add(&reg, &a, "a");
    registry_add(&reg, &b, "b");
    CHECK(reg.count == 2, "two added");
    CHECK(registry_remove_last(&reg) == 0 && reg.count == 1, "remove -> 1");
    CHECK(registry_remove_last(&reg) == 0 && reg.count == 0, "remove -> 0");
    CHECK(registry_remove_last(&reg) == -1, "remove on empty -> -1");
    registry_free(&reg);
    btn_free(&a);
    btn_free(&b);
}

static void test_sleep_cse_traces(void) {
    LibraryTrace traces[2];
    LibraryReport rep;
    PrimitiveRegistry reg;
    printf("sleep CSE across traces:\n");
    memset(traces, 0, sizeof traces);
    memset(&rep, 0, sizeof rep);
    registry_init(&reg);
    traces[0].length = 2;
    snprintf(traces[0].steps[0], sizeof traces[0].steps[0], "%s", "dec");
    snprintf(traces[0].steps[1], sizeof traces[0].steps[1], "%s", "inc");
    traces[1].length = 3;
    snprintf(traces[1].steps[0], sizeof traces[1].steps[0], "%s", "dec");
    snprintf(traces[1].steps[1], sizeof traces[1].steps[1], "%s", "inc");
    snprintf(traces[1].steps[2], sizeof traces[1].steps[2], "%s", "extra");
    CHECK(library_sleep_compress(&reg, traces, 2, NULL, 0, NULL, &rep) == 0,
          "sleep runs");
    CHECK(rep.sleep_compressed == 1, "shared brick extracted");
    {
        LibraryReport one;
        memset(&one, 0, sizeof one);
        CHECK(library_sleep_compress(&reg, traces, 1, NULL, 0, NULL, &one) == 0,
              "one trace call");
        CHECK(one.sleep_compressed == 0, "one trace is not sleep");
        library_report_free(&one);
    }
    registry_free(&reg);
    library_report_free(&rep);
}

int run_test_library(void) {
    test_registry_remove_last();
    test_route_invention();
    test_dag_invention();
    test_law_guard_rollback();
    test_sleep_cse_traces();
    if (failures == 0) printf("\nALL LIBRARY TESTS PASS\n");
    else printf("\n%d CHECK(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}


#ifdef CNET_LIBRARY_STANDALONE
int main(void) { return run_test_library(); }
#endif
