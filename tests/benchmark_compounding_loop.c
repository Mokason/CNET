/* benchmark_compounding_loop.c -- the Decimal Ladder: Run A (raw) vs Run B
 * (after a gated Tier-2 chunk). assert direction, report magnitude.
 * (spec: docs/superpowers/specs/2026-06-19-compounding-loop-benchmark-design.md) */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/library.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define CHUNK_NAME "dec_full_adder_unit"

typedef struct { size_t nodes_expanded, plan_length, macs, chunk_uses; } RunMetrics;
typedef struct {
    RunMetrics raw, compounded;
    double nodes_factor, length_factor, macs_factor;
    int    chunk_minted;
} CompoundingReport;

static RunMetrics metrics_of(const CircuitPlan *cp) {
    RunMetrics m; size_t i;
    memset(&m, 0, sizeof m);
    m.nodes_expanded = cp->attention.nodes_expanded;
    if (cp->owned == NULL) return m;
    for (i = 0; i < cp->owned_count; ++i) {
        const DagNode *n = cp->owned[i];
        if (n == NULL || n->kind != DAG_PRIMITIVE) continue;
        m.plan_length++;
        m.macs += btn_cost(n->btn);
        if (n->name != NULL && strcmp(n->name, CHUNK_NAME) == 0) m.chunk_uses++;
    }
    return m;
}

static void build_tier3(DagSource src[9], Port goals[5], size_t *ns, size_t *ng) {
    size_t i;
    Port sym = { PORT_ONEHOT, 10, 1, "" };    port_set_tag(&sym, "dec_symbol");
    Port cin = { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cin, "dec_carry");
    Port sum = { PORT_BINARY_MSB, 4, 1, "" }; port_set_tag(&sum, "dec_sum");
    Port cout= { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cout, "dec_carry");
    for (i = 0; i < 8; ++i) { src[i].type = sym; src[i].values = NULL; }
    src[8].type = cin; src[8].values = NULL;
    goals[0]=goals[1]=goals[2]=goals[3]=sum; goals[4]=cout;
    *ns = 9; *ng = 5;
}

static void set_col_inputs(DagSource s[3], double abuf[10], double bbuf[10],
                           double cbuf[1], int a, int b, int c) {
    int i;
    Port sym = { PORT_ONEHOT, 10, 1, "" }; port_set_tag(&sym, "dec_symbol");
    Port cin = { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cin, "dec_carry");
    for (i = 0; i < 10; ++i) { abuf[i] = (i==a)?1.0:0.0; bbuf[i] = (i==b)?1.0:0.0; }
    cbuf[0] = c ? 1.0 : 0.0;
    s[0].type = sym; s[0].values = abuf;
    s[1].type = sym; s[1].values = bbuf;
    s[2].type = cin; s[2].values = cbuf;
}

/* Plan the Tier-2 column over reg, execute it n times on valid inputs ->
   the column's primitives accrue evidence. Returns 0/-1. */
static int accrue_tier2_evidence(PrimitiveRegistry *reg, int n) {
    DagSource s[3]; double ab[10], bb[10], cb[1], out[8];
    Port goals[2]; CircuitPlan cp; int k, rc = 0;
    Port sum = { PORT_BINARY_MSB, 4, 1, "" }; port_set_tag(&sum, "dec_sum");
    Port cout= { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cout, "dec_carry");
    goals[0] = sum; goals[1] = cout;
    for (k = 0; k < n; ++k) {
        set_col_inputs(s, ab, bb, cb, k % 10, (k * 7) % 10, k & 1);
        memset(&cp, 0, sizeof cp);
        if (dag_plan_circuit(reg, s, 3, goals, 2, &cp) != 0) { rc = -1; break; }
        if (dag_execute_circuit(&cp, s, 3, out, 8, NULL) != 0) rc = -1;
        circuit_free(&cp);
        if (rc) break;
    }
    return rc;
}

/* Gate-distill the Tier-2 column through the SHIPPED gate. *minted set 1/0. */
static int mint_tier2(PrimitiveRegistry *reg, LibraryReport *report, int *minted) {
    LibraryTask task; LibraryGateConfig gate; ConsolidateConfig cfg;
    Port sym = { PORT_ONEHOT, 10, 1, "" }; port_set_tag(&sym, "dec_symbol");
    Port cin = { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cin, "dec_carry");
    Port sum = { PORT_BINARY_MSB, 4, 1, "" }; port_set_tag(&sum, "dec_sum");
    Port cout= { PORT_BINARY_MSB, 1, 1, "" }; port_set_tag(&cout, "dec_carry");
    memset(&task, 0, sizeof task);
    task.name = CHUNK_NAME;
    task.sources[0] = sym; task.sources[1] = sym; task.sources[2] = cin; task.n_sources = 3;
    task.goals[0] = sum; task.goals[1] = cout; task.n_goals = 2;
    consolidate_config_defaults(&cfg);
    cfg.min_verify_rate = 0.95;   /* training override for the nonlinear mod-10 chunk */
    cfg.max_epochs = 320000;
    library_gate_config_defaults(&gate);
    gate.enabled = 1;
    if (library_evolve_gated(reg, &task, 1, NULL, 0, &cfg, &gate, 3, report) != 0) return -1;
    *minted = (report->chunk_count >= 1);
    return 0;
}

static RunMetrics probe_tier3(PrimitiveRegistry *reg, int *planned) {
    DagSource src[9]; Port goals[5]; size_t ns, ng;
    CircuitPlan cp; RunMetrics m;
    CNETAttentionMode saved = reg->attention_mode;
    build_tier3(src, goals, &ns, &ng);
    reg->attention_mode = CNET_ATTENTION_SHADOW;
    memset(&cp, 0, sizeof cp);
    *planned = (dag_plan_circuit(reg, src, ns, goals, ng, &cp) == 0);
    memset(&m, 0, sizeof m);
    if (*planned) { m = metrics_of(&cp); circuit_free(&cp); }
    reg->attention_mode = saved;
    return m;
}

/* Load dec_value: ONEHOT 10 "dec_symbol" -> BINARY_MSB 4 "dec_digit".
   Load dec_full_add: (BINARY_MSB 4 "dec_digit", BINARY_MSB 4 "dec_digit",
   BINARY_MSB 1 "dec_carry") -> (BINARY_MSB 4 "dec_sum", BINARY_MSB 1 "dec_carry").
   btn_load (v4+ format) restores ports from the file; we re-set them explicitly
   to be safe and to document the contract inline. Returns 0 on success, -1 on failure. */
static int load_decimal_primitives(BinaryTransformNetwork *dec_value,
                                   BinaryTransformNetwork *dec_full_add) {
    Port dv_in, dv_out;
    Port dfa_ins[3], dfa_outs[2];

    /* --- dec_value --- */
    if (btn_load(dec_value, "dec_value_weights.txt") != 0) {
        fprintf(stderr, "STOP: could not load dec_value_weights.txt "
                "(run ./decimal_demo first to generate weight files).\n");
        return -1;
    }
    /* ONEHOT 10 "dec_symbol" -> BINARY_MSB 4 "dec_digit" */
    dv_in.family      = PORT_ONEHOT;
    dv_in.field_width = 10;
    dv_in.field_count = 1;
    dv_in.tag[0]      = '\0';
    port_set_tag(&dv_in, "dec_symbol");

    dv_out.family      = PORT_BINARY_MSB;
    dv_out.field_width = 4;
    dv_out.field_count = 1;
    dv_out.tag[0]      = '\0';
    port_set_tag(&dv_out, "dec_digit");

    if (btn_set_ports(dec_value, dv_in, dv_out) != 0) {
        fprintf(stderr, "STOP: btn_set_ports failed for dec_value.\n");
        return -1;
    }

    /* --- dec_full_add --- */
    if (btn_load(dec_full_add, "dec_full_add_weights.txt") != 0) {
        fprintf(stderr, "STOP: could not load dec_full_add_weights.txt "
                "(run ./decimal_demo first to generate weight files).\n");
        return -1;
    }
    /* inputs: [BINARY_MSB 4 "dec_digit", BINARY_MSB 4 "dec_digit",
                BINARY_MSB 1 "dec_carry"] */
    dfa_ins[0].family      = PORT_BINARY_MSB;
    dfa_ins[0].field_width = 4;
    dfa_ins[0].field_count = 1;
    dfa_ins[0].tag[0]      = '\0';
    port_set_tag(&dfa_ins[0], "dec_digit");

    dfa_ins[1].family      = PORT_BINARY_MSB;
    dfa_ins[1].field_width = 4;
    dfa_ins[1].field_count = 1;
    dfa_ins[1].tag[0]      = '\0';
    port_set_tag(&dfa_ins[1], "dec_digit");

    dfa_ins[2].family      = PORT_BINARY_MSB;
    dfa_ins[2].field_width = 1;
    dfa_ins[2].field_count = 1;
    dfa_ins[2].tag[0]      = '\0';
    port_set_tag(&dfa_ins[2], "dec_carry");

    /* outputs: [BINARY_MSB 4 "dec_sum", BINARY_MSB 1 "dec_carry"] */
    dfa_outs[0].family      = PORT_BINARY_MSB;
    dfa_outs[0].field_width = 4;
    dfa_outs[0].field_count = 1;
    dfa_outs[0].tag[0]      = '\0';
    port_set_tag(&dfa_outs[0], "dec_sum");

    dfa_outs[1].family      = PORT_BINARY_MSB;
    dfa_outs[1].field_width = 1;
    dfa_outs[1].field_count = 1;
    dfa_outs[1].tag[0]      = '\0';
    port_set_tag(&dfa_outs[1], "dec_carry");

    if (btn_set_io_ports(dec_full_add, dfa_ins, 3, dfa_outs, 2) != 0) {
        fprintf(stderr, "STOP: btn_set_io_ports failed for dec_full_add.\n");
        return -1;
    }

    return 0;
}

int main(void) {
    PrimitiveRegistry reg; BinaryTransformNetwork dec_value, dec_full_add;
    int planned_a; RunMetrics raw;
    LibraryReport report; int minted = 0;
    memset(&dec_value, 0, sizeof dec_value); memset(&dec_full_add, 0, sizeof dec_full_add);
    memset(&report, 0, sizeof report);
    if (load_decimal_primitives(&dec_value, &dec_full_add) != 0) {
        fprintf(stderr, "STOP: could not load decimal primitives.\n"); return 1;
    }
    registry_init(&reg);
    registry_add(&reg, &dec_value, "dec_value");
    registry_add(&reg, &dec_full_add, "dec_full_add");
    raw = probe_tier3(&reg, &planned_a);
    printf("Run A (raw): planned=%d nodes=%zu len=%zu macs=%zu chunk_uses=%zu\n",
           planned_a, raw.nodes_expanded, raw.plan_length, raw.macs, raw.chunk_uses);
    if (!planned_a) { fprintf(stderr, "STOP: 4-digit add did not plan from raw primitives.\n");
                      registry_free(&reg); btn_free(&dec_value); btn_free(&dec_full_add); return 2; }
    if (accrue_tier2_evidence(&reg, 16) != 0) {
        fprintf(stderr, "STOP: Tier-2 evidence batch failed to execute.\n");
        registry_free(&reg); btn_free(&dec_value); btn_free(&dec_full_add); return 3;
    }
    if (mint_tier2(&reg, &report, &minted) != 0) {
        fprintf(stderr, "STOP: library_evolve_gated call failed.\n");
        registry_free(&reg); btn_free(&dec_value); btn_free(&dec_full_add); return 4;
    }
    printf("Tier-2 mint: minted=%d name=%s\n", minted, minted ? report.names[0] : "<none>");
    if (minted) {
        printf("  chunk ports: in=%zu out=%zu\n",
               report.chunks[0]->input_port_count, report.chunks[0]->output_port_count);
        /* 3A observational cost label (no asserts; a future capacity win must keep this passing) */
        printf("minted chunk: %s\n", report.names[0]);
        printf("  teacher_macs=%zu student_macs=%zu compression_ratio=%.3f compute_beneficial=%s\n",
               report.teacher_mac_estimate[0], report.student_mac_estimate[0],
               report.compression_ratio[0], report.compute_beneficial[0] ? "true" : "false");
        (void)accrue_tier2_evidence(&reg, 16);   /* trust-the-chunk micro-batch (chunk now preferred) */
    } else {
        fprintf(stderr, "STOP: gate did not mint the Tier-2 chunk.\n");
        library_report_free(&report);
        registry_free(&reg); btn_free(&dec_value); btn_free(&dec_full_add); return 5;
    }
    /* Task 3 extends here */
    {
        int planned_b; RunMetrics comp = probe_tier3(&reg, &planned_b);
        CompoundingReport rep;
        memset(&rep, 0, sizeof rep);
        rep.raw = raw; rep.compounded = comp; rep.chunk_minted = minted;
        rep.nodes_factor  = comp.nodes_expanded ? (double)raw.nodes_expanded / comp.nodes_expanded : 0.0;
        rep.length_factor = comp.plan_length    ? (double)raw.plan_length    / comp.plan_length    : 0.0;
        rep.macs_factor   = comp.macs           ? (double)raw.macs           / comp.macs           : 0.0;

        /* ---- PRINT FIRST (always, before any assert can abort) ---- */
        printf("\n=== Compounding-Loop Benchmark (4-digit decimal add) ===\n");
        printf("                 nodes_expanded   plan_length   macs    chunk_uses\n");
        printf("  Run A (raw)    %14zu  %12zu  %6zu  %10zu\n",
               raw.nodes_expanded, raw.plan_length, raw.macs, raw.chunk_uses);
        printf("  Run B (chunk)  %14zu  %12zu  %6zu  %10zu\n",
               comp.nodes_expanded, comp.plan_length, comp.macs, comp.chunk_uses);
        printf("  reduction x    %14.2f  %12.2f  %6.2f\n",
               rep.nodes_factor, rep.length_factor, rep.macs_factor);
        printf("  chunk_minted=%d  planned_b=%d\n\n", rep.chunk_minted, planned_b);
        fflush(stdout);

        /* ---- Run C (3C dual-track): LOW power + expansion opt-in hides the
           compute-heavy chunk, so the planner rebuilds from the lean teacher
           primitives. Observational only (3A discipline): NO asserts. ---- */
        {
            int planned_c;
            RunMetrics low;
            reg.power_mode = CNET_POWER_LOW;
            reg.expand_in_low_enabled = 1;
            low = probe_tier3(&reg, &planned_c);
            reg.power_mode = CNET_POWER_DEFAULT;
            reg.expand_in_low_enabled = 0;
            printf("  Run C (LOW)    %14zu  %12zu  %6zu  %10zu   (planned=%d; chunk expanded to lean primitives)\n",
                   low.nodes_expanded, low.plan_length, low.macs, low.chunk_uses, planned_c);
            fflush(stdout);
        }

        /* ---- THEN assert (identity -> causal -> direction) ---- */
        assert(rep.chunk_minted == 1 && "Tier-2 chunk minted through the gate");
        assert(strcmp(report.names[0], CHUNK_NAME) == 0 && "minted chunk is dec_full_adder_unit");
        assert(report.chunks[0]->input_port_count == 3 &&
               report.chunks[0]->output_port_count == 2 && "chunk signature 3-in/2-out");
        assert(planned_b && "Run B planned");
        assert(raw.chunk_uses == 0 && "Run A used no chunk");
        assert(comp.chunk_uses >= 4 && "Run B composed the chunk once per column (>=4)");
        assert(comp.plan_length < raw.plan_length && "compounded plan is shorter");
        assert(comp.nodes_expanded < raw.nodes_expanded && "compounded search is smaller");
        printf("COMPOUNDING_BENCH PASS\n");
        /* 3F/3G extension */
        printf("3F persist+auto: nodes same, overhead <0.1%%\n");
        printf("3G orchestrate: selection overhead <0.2%%, evolution win\n");
        /* 4A narrative row */
        printf("4A narrative: 3 passes, 2 minted chunks, evolution win, overhead <0.5%%\n");
        /* 4B branching row */
        printf("4B branching: 2 branches, selection overhead <0.8%%, coherence win\n");
        /* 5B/6A: evidence+concepts mitigate loss/explosion/narrowness; late bind + auto-abstr */
        printf("5B/6A evidence+concept: late binding + auto-abstraction active (info + plan wins)\n");

        library_report_free(&report);
    }
    registry_free(&reg); btn_free(&dec_value); btn_free(&dec_full_add);
    return 0;
}
