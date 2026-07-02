/* tests/test_distillation_gate.c -- TDD for the distillation gate.
 * (spec: docs/superpowers/specs/2026-06-19-distillation-gate-design.md) */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/scan.h"
#include "../include/library.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

static int failures = 0;
#define CHECK(cond, desc) do { \
    if (cond) printf("  ok   %s\n", (desc)); \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* One DAG_PRIMITIVE root producing output port 0, named `name`, with one
   (unresolved) source child slot. Enough to exercise the structural digest. */
static void make_unit_plan(DagPlan *p, BinaryTransformNetwork *btn, const char *name) {
    DagNode *root = calloc(1, sizeof *root);
    root->kind = DAG_PRIMITIVE;
    root->btn = btn;
    root->name = name;
    root->output_index = 0;
    root->child_count = 1;
    root->children[0] = NULL;
    root->child_ports[0] = 0;
    memset(p, 0, sizeof *p);
    p->root = root;
    p->owned = calloc(1, sizeof(DagNode *));
    p->owned[0] = root;
    p->owned_count = 1;
}
static void free_unit_plan(DagPlan *p) { free(p->owned[0]); free(p->owned); }

static void test_scan_digest(void) {
    BinaryTransformNetwork a, b;
    Port in = { PORT_ONEHOT, 4, 1, "" }, out = { PORT_ONEHOT, 4, 1, "" };
    port_set_tag(&in, "x"); port_set_tag(&out, "g");
    btn_init(&a, 4, 4, 1, 4, 0.5, 1u); btn_set_ports(&a, in, out);
    btn_init(&b, 4, 4, 1, 4, 0.5, 1u); btn_set_ports(&b, in, out);

    DagPlan p1, p2, p3;
    make_unit_plan(&p1, &a, "solo");
    make_unit_plan(&p2, &b, "solo");   /* same name+ports, different BTN ptr */
    make_unit_plan(&p3, &b, "other");  /* different name */

    CHECK(scan_plan_digest(&p1) == scan_plan_digest(&p2),
          "digest is by name/structure, not pointer (same struct -> equal)");
    CHECK(scan_plan_digest(&p1) != scan_plan_digest(&p3),
          "different producer name -> different digest");

    free_unit_plan(&p1); free_unit_plan(&p2); free_unit_plan(&p3);
    btn_free(&a); btn_free(&b);
}

/* one producer X("x") -> G(goal_tag), untrained */
static int gate_build_prod(BinaryTransformNetwork *b, const char *goal_tag) {
    Port in = { PORT_ONEHOT, 4, 1, "" }, out = { PORT_ONEHOT, 4, 1, "" };
    port_set_tag(&in, "x"); port_set_tag(&out, goal_tag);
    memset(b, 0, sizeof *b);
    if (btn_init(b, 4, 4, 1, 4, 0.5, 1u) != 0) return -1;
    return btn_set_ports(b, in, out);
}

static void test_canonical_digest(void) {
    BinaryTransformNetwork a, b;
    PrimitiveRegistry reg;
    Port goal = { PORT_ONEHOT, 4, 1, "" }, src_t = { PORT_ONEHOT, 4, 1, "" };
    double sv[4] = {1,0,0,0};
    DagSource src; uint64_t d1, d2;

    gate_build_prod(&a, "gl"); gate_build_prod(&b, "gl");
    port_set_tag(&goal, "gl"); port_set_tag(&src_t, "x");
    src.type = src_t; src.values = sv;

    registry_init(&reg);
    registry_add(&reg, &a, "alt_a");
    registry_add(&reg, &b, "alt_b");
    d1 = structural_canonical_digest(&reg, &src, 1, goal);
    d2 = structural_canonical_digest(&reg, &src, 1, goal);
    CHECK(d1 != 0, "lure: canonical digest is nonzero (a plan exists)");
    CHECK(d1 == d2, "lure: canonical digest is stable across calls (content-addressed)");
    registry_free(&reg);
    btn_free(&a); btn_free(&b);
}

static void test_gate_off_parity(void) {
    LibraryGateConfig g;
    library_gate_config_defaults(&g);
    CHECK(g.enabled == 0, "gate config default is DISABLED (legacy)");
    CHECK(g.evidence_threshold == 0.9 && g.min_evidence == 16,
          "gate defaults reuse the lifecycle 0.9/16 bar");
}

/* ---- evidence_gate fixture (mirrors test_library route invention) ---- */

static void msb2g(int i, double *o) { o[0] = (double)((i >> 1) & 1); o[1] = (double)(i & 1); }
static void msb3g(int i, double *o) { o[0] = (double)((i >> 2) & 1); o[1] = (double)((i >> 1) & 1); o[2] = (double)(i & 1); }

static Port PTG(PortFamily family, size_t field_width, size_t field_count) {
    Port p; p.family = family; p.field_width = field_width;
    p.field_count = field_count; p.tag[0] = '\0';
    return p;
}

/* dec: ONEHOT4 -> BINARY_MSB2, i -> i */
static int ev_make_dec(BinaryTransformNetwork *b) {
    double in[4][4] = {{0}}; double tg[4][2]; int i;
    if (btn_init(b, 4, 2, 1, 16, 0.8, 11u) != 0) return -1;
    if (btn_set_ports(b, PTG(PORT_ONEHOT, 4, 1), PTG(PORT_BINARY_MSB, 2, 1)) != 0) return -1;
    for (i = 0; i < 4; ++i) { in[i][i] = 1.0; msb2g(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4, 60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* inc: BINARY_MSB2 -> BINARY_MSB3, i -> i+1 */
static int ev_make_inc(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][3]; int i;
    if (btn_init(b, 2, 3, 1, 16, 0.8, 13u) != 0) return -1;
    if (btn_set_ports(b, PTG(PORT_BINARY_MSB, 2, 1), PTG(PORT_BINARY_MSB, 3, 1)) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2g(i, in[i]); msb3g(i + 1, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4, 60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* Seed evidence counters (fields are _Atomic; plain assignment is an atomic store). */
static void gate_set_ev(BinaryTransformNetwork *b, unsigned long s, unsigned long f) {
    b->output_successes = s; b->output_failures = f;
}

static void test_evidence_gate(void) {
    printf("evidence_gate:\n");

    /* Branch A: fresh primitives (0 evidence) + gate ENABLED -> DEFER, no chunk */
    {
        BinaryTransformNetwork dec = {0}, inc = {0};
        PrimitiveRegistry reg;
        LibraryTask task;
        LibraryReport report;
        LibraryGateConfig g;

        if (ev_make_dec(&dec) != 0 || ev_make_inc(&inc) != 0) {
            printf("  FAIL base primitives train (A)\n"); ++failures;
            btn_free(&dec); btn_free(&inc);
            goto branch_b;
        }
        registry_init(&reg);
        registry_add(&reg, &dec, "dec");
        registry_add(&reg, &inc, "inc");

        memset(&task, 0, sizeof task);
        task.name = "chunk_inc4";
        task.sources[0] = PTG(PORT_ONEHOT, 4, 1);
        task.n_sources = 1;
        task.goal = PTG(PORT_BINARY_MSB, 3, 1);

        library_gate_config_defaults(&g);
        g.enabled = 1;  /* gate ON; evidence = 0 for all primitives */

        library_evolve_gated(&reg, &task, 1, NULL, 0, NULL, &g, 4, &report);
        CHECK(report.chunk_count == 0, "A: fresh evidence -> no chunk minted");
        CHECK(report.deferred >= 1, "A: deferred >= 1 (evidence gate fired)");

        library_report_free(&report);
        registry_free(&reg);
        btn_free(&dec);
        btn_free(&inc);
    }

branch_b:
    /* Branch B: seed all base primitives to 16/0 evidence -> gate clears -> chunk minted */
    {
        BinaryTransformNetwork dec = {0}, inc = {0};
        PrimitiveRegistry reg;
        LibraryTask task;
        LibraryReport report;
        LibraryGateConfig g;

        if (ev_make_dec(&dec) != 0 || ev_make_inc(&inc) != 0) {
            printf("  FAIL base primitives train (B)\n"); ++failures;
            btn_free(&dec); btn_free(&inc);
            return;
        }
        /* seed evidence: reliability = (16+1)/(16+0+2) = 17/18 ~ 0.944 >= 0.9 */
        gate_set_ev(&dec, 16, 0);
        gate_set_ev(&inc, 16, 0);

        registry_init(&reg);
        registry_add(&reg, &dec, "dec");
        registry_add(&reg, &inc, "inc");

        memset(&task, 0, sizeof task);
        task.name = "chunk_inc4";
        task.sources[0] = PTG(PORT_ONEHOT, 4, 1);
        task.n_sources = 1;
        task.goal = PTG(PORT_BINARY_MSB, 3, 1);

        library_gate_config_defaults(&g);
        g.enabled = 1;

        library_evolve_gated(&reg, &task, 1, NULL, 0, NULL, &g, 4, &report);
        CHECK(report.chunk_count >= 1, "B: evidence cleared -> chunk minted");
        CHECK(report.teacher_mac_estimate[0] > 0, "route: teacher_mac populated");
        CHECK(report.student_mac_estimate[0] == btn_cost(report.chunks[0]), "route: student_mac == btn_cost(chunk)");
        CHECK(report.compute_beneficial[0] == (report.student_mac_estimate[0] < report.teacher_mac_estimate[0]),
              "route: compute_beneficial self-consistent");

        library_report_free(&report);
        registry_free(&reg);
        btn_free(&dec);
        btn_free(&inc);
    }
}

static void test_circuit_task_shape(void) {
    LibraryTask t;
    memset(&t, 0, sizeof t);
    CHECK(t.n_goals == 0, "LibraryTask.n_goals zero-inits to 0 (legacy single-goal)");
    t.n_goals = 2;
    CHECK(t.n_goals == 2 && (sizeof t.goals / sizeof t.goals[0]) >= 2,
          "LibraryTask carries goals[] for the circuit path");
}

/* Helper: build port with tag (mirrors PT() in test_decimal.c). */
static Port PT_c(PortFamily family, size_t field_width, size_t field_count,
                 const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL) port_set_tag(&p, tag);
    return p;
}

/* Build dec_value: ONEHOT 10 "dec_symbol" -> BINARY_MSB 4 "dec_digit".
   Uses btn_init_committed when generated.h is available, btn_load otherwise. */
static int cg_make_dec_value(BinaryTransformNetwork *b) {
#ifdef USE_FROZEN_COMMITTED
    return btn_init_committed(b, "dec_value");
#else
    return btn_load(b, "dec_value_weights.txt");
#endif
}

/* Build dec_full_add: (BINARY_MSB 4 "dec_digit", BINARY_MSB 4 "dec_digit",
   BINARY_MSB 1 "dec_carry") -> (BINARY_MSB 4 "dec_sum", BINARY_MSB 1 "dec_carry"). */
static int cg_make_dec_full_add(BinaryTransformNetwork *b) {
#ifdef USE_FROZEN_COMMITTED
    return btn_init_committed(b, "dec_full_add");
#else
    return btn_load(b, "dec_full_add_weights.txt");
#endif
}

/* Helper: seed evidence counters. */
static void cg_set_ev(BinaryTransformNetwork *b, unsigned long s, unsigned long f) {
    b->output_successes = s;
    b->output_failures = f;
}

/* Build the circuit task: 3 sources -> 2 goals. */
static void cg_fill_task(LibraryTask *task) {
    memset(task, 0, sizeof *task);
    task->name = "dec_full_adder_unit";
    /* sources: dec_symbol, dec_symbol, dec_carry (BINARY_MSB 1) */
    task->sources[0] = PT_c(PORT_ONEHOT, 10, 1, "dec_symbol");
    task->sources[1] = PT_c(PORT_ONEHOT, 10, 1, "dec_symbol");
    task->sources[2] = PT_c(PORT_BINARY_MSB, 1, 1, "dec_carry");
    task->n_sources = 3;
    /* goals: dec_sum (BINARY_MSB 4), dec_carry (BINARY_MSB 1) */
    task->goals[0] = PT_c(PORT_BINARY_MSB, 4, 1, "dec_sum");
    task->goals[1] = PT_c(PORT_BINARY_MSB, 1, 1, "dec_carry");
    task->n_goals = 2;
}

static void test_circuit_evidence_gate(void) {
    printf("circuit_evidence_gate:\n");

    /* ---- Branch A: all primitives 0 evidence, gate enabled -> DEFER, no chunk ---- */
    {
        BinaryTransformNetwork dv = {0}, dfa = {0};
        PrimitiveRegistry reg;
        LibraryTask task;
        LibraryReport rep;
        LibraryGateConfig g;

        if (cg_make_dec_value(&dv) != 0 || cg_make_dec_full_add(&dfa) != 0) {
            printf("  FAIL A: load dec_value/dec_full_add\n"); ++failures;
            btn_free(&dv); btn_free(&dfa);
            goto branch_b_circ;
        }
        /* evidence stays at 0 */
        registry_init(&reg);
        registry_add(&reg, &dv, "dec_value");
        registry_add(&reg, &dfa, "dec_full_add");

        cg_fill_task(&task);
        library_gate_config_defaults(&g);
        g.enabled = 1;

        library_evolve_gated(&reg, &task, 1, NULL, 0, NULL, &g, 4, &rep);
        if (rep.deferred == 0) {
            printf("  STOP: dag_plan_circuit found no >=2-primitive circuit "
                   "(deferred==0); planner did not discover a valid plan for this "
                   "fixture -- check primitive ports and n_goals routing.\n");
            ++failures;
            library_report_free(&rep);
            registry_free(&reg); btn_free(&dv); btn_free(&dfa);
            goto branch_b_circ;
        }
        CHECK(rep.chunk_count == 0, "A: 0-evidence + gate enabled -> no chunk minted");
        CHECK(rep.deferred >= 1, "A: 0-evidence + gate enabled -> deferred >= 1");

        library_report_free(&rep);
        registry_free(&reg);
        btn_free(&dv); btn_free(&dfa);
    }

branch_b_circ:
    /* ---- Branch B: seed ONLY dec_value to (16,0), dec_full_add stays at 0 -> still defer ---- */
    {
        BinaryTransformNetwork dv = {0}, dfa = {0};
        PrimitiveRegistry reg;
        LibraryTask task;
        LibraryReport rep;
        LibraryGateConfig g;

        if (cg_make_dec_value(&dv) != 0 || cg_make_dec_full_add(&dfa) != 0) {
            printf("  FAIL B: load dec_value/dec_full_add\n"); ++failures;
            btn_free(&dv); btn_free(&dfa);
            goto branch_c_circ;
        }
        cg_set_ev(&dv, 16, 0);   /* dec_value cleared */
        /* dec_full_add remains at 0 evidence */

        registry_init(&reg);
        registry_add(&reg, &dv, "dec_value");
        registry_add(&reg, &dfa, "dec_full_add");

        cg_fill_task(&task);
        library_gate_config_defaults(&g);
        g.enabled = 1;

        library_evolve_gated(&reg, &task, 1, NULL, 0, NULL, &g, 4, &rep);
        CHECK(rep.chunk_count == 0, "B: only dec_value seeded -> still no chunk (dec_full_add not clear)");
        CHECK(rep.deferred >= 1, "B: shared dec_full_add node blocks distillation (deferred >= 1)");

        library_report_free(&rep);
        registry_free(&reg);
        btn_free(&dv); btn_free(&dfa);
    }

branch_c_circ:
    /* ---- Branch C: seed BOTH to (16,0) -> chunk minted, output_port_count == 2 ---- */
    {
        BinaryTransformNetwork dv = {0}, dfa = {0};
        PrimitiveRegistry reg;
        LibraryTask task;
        LibraryReport rep;
        LibraryGateConfig g;
        ConsolidateConfig cfg;

        if (cg_make_dec_value(&dv) != 0 || cg_make_dec_full_add(&dfa) != 0) {
            printf("  FAIL C: load dec_value/dec_full_add\n"); ++failures;
            btn_free(&dv); btn_free(&dfa);
            goto branch_d_circ;
        }
        cg_set_ev(&dv, 16, 0);
        cg_set_ev(&dfa, 16, 0);

        registry_init(&reg);
        registry_add(&reg, &dv, "dec_value");
        registry_add(&reg, &dfa, "dec_full_add");

        cg_fill_task(&task);
        library_gate_config_defaults(&g);
        g.enabled = 1;

        /* Use more generous training for this multi-output circuit:
           the circuit has 200 samples (10*10*2); relax min_verify_rate
           to 0.95 to tolerate the occasional near-boundary miss while
           still proving the distillation path fires. */
        consolidate_config_defaults(&cfg);
        cfg.min_verify_rate = 0.95;
        cfg.max_epochs = 320000;

        library_evolve_gated(&reg, &task, 1, NULL, 0, &cfg, &g, 4, &rep);
        CHECK(rep.chunk_count >= 1, "C: both primitives cleared -> chunk minted");
        if (rep.chunk_count >= 1) {
            CHECK(rep.chunks[0]->output_port_count == 2,
                  "C: minted circuit chunk has output_port_count == 2");
        }
        {
            size_t exp_teacher = 2 * btn_cost(&dv) + btn_cost(&dfa);
            CHECK(rep.teacher_mac_estimate[0] == exp_teacher,
                  "circuit: teacher_mac == sum btn_cost over column primitives");
            CHECK(rep.student_mac_estimate[0] == btn_cost(rep.chunks[0]),
                  "circuit: student_mac == btn_cost(chunk)");
            CHECK(rep.compute_beneficial[0] == (rep.student_mac_estimate[0] < rep.teacher_mac_estimate[0]),
                  "circuit: compute_beneficial == (student < teacher)");
            if (rep.student_mac_estimate[0] > 0)
                CHECK(rep.compression_ratio[0] ==
                        (double)rep.teacher_mac_estimate[0] / (double)rep.student_mac_estimate[0],
                      "circuit: compression_ratio == teacher/student");
        }

        /* 3C: the minted chunk's registry entry carries the expansion recipe
           (the teacher column) + the persisted cost truth -- the mint-capture
           path that lets the planner expand it under LOW power later. */
        if (rep.chunk_count >= 1) {
            size_t ei, k, n_dv = 0, n_dfa = 0, bad = 0;
            const RegistryEntry *ce = NULL;
            for (ei = 0; ei < reg.count; ++ei)
                if (reg.entries[ei].name != NULL &&
                    strcmp(reg.entries[ei].name, "dec_full_adder_unit") == 0) {
                    ce = &reg.entries[ei]; break;
                }
            CHECK(ce != NULL && ce->recipe != NULL,
                  "3C: minted chunk entry carries an expansion recipe");
            if (ce != NULL && ce->recipe != NULL) {
                CHECK(ce->recipe->primitive_count == 3,
                      "3C: recipe is the 3-primitive teacher column");
                for (k = 0; k < ce->recipe->primitive_count; ++k) {
                    if (strcmp(ce->recipe->primitives[k], "dec_value") == 0) ++n_dv;
                    else if (strcmp(ce->recipe->primitives[k], "dec_full_add") == 0) ++n_dfa;
                    else ++bad;
                }
                CHECK(n_dv == 2 && n_dfa == 1 && bad == 0,
                      "3C: recipe names == {dec_value x2, dec_full_add x1}");
                CHECK(ce->expand_in_low == (rep.compute_beneficial[0] ? 0 : 1),
                      "3C: expand_in_low == !compute_beneficial");
                CHECK(ce->teacher_mac == rep.teacher_mac_estimate[0] &&
                      ce->student_mac == rep.student_mac_estimate[0] &&
                      ce->compute_beneficial == rep.compute_beneficial[0],
                      "3C: entry cost truth matches the report");
            }
        }

        library_report_free(&rep);
        registry_free(&reg);
        btn_free(&dv); btn_free(&dfa);
    }

branch_d_circ:
    /* ---- Branch D: gate DISABLED (g.enabled=0) -> chunk minted regardless of evidence ---- */
    {
        BinaryTransformNetwork dv = {0}, dfa = {0};
        PrimitiveRegistry reg;
        LibraryTask task;
        LibraryReport rep;
        LibraryGateConfig g;
        ConsolidateConfig cfg;

        if (cg_make_dec_value(&dv) != 0 || cg_make_dec_full_add(&dfa) != 0) {
            printf("  FAIL D: load dec_value/dec_full_add\n"); ++failures;
            btn_free(&dv); btn_free(&dfa);
            return;
        }
        /* gate is OFF; seed evidence for well-conditioned training */
        cg_set_ev(&dv, 16, 0);
        cg_set_ev(&dfa, 16, 0);

        registry_init(&reg);
        registry_add(&reg, &dv, "dec_value");
        registry_add(&reg, &dfa, "dec_full_add");

        cg_fill_task(&task);
        library_gate_config_defaults(&g);
        g.enabled = 0;  /* gate OFF -> legacy behavior */

        consolidate_config_defaults(&cfg);
        cfg.min_verify_rate = 0.95;
        cfg.max_epochs = 320000;

        library_evolve_gated(&reg, &task, 1, NULL, 0, &cfg, &g, 4, &rep);
        CHECK(rep.chunk_count >= 1, "D: gate disabled -> chunk minted regardless of evidence");

        library_report_free(&rep);
        registry_free(&reg);
        btn_free(&dv); btn_free(&dfa);
    }
}

int run_test_distillation_gate(void) {
    failures = 0;
    printf("distillation_gate:\n");
    test_scan_digest();
    test_canonical_digest();
    test_gate_off_parity();
    test_evidence_gate();
    test_circuit_task_shape();
    test_circuit_evidence_gate();
    if (failures == 0) { printf("DISTILLATION_GATE PASS\n"); return 0; }
    printf("DISTILLATION_GATE FAIL: %d\n", failures);
    return 1;
}

#ifndef TEST_ALL
int main(void) { return run_test_distillation_gate(); }
#endif
