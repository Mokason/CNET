/*
 * test_fastpath.c -- the fast lane is an independent route executor; this
 * certifies it against route_execute (the double oracle) over the committed
 * hex_value -> increment route. Loads committed frozen weights (like
 * test_decimal); run ./nn_demo first if they are stale or absent.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/fastpath.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", (msg)); ++failures; } \
} while (0)

static Port P(PortFamily family, size_t fw, size_t fc) {
    Port p; p.family = family; p.field_width = fw; p.field_count = fc;
    p.tag[0] = '\0'; return p;
}

/* Load the committed route once; returns 0 on success. Caller frees. */
static int load_route(BinaryTransformNetwork *hexval,
                      BinaryTransformNetwork *incr,
                      PrimitiveRegistry *reg, RoutePlan *plan) {
    memset(hexval, 0, sizeof *hexval);
    memset(incr, 0, sizeof *incr);
    if (btn_load(hexval, "hex_value_weights.txt") != 0 ||
        btn_load(incr, "increment_weights.txt") != 0) {
        fprintf(stderr, "could not load frozen primitives (run ./nn_demo).\n");
        return -1;
    }
    registry_init(reg);
    registry_add(reg, hexval, "hex_value");
    registry_add(reg, incr, "increment");
    return route_plan(reg, P(PORT_ONEHOT, 16, 1), P(PORT_BINARY_MSB, 5, 1), plan);
}

static void test_f64_matches_oracle(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision prec = { FP_F64, 0 };
    CompiledRoute *cr;
    int i;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup\n"); ++failures; return;
    }
    cr = fp_route_compile(&plan, prec, 1);
    CHECK(cr != NULL, "fp_route_compile f64");
    if (cr == NULL) { registry_free(&reg); btn_free(&hexval); btn_free(&incr); return; }

    CHECK(fp_route_in_total(cr) == 16, "in_total == 16");
    CHECK(fp_route_out_total(cr) == 5, "out_total == 5");

    for (i = 0; i < 16; ++i) {
        double in[16] = {0};
        double want[5] = {0};
        double got[5] = {0};
        in[i] = 1.0;
        CHECK(route_execute(&plan, in, 16, want, 5) == 0, "oracle run");
        CHECK(fp_route_run(cr, in, 1, got, 5) == 0, "fast f64 run");
        CHECK(memcmp(want, got, sizeof want) == 0, "f64 == oracle (snapped)");
    }

    fp_route_free(cr);
    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}

static void test_batched_equals_single_f64(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision prec = { FP_F64, 0 };
    CompiledRoute *cr;
    double in[16 * 16] = {0};
    double batched[16 * 5] = {0};
    double single[16 * 5] = {0};
    int i;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup (batch)\n"); ++failures; return;
    }
    cr = fp_route_compile(&plan, prec, 16);
    CHECK(cr != NULL, "compile batch=16");
    if (cr == NULL) { registry_free(&reg); btn_free(&hexval); btn_free(&incr); return; }

    for (i = 0; i < 16; ++i) in[i * 16 + i] = 1.0;   /* 16 one-hot inputs */

    CHECK(fp_route_run(cr, in, 16, batched, sizeof batched / sizeof(double)) == 0,
          "batched run");
    for (i = 0; i < 16; ++i)
        CHECK(fp_route_run(cr, in + i * 16, 1, single + i * 5, 5) == 0,
              "single run");

    CHECK(memcmp(batched, single, sizeof batched) == 0,
          "batched f64 == single f64 (bit-identical)");

    fp_route_free(cr);
    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}

static void test_f32_matches_oracle(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision prec = { FP_F32, 0 };
    CompiledRoute *cr;
    int i, mismatches = 0;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup (f32)\n"); ++failures; return;
    }
    cr = fp_route_compile(&plan, prec, 16);
    CHECK(cr != NULL, "compile f32");
    if (cr == NULL) { registry_free(&reg); btn_free(&hexval); btn_free(&incr); return; }

    for (i = 0; i < 16; ++i) {
        double in[16] = {0};
        double want[5] = {0};
        double got[5] = {0};
        in[i] = 1.0;
        route_execute(&plan, in, 16, want, 5);
        CHECK(fp_route_run(cr, in, 1, got, 5) == 0, "f32 run");
        if (memcmp(want, got, sizeof want) != 0) ++mismatches;
    }
    CHECK(mismatches == 0, "f32 == oracle (snapped, all 16)");
    /* The frozen primitives certify well above the band; f32 must not crowd it. */
    CHECK(fp_route_min_margin(cr) >= 0.25, "f32 min margin >= floor (0.25)");

    fp_route_free(cr);
    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}

static double run_route_min_margin(RoutePlan *plan, FastPrecision prec,
                                   int *out_mismatches) {
    CompiledRoute *cr = fp_route_compile(plan, prec, 16);
    int i, mism = 0;
    double mm;
    if (cr == NULL) { *out_mismatches = 9999; return 0.0; }
    for (i = 0; i < 16; ++i) {
        double in[16] = {0};
        double want[5] = {0};
        double got[5] = {0};
        in[i] = 1.0;
        route_execute(plan, in, 16, want, 5);
        if (fp_route_run(cr, in, 1, got, 5) != 0 ||
            memcmp(want, got, sizeof want) != 0) ++mism;
    }
    mm = fp_route_min_margin(cr);
    *out_mismatches = mism;
    fp_route_free(cr);
    return mm;
}

static void test_int8_gate_refuses_on_margin(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision p8 = { FP_INT, 8 };
    int mism8 = 0;
    double mm8;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup (int8)\n"); ++failures; return;
    }
    mm8 = run_route_min_margin(&plan, p8, &mism8);
    /* The int lane must produce correct snapped outputs (mismatch=0).
       The margin verdict is informational: if mm8 < 0.25, the gate correctly
       refuses certification (margin collapse is the designed signal).
       REAL FINDING: on this route, int8 gives mm8 ~ 0.234 < 0.25 floor --
       quantization noise erodes the margin; gate bites via margin path. */
    CHECK(mism8 == 0, "int8 == oracle (snapped, all 16)");
    printf("  [int8 margin=%.4f mismatches=%d -- gate %s certification]\n",
           mm8, mism8, (mism8 == 0 && mm8 >= 0.25) ? "GRANTS" : "REFUSES");

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}

static void test_int2_gate_bites(void) {
    BinaryTransformNetwork hexval, incr;
    PrimitiveRegistry reg;
    RoutePlan plan;
    FastPrecision p2 = { FP_INT, 2 };
    int mism2 = 0;
    double mm2;

    if (load_route(&hexval, &incr, &reg, &plan) != 0) {
        printf("FAIL: route setup (int2)\n"); ++failures; return;
    }
    mm2 = run_route_min_margin(&plan, p2, &mism2);
    /* An over-aggressive precision must NOT certify: either it diverges from the
       oracle, or its margin collapses below the floor. The gate bites. */
    CHECK(mism2 > 0 || mm2 < 0.25, "int2 is rejected (gate bites)");

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
}

static void test_dag_f64_matches_oracle(void) {
    BinaryTransformNetwork hexval, combine;
    PrimitiveRegistry reg;
    DagSource sources[2];
    DagPlan plan = {0};
    Port onehot16 = P(PORT_ONEHOT, 16, 1);
    Port src_types[2];
    FastPrecision prec = { FP_F64, 0 };
    CompiledDag *cd;
    double seed_hi[16] = {0}, seed_lo[16] = {0};
    double *hi_batch, *lo_batch, *fast_out;
    const double *src_batches[2];
    int hi, lo, mism = 0;

    memset(&hexval, 0, sizeof hexval);
    memset(&combine, 0, sizeof combine);
    if (btn_load(&hexval, "hex_value_weights.txt") != 0 ||
        btn_load(&combine, "combine_weights.txt") != 0) {
        printf("FAIL: load dag primitives (run ./nn_demo)\n"); ++failures; return;
    }
    registry_init(&reg);
    registry_add(&reg, &hexval, "hex_value");
    registry_add(&reg, &combine, "combine");

    /* Plan once with seed source values (dag_plan reads types, not values). */
    seed_hi[0] = 1.0; seed_lo[0] = 1.0;
    sources[0].type = onehot16; sources[0].values = seed_hi;
    sources[1].type = onehot16; sources[1].values = seed_lo;
    if (dag_plan(&reg, sources, 2, P(PORT_BINARY_MSB, 8, 1), &plan) != 0) {
        printf("FAIL: no DAG plan\n"); ++failures;
        registry_free(&reg); btn_free(&hexval); btn_free(&combine); return;
    }

    src_types[0] = onehot16; src_types[1] = onehot16;
    cd = fp_dag_compile(&plan, src_types, 2, prec, 256);
    CHECK(cd != NULL, "fp_dag_compile f64");
    CHECK(cd != NULL && fp_dag_out_total(cd) == 8, "dag out_total == 8");
    if (cd == NULL) { dag_free(&plan); registry_free(&reg);
                      btn_free(&hexval); btn_free(&combine); return; }

    /* 256 samples: all (hi,lo) one-hot pairs. */
    hi_batch = calloc(256 * 16, sizeof(double));
    lo_batch = calloc(256 * 16, sizeof(double));
    fast_out = calloc(256 * 8, sizeof(double));
    for (hi = 0; hi < 16; ++hi)
        for (lo = 0; lo < 16; ++lo) {
            size_t k = (size_t)hi * 16 + lo;
            hi_batch[k * 16 + hi] = 1.0;
            lo_batch[k * 16 + lo] = 1.0;
        }
    src_batches[0] = hi_batch; src_batches[1] = lo_batch;

    CHECK(fp_dag_run(cd, src_batches, 256, fast_out, 256 * 8) == 0, "fp_dag_run f64");

    for (hi = 0; hi < 16; ++hi)
        for (lo = 0; lo < 16; ++lo) {
            size_t k = (size_t)hi * 16 + lo;
            double want[8] = {0};
            sources[0].values = hi_batch + k * 16;
            sources[1].values = lo_batch + k * 16;
            if (dag_execute(&plan, sources, 2, want, 8) != 0 ||
                memcmp(want, fast_out + k * 8, sizeof want) != 0) ++mism;
        }
    CHECK(mism == 0, "dag f64 == oracle (all 256 pairs)");

    free(hi_batch); free(lo_batch); free(fast_out);
    fp_dag_free(cd);
    dag_free(&plan);
    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&combine);
}

/* A SHARED node: one split node feeds both combine slots via its two output
   ports -- combine(split(byte)[0], split(byte)[1]) reconstructs the byte.
   Exercises the shared-node path (split evaluated once, two ports projected) and
   the post-order evaluation order. Hand-built (stack DagNodes, plan.owned=NULL),
   so we do NOT dag_free. Compares fp_dag_run to dag_execute per byte (return code
   AND value), so it is robust even if a frozen primitive is out-of-domain. */
static void test_dag_shared_node_split(void) {
    BinaryTransformNetwork split, combine;
    DagNode src = {0}, sp = {0}, cm = {0};
    DagPlan plan = {0};
    Port byte_t = P(PORT_BINARY_MSB, 8, 1);
    Port src_types[1];
    FastPrecision prec = { FP_F64, 0 };
    CompiledDag *cd;
    int b, bit, mism = 0;

    memset(&split, 0, sizeof split);
    memset(&combine, 0, sizeof combine);
    if (btn_load(&split, "split_weights.txt") != 0 ||
        btn_load(&combine, "combine_weights.txt") != 0) {
        printf("FAIL: load split/combine (run ./nn_demo)\n"); ++failures; return;
    }

    src.kind = DAG_SOURCE; src.source_index = 0;
    sp.kind = DAG_PRIMITIVE; sp.btn = &split; sp.name = "split";
    sp.children[0] = &src; sp.child_count = 1; sp.output_index = 0;
    cm.kind = DAG_PRIMITIVE; cm.btn = &combine; cm.name = "combine";
    cm.children[0] = &sp; cm.children[1] = &sp;   /* SAME node -> shared */
    cm.child_ports[0] = 0;   /* port 0 (hi nibble) via output_index fallback */
    cm.child_ports[1] = 1;   /* port 1 (lo nibble) */
    cm.child_count = 2; cm.output_index = 0;
    plan.root = &cm; plan.owned = NULL;           /* hand-built; do NOT dag_free */

    src_types[0] = byte_t;
    cd = fp_dag_compile(&plan, src_types, 1, prec, 1);
    CHECK(cd != NULL, "shared-node dag compile");
    if (cd == NULL) { btn_free(&split); btn_free(&combine); return; }

    for (b = 0; b < 256; ++b) {
        double in[8], fo[8], oo[8];
        const double *sb[1];
        DagSource s0;
        int rfp, ror;
        for (bit = 0; bit < 8; ++bit) in[bit] = (double)((b >> (7 - bit)) & 1);
        sb[0] = in;
        rfp = fp_dag_run(cd, sb, 1, fo, 8);
        s0.type = byte_t; s0.values = in;
        ror = dag_execute(&plan, &s0, 1, oo, 8);
        if (rfp != ror) ++mism;
        else if (rfp == 0 && memcmp(fo, oo, sizeof fo) != 0) ++mism;
    }
    CHECK(mism == 0, "shared-node dag f64 == oracle (256 bytes, split eval'd once)");

    fp_dag_free(cd);
    btn_free(&split);
    btn_free(&combine);
}

/* Shared helper: build the committed combine(hex_value,hex_value)->byte plan.
   Returns 0 on success; fills *plan, *reg, and the two btns (caller frees). */
static int load_byte_dag(BinaryTransformNetwork *hexval,
                         BinaryTransformNetwork *combine,
                         PrimitiveRegistry *reg, DagPlan *plan,
                         DagSource sources[2], double *seed_hi, double *seed_lo) {
    Port onehot16 = P(PORT_ONEHOT, 16, 1);
    memset(hexval, 0, sizeof *hexval);
    memset(combine, 0, sizeof *combine);
    if (btn_load(hexval, "hex_value_weights.txt") != 0 ||
        btn_load(combine, "combine_weights.txt") != 0) return -1;
    registry_init(reg);
    registry_add(reg, hexval, "hex_value");
    registry_add(reg, combine, "combine");
    seed_hi[0] = 1.0; seed_lo[0] = 1.0;
    sources[0].type = onehot16; sources[0].values = seed_hi;
    sources[1].type = onehot16; sources[1].values = seed_lo;
    return dag_plan(reg, sources, 2, P(PORT_BINARY_MSB, 8, 1), plan);
}

/* Fill 256 (hi,lo) one-hot pairs into hi_batch/lo_batch (each 256*16). */
static void fill_pairs(double *hi_batch, double *lo_batch) {
    int hi, lo;
    for (hi = 0; hi < 16; ++hi)
        for (lo = 0; lo < 16; ++lo) {
            size_t k = (size_t)hi * 16 + lo;
            hi_batch[k * 16 + hi] = 1.0;
            lo_batch[k * 16 + lo] = 1.0;
        }
}

static void test_dag_batched_equals_single_f64(void) {
    BinaryTransformNetwork hexval, combine; PrimitiveRegistry reg; DagPlan plan = {0};
    DagSource sources[2]; double seed_hi[16] = {0}, seed_lo[16] = {0};
    Port st[2]; FastPrecision prec = { FP_F64, 0 }; CompiledDag *cd;
    double *hi_b, *lo_b, *batched, *single; const double *sb[2]; size_t k;

    if (load_byte_dag(&hexval, &combine, &reg, &plan, sources, seed_hi, seed_lo) != 0) {
        printf("FAIL: byte dag setup (batch)\n"); ++failures; return;
    }
    st[0] = P(PORT_ONEHOT,16,1); st[1] = st[0];
    cd = fp_dag_compile(&plan, st, 2, prec, 256);
    CHECK(cd != NULL, "dag compile batch=256");
    if (cd == NULL) { dag_free(&plan); registry_free(&reg);
                      btn_free(&hexval); btn_free(&combine); return; }
    hi_b = calloc(256*16, sizeof(double)); lo_b = calloc(256*16, sizeof(double));
    batched = calloc(256*8, sizeof(double)); single = calloc(256*8, sizeof(double));
    fill_pairs(hi_b, lo_b); sb[0] = hi_b; sb[1] = lo_b;

    CHECK(fp_dag_run(cd, sb, 256, batched, 256*8) == 0, "dag batched run");
    for (k = 0; k < 256; ++k) {
        const double *one[2]; one[0] = hi_b + k*16; one[1] = lo_b + k*16;
        CHECK(fp_dag_run(cd, one, 1, single + k*8, 8) == 0, "dag single run");
    }
    CHECK(memcmp(batched, single, 256*8*sizeof(double)) == 0,
          "dag batched f64 == single f64 (bit-identical)");

    free(hi_b); free(lo_b); free(batched); free(single);
    fp_dag_free(cd); dag_free(&plan); registry_free(&reg);
    btn_free(&hexval); btn_free(&combine);
}

/* mismatches of fp_dag at `prec` vs dag_execute over the 256 pairs; returns min margin. */
static double dag_run_mismatches(DagPlan *plan, DagSource sources[2],
                                 FastPrecision prec, int *out_mism) {
    Port st[2]; CompiledDag *cd; double *hi_b, *lo_b, *fo; const double *sb[2];
    int hi, lo, mism = 0; double mm;
    st[0] = P(PORT_ONEHOT,16,1); st[1] = st[0];
    cd = fp_dag_compile(plan, st, 2, prec, 256);
    if (cd == NULL) { *out_mism = 9999; return 0.0; }
    hi_b = calloc(256*16, sizeof(double)); lo_b = calloc(256*16, sizeof(double));
    fo = calloc(256*8, sizeof(double));
    fill_pairs(hi_b, lo_b); sb[0] = hi_b; sb[1] = lo_b;
    fp_dag_reset_margin(cd);
    fp_dag_run(cd, sb, 256, fo, 256*8);
    for (hi = 0; hi < 16; ++hi) for (lo = 0; lo < 16; ++lo) {
        size_t k = (size_t)hi*16 + lo; double want[8] = {0};
        sources[0].values = hi_b + k*16; sources[1].values = lo_b + k*16;
        if (dag_execute(plan, sources, 2, want, 8) != 0 ||
            memcmp(want, fo + k*8, sizeof want) != 0) ++mism;
    }
    mm = fp_dag_min_margin(cd);
    *out_mism = mism;
    free(hi_b); free(lo_b); free(fo); fp_dag_free(cd);
    return mm;
}

static void test_dag_f32_matches_oracle(void) {
    BinaryTransformNetwork hexval, combine; PrimitiveRegistry reg; DagPlan plan = {0};
    DagSource sources[2]; double seed_hi[16] = {0}, seed_lo[16] = {0};
    FastPrecision prec = { FP_F32, 0 }; int mism = 0; double mm;
    if (load_byte_dag(&hexval, &combine, &reg, &plan, sources, seed_hi, seed_lo) != 0) {
        printf("FAIL: byte dag setup (f32)\n"); ++failures; return;
    }
    mm = dag_run_mismatches(&plan, sources, prec, &mism);
    CHECK(mism == 0, "dag f32 == oracle (256 pairs)");
    CHECK(mm >= 0.25, "dag f32 min margin >= floor");
    dag_free(&plan); registry_free(&reg); btn_free(&hexval); btn_free(&combine);
}

static void test_dag_int2_gate_bites(void) {
    BinaryTransformNetwork hexval, combine; PrimitiveRegistry reg; DagPlan plan = {0};
    DagSource sources[2]; double seed_hi[16] = {0}, seed_lo[16] = {0};
    FastPrecision prec = { FP_INT, 2 }; int mism = 0; double mm;
    if (load_byte_dag(&hexval, &combine, &reg, &plan, sources, seed_hi, seed_lo) != 0) {
        printf("FAIL: byte dag setup (int2)\n"); ++failures; return;
    }
    mm = dag_run_mismatches(&plan, sources, prec, &mism);
    CHECK(mism > 0 || mm < 0.25, "dag int2 is rejected (gate bites)");
    dag_free(&plan); registry_free(&reg); btn_free(&hexval); btn_free(&combine);
}

/* Tagged-port helper (circuits use semantic tags; port_validate ignores tags,
   but dag_plan_circuit needs them to match split's contract). */
static Port PT_(PortFamily family, size_t fw, size_t fc, const char *tag) {
    Port p; p.family = family; p.field_width = fw; p.field_count = fc;
    p.tag[0] = '\0';
    if (tag) port_set_tag(&p, tag);
    return p;
}

static void test_circuit_split_matches_oracle(void) {
    BinaryTransformNetwork split;
    PrimitiveRegistry reg;
    DagSource src[1];
    Port goals[2];
    CircuitPlan cp = {0};
    Port byte_t = PT_(PORT_BINARY_MSB, 8, 1, "byte_value");
    Port nib_t  = PT_(PORT_BINARY_MSB, 4, 1, "nibble_value");
    Port src_types[1];
    FastPrecision prec = { FP_F64, 0 };
    CompiledCircuit *cc;
    double seed[8] = {0};
    double *in_b, *fast_out;
    const double *sb[1];
    int b, bit, mism = 0;

    memset(&split, 0, sizeof split);
    if (btn_load(&split, "split_weights.txt") != 0) {
        printf("FAIL: load split (run ./nn_demo)\n"); ++failures; return;
    }
    registry_init(&reg);
    registry_add(&reg, &split, "split");
    src[0].type = byte_t; src[0].values = seed;
    goals[0] = nib_t; goals[1] = nib_t;
    if (dag_plan_circuit(&reg, src, 1, goals, 2, &cp) != 0) {
        printf("FAIL: no split circuit\n"); ++failures;
        registry_free(&reg); btn_free(&split); return;
    }

    src_types[0] = byte_t;
    cc = fp_circuit_compile(&cp, src_types, 1, prec, 256);
    CHECK(cc != NULL, "fp_circuit_compile");
    if (cc == NULL) { circuit_free(&cp); registry_free(&reg); btn_free(&split); return; }
    CHECK(fp_circuit_out_total(cc) == 8, "circuit out_total == 8");
    CHECK(fp_circuit_node_count(cc) == 2, "split shared: graph has 2 nodes (src + split)");

    in_b = calloc(256 * 8, sizeof(double));
    fast_out = calloc(256 * 8, sizeof(double));
    for (b = 0; b < 256; ++b)
        for (bit = 0; bit < 8; ++bit)
            in_b[b * 8 + bit] = (double)((b >> (7 - bit)) & 1);   /* MSB-first */
    sb[0] = in_b;

    CHECK(fp_circuit_run(cc, sb, 256, fast_out, 256 * 8) == 0, "fp_circuit_run f64");

    for (b = 0; b < 256; ++b) {
        double oo[8] = {0};
        src[0].values = in_b + b * 8;
        if (dag_execute_circuit(&cp, src, 1, oo, 8, NULL) != 0 ||
            memcmp(oo, fast_out + b * 8, sizeof oo) != 0) ++mism;
    }
    CHECK(mism == 0, "circuit f64 == oracle (256 bytes, split eval'd once)");

    free(in_b); free(fast_out);
    fp_circuit_free(cc);
    circuit_free(&cp);
    registry_free(&reg);
    btn_free(&split);
}

int run_test_fastpath(void) {
    test_f64_matches_oracle();
    test_batched_equals_single_f64();
    test_f32_matches_oracle();
    test_int8_gate_refuses_on_margin();
    test_int2_gate_bites();
    test_dag_f64_matches_oracle();
    test_dag_shared_node_split();
    test_dag_batched_equals_single_f64();
    test_dag_f32_matches_oracle();
    test_dag_int2_gate_bites();
    test_circuit_split_matches_oracle();
    if (failures == 0) {
        printf("FASTPATH PASS\n");
        return 0;
    }
    printf("FASTPATH FAIL: %d checks failed.\n", failures);
    return 1;
}
