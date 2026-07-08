/*
 * throughput_study.c -- how fast can the frozen route push bytes, and which
 * lever buys the speed? Runs the README hex_value -> increment route over a
 * large buffer through the double oracle (route_execute) and the fast lane at
 * each precision, and prints a capacity_results-style table:
 *
 *   lever | ns/sample | speedup vs L0 | mismatches/N | min margin | certified?
 *
 * Faithfulness bar: identical SNAPPED output to the oracle. A precision lever is
 * certified iff mismatches == 0 AND min margin >= FP_MARGIN_FLOOR. Phase 1
 * reports the verdict; it does not reject. Standalone study: NOT part of make
 * test. Loads the committed frozen hex_value/increment weights (run ./nn_demo).
 *
 * Timing note: this build is -mno-avx and single-threaded; the numbers are a
 * within-machine comparison of levers, not an absolute bytes/sec claim.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/fastpath.h"
#include "../include/consolidate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <omp.h>  /* for omp_get_max_threads etc. */

#define N      200000   /* samples in the buffer */
#define BATCH  1024     /* fast-lane window */
#define FP_MARGIN_FLOOR 0.25

/* Local copies of N and BATCH so they can appear safely in OpenMP clauses
   without macro expansion breaking the pragma (default(none) rule). */
static const size_t NN = N;
static const size_t BATCHV = BATCH;

static Port P(PortFamily f, size_t fw, size_t fc) {
    Port p; p.family = f; p.field_width = fw; p.field_count = fc;
    p.tag[0] = '\0'; return p;
}

/* A simple deterministic generator of valid one-hot-16 inputs. */
static void fill_inputs(double *in, size_t n) {
    size_t k;
    for (k = 0; k < n; ++k) {
        memset(in + k * 16, 0, 16 * sizeof(double));
        in[k * 16 + (k % 16)] = 1.0;
    }
}

static double now_seconds(void) {
    return (double)clock() / (double)CLOCKS_PER_SEC;
}

int main(void) {
    BinaryTransformNetwork hexval = {0}, incr = {0};
    PrimitiveRegistry reg;
    RoutePlan plan;
    double *in = malloc((size_t)N * 16 * sizeof(double));
    double *oracle_out = malloc((size_t)N * 5 * sizeof(double));
    double *fast_out = malloc((size_t)N * 5 * sizeof(double));
    double t0, t1, base_ns = 0.0;
    size_t k;

    if (!in || !oracle_out || !fast_out) { fprintf(stderr, "OOM\n"); return 1; }
    if (btn_load(&hexval, "hex_value_weights.txt") != 0 ||
        btn_load(&incr, "increment_weights.txt") != 0) {
        fprintf(stderr, "could not load frozen primitives (run ./nn_demo).\n");
        return 1;
    }
    registry_init(&reg);
    registry_add(&reg, &hexval, "hex_value");
    registry_add(&reg, &incr, "increment");
    if (route_plan(&reg, P(PORT_ONEHOT,16,1), P(PORT_BINARY_MSB,5,1), &plan) != 0) {
        fprintf(stderr, "no route\n"); return 1;
    }
    fill_inputs(in, N);

    printf("throughput study: hex_value -> increment over N=%d (batch=%d)\n", N, BATCH);
    printf("OpenMP: max %d threads (use $env:OMP_NUM_THREADS=1; .\\throughput_study.exe for single-thread regression)\n\n", omp_get_max_threads());
    /* All parallel regions follow the rules: default(none) always; OMP_NUM_THREADS=1
       for Binary Bug Search (race vs logic); -fdump-tree-ompexp for compiler view;
       TSan via WSL workaround. OMP_NUM_THREADS=1 is default for deterministic runs. */
    printf(" lever      | ns/sample | speedup | mism/N  | min margin | certified\n");
    printf(" -----------|-----------|---------|---------|------------|----------\n");

    /* L0: the oracle (route_execute), per call (malloc/call, double).
       Now parallelized with OpenMP (default(none) rule followed).
       This makes the baseline a multi-thread reference; speedups show
       relative gain of fast lanes under the same threading. */
    t0 = now_seconds();
    #pragma omp parallel for default(none) \
        shared(plan, in, oracle_out, NN)
    for (k = 0; k < (size_t)NN; ++k)
        route_execute(&plan, in + k * 16, 16, oracle_out + k * 5, 5);
    t1 = now_seconds();
    base_ns = (t1 - t0) * 1e9 / N;
    printf(" L0 oracle  | %9.1f | %7s | %7d | %10s | %s\n",
           base_ns, "1.00x", 0, "-", "ref (parallel)");

    /* L1..L4: compile once, run in batches; compare snapped output to oracle. */
    {
        struct { const char *name; FastPrecision prec; } levers[] = {
            { "L1 f64*1",  { FP_F64, 0 } },   /* batch=1: scratch reuse only */
            { "L2 f64",    { FP_F64, 0 } },   /* batched */
            { "L3 f32",    { FP_F32, 0 } },
            { "L4 int8",   { FP_INT, 8 } },
        };
        size_t L;
        for (L = 0; L < sizeof levers / sizeof levers[0]; ++L) {
            size_t batch = (L == 0) ? 1 : BATCH;
            CompiledRoute *cr = fp_route_compile(&plan, levers[L].prec, batch);
            size_t off, mism = 0;
            double ns, mm;
            int run_ok = 1;
            if (cr == NULL) { printf(" %-10s | compile failed\n", levers[L].name); continue; }
            fp_route_reset_margin(cr);
            t0 = now_seconds();
            size_t cnt;
            #pragma omp parallel for default(none) \
                shared(plan, cr, in, fast_out, NN, batch, run_ok) private(cnt)
            for (off = 0; off < (size_t)NN; off += batch) {
                cnt = ((size_t)NN - off < batch) ? (size_t)NN - off : batch;
                if (fp_route_run(cr, in + off * 16, cnt,
                                 fast_out + off * 5, cnt * 5) != 0) run_ok = 0;
            }
            t1 = now_seconds();
            if (run_ok) {
                ns = (t1 - t0) * 1e9 / N;
                mism = 0;
                /* OpenMP parallel mismatch check (demo of threads + one exe).
                   Golden Rule: default(none) always.
                   To debug / regression: $env:OMP_NUM_THREADS=1; .\throughput_study.exe
                   (bug gone = race; bug stays = logic error). OMP_NUM_THREADS=1 is the
                   recommended default for deterministic regression runs of this study.
                   For deeper insight on MinGW: gcc -fopenmp -fdump-tree-ompexp ...
                   TSan for races: use WSL with -fsanitize=thread (not native on Windows). */
                #pragma omp parallel for default(none) \
                    shared(fast_out, oracle_out, NN) reduction(+:mism)
                for (k = 0; k < (size_t)NN * 5; ++k) {
                    if (fast_out[k] != oracle_out[k]) mism++;
                }
                mm = fp_route_min_margin(cr);
                printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                       levers[L].name, ns, base_ns / ns, (unsigned long)mism, mm,
                       (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
            } else {
                printf(" %-10s | run failed\n", levers[L].name);
            }
            fp_route_free(cr);
        }
    }

    /* L5: consolidate the 2-step route into ONE chunk, run it as a length-1
       route through the same lane. Fewer snaps/byte; the chunk is wider, so the
       study tells us whether collapsing depth actually pays here. */
    {
        BinaryTransformNetwork chunk = {0};
        ConsolidateReport rep;
        if (consolidate_route(&plan, NULL, &chunk, &rep) != 0) {
            printf(" L5 chunk   | consolidation refused (verify rate below 1.0)\n");
        } else {
            RoutePlan cplan;
            FastPrecision prec = { FP_F64, 0 };
            CompiledRoute *cr;
            memset(&cplan, 0, sizeof cplan);
            cplan.steps[0] = &chunk;
            cplan.names[0] = "chunk";
            cplan.length = 1;
            cplan.goal = chunk.output_ports[0];
            cr = fp_route_compile(&cplan, prec, BATCH);
            if (cr == NULL) {
                printf(" L5 chunk   | compile failed\n");
            } else {
                size_t off, mism = 0;
                double ns, mm;
                int run_ok = 1;
                fp_route_reset_margin(cr);
                t0 = now_seconds();
                size_t cnt;
                #pragma omp parallel for default(none) \
                    shared(cr, in, fast_out, NN, BATCHV, run_ok) private(cnt)
                for (off = 0; off < (size_t)NN; off += BATCHV) {
                    cnt = ((size_t)NN - off < BATCHV) ? (size_t)NN - off : BATCHV;
                    if (fp_route_run(cr, in + off * 16, cnt,
                                     fast_out + off * 5, cnt * 5) != 0) run_ok = 0;
                }
                t1 = now_seconds();
                if (run_ok) {
                    ns = (t1 - t0) * 1e9 / N;
                    mism = 0;
                    #pragma omp parallel for default(none) \
                        shared(fast_out, oracle_out, NN) reduction(+:mism)
                    for (k = 0; k < (size_t)NN * 5; ++k) {
                        if (fast_out[k] != oracle_out[k]) mism++;
                    }
                    mm = fp_route_min_margin(cr);
                    printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                           "L5 chunk", ns, base_ns / ns, (unsigned long)mism, mm,
                           (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
                    printf("   (chunk %lu hidden, verified %lu/%lu, "
                           "teacher-aborts %lu; route was 2 steps)\n",
                           (unsigned long)chunk.hidden_count,
                           (unsigned long)rep.verified, (unsigned long)rep.samples,
                           (unsigned long)rep.teacher_aborts);
                } else {
                    printf(" L5 chunk   | run failed\n");
                }
                fp_route_free(cr);
            }
            btn_free(&chunk);
        }
    }

    /* ===== DAG lane: combine(hex_value, hex_value) -> byte ===== */
    {
        BinaryTransformNetwork combine = {0};
        PrimitiveRegistry dreg;
        DagSource dsrc[2];
        DagPlan dplan = {0};
        Port onehot16 = P(PORT_ONEHOT, 16, 1);
        Port stypes[2];
        double seed_hi[16] = {0}, seed_lo[16] = {0};
        double *hi_b = malloc((size_t)N * 16 * sizeof(double));
        double *lo_b = malloc((size_t)N * 16 * sizeof(double));
        double *dor  = malloc((size_t)N * 8 * sizeof(double));   /* dag oracle out */
        double *dfo  = malloc((size_t)N * 8 * sizeof(double));   /* dag fast out  */
        double dbase_ns = 0.0;
        size_t kk;

        if (!hi_b || !lo_b || !dor || !dfo ||
            btn_load(&combine, "combine_weights.txt") != 0) {
            fprintf(stderr, "skip DAG study (combine load / OOM)\n");
        } else {
            registry_init(&dreg);
            registry_add(&dreg, &hexval, "hex_value");   /* hexval already loaded above */
            registry_add(&dreg, &combine, "combine");
            seed_hi[0] = 1.0; seed_lo[0] = 1.0;
            dsrc[0].type = onehot16; dsrc[0].values = seed_hi;
            dsrc[1].type = onehot16; dsrc[1].values = seed_lo;
            stypes[0] = onehot16; stypes[1] = onehot16;

            for (kk = 0; kk < (size_t)N; ++kk) {
                memset(hi_b + kk*16, 0, 16*sizeof(double));
                memset(lo_b + kk*16, 0, 16*sizeof(double));
                hi_b[kk*16 + (kk % 16)] = 1.0;
                lo_b[kk*16 + ((kk/16) % 16)] = 1.0;
            }

            if (dag_plan(&dreg, dsrc, 2, P(PORT_BINARY_MSB,8,1), &dplan) != 0) {
                fprintf(stderr, "skip DAG study (no plan)\n");
            } else {
                struct { const char *name; FastPrecision prec; } dlevers[] = {
                    { "D1 f64*1", { FP_F64, 0 } },
                    { "D2 f64",   { FP_F64, 0 } },
                    { "D3 f32",   { FP_F32, 0 } },
                    { "D4 int8",  { FP_INT, 8 } },
                };
                size_t L;

                printf("\nDAG: combine(hex_value,hex_value) -> byte, N=%d\n\n", N);
                printf(" lever      | ns/sample | speedup | mism/N  | min margin | certified\n");
                printf(" -----------|-----------|---------|---------|------------|----------\n");

                /* D0 oracle: dag_execute per sample (fills dor). Now parallel. */
                t0 = now_seconds();
                #pragma omp parallel for default(none) \
                    shared(dplan, dsrc, hi_b, lo_b, dor, NN)
                for (kk = 0; kk < (size_t)NN; ++kk) {
                    dsrc[0].values = hi_b + kk*16; dsrc[1].values = lo_b + kk*16;
                    dag_execute(&dplan, dsrc, 2, dor + kk*8, 8);
                }
                t1 = now_seconds();
                dbase_ns = (t1 - t0) * 1e9 / N;
                printf(" D0 oracle  | %9.1f | %7s | %7d | %10s | %s\n",
                       dbase_ns, "1.00x", 0, "-", "ref (parallel)");

                for (L = 0; L < sizeof dlevers / sizeof dlevers[0]; ++L) {
                    size_t batch = (L == 0) ? 1 : BATCH, off, mism = 0;
                    int run_ok = 1; double ns, mm;
                    CompiledDag *cd = fp_dag_compile(&dplan, stypes, 2, dlevers[L].prec, batch);
                    if (cd == NULL) { printf(" %-10s | compile failed\n", dlevers[L].name); continue; }
                    fp_dag_reset_margin(cd);
                    t0 = now_seconds();
                    size_t cnt;
                    const double *w[2];
                    #pragma omp parallel for default(none) \
                        shared(cd, hi_b, lo_b, dfo, NN, batch, run_ok) private(cnt, w)
                    for (off = 0; off < (size_t)NN; off += batch) {
                        cnt = ((size_t)NN - off < batch) ? (size_t)NN - off : batch;
                        w[0] = hi_b + off*16; w[1] = lo_b + off*16;
                        if (fp_dag_run(cd, w, cnt, dfo + off*8, cnt*8) != 0) run_ok = 0;
                    }
                    t1 = now_seconds();
                    if (run_ok) {
                        ns = (t1 - t0) * 1e9 / N;
                        mism = 0;
                        #pragma omp parallel for default(none) \
                            shared(dfo, dor, NN) reduction(+:mism)
                        for (kk = 0; kk < (size_t)NN * 8; ++kk)
                            if (dfo[kk] != dor[kk]) mism++;
                        mm = fp_dag_min_margin(cd);
                        printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                               dlevers[L].name, ns, dbase_ns / ns, (unsigned long)mism, mm,
                               (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
                    } else {
                        printf(" %-10s | run failed\n", dlevers[L].name);
                    }
                    fp_dag_free(cd);
                }

                /* D5: consolidate the DAG into ONE 2-input chunk, run it as a
                   1-primitive DAG through the same lane. */
                {
                    BinaryTransformNetwork chunk = {0};
                    ConsolidateReport rep;
                    if (consolidate_dag(&dplan, dsrc, 2, NULL, &chunk, &rep) != 0) {
                        printf(" D5 chunk   | consolidation refused\n");
                    } else {
                        PrimitiveRegistry creg;
                        DagPlan cplan = {0};
                        registry_init(&creg);
                        registry_add(&creg, &chunk, "byte_chunk");
                        if (dag_plan(&creg, dsrc, 2, P(PORT_BINARY_MSB,8,1), &cplan) != 0) {
                            printf(" D5 chunk   | replan failed\n");
                        } else {
                            CompiledDag *cd = fp_dag_compile(&cplan, stypes, 2,
                                                             (FastPrecision){FP_F64,0}, BATCH);
                            if (cd == NULL) { printf(" D5 chunk   | compile failed\n"); }
                            else {
                                size_t off, mism = 0; int run_ok = 1; double ns, mm;
                                fp_dag_reset_margin(cd);
                                t0 = now_seconds();
                                size_t cnt;
                                const double *w[2];
                                #pragma omp parallel for default(none) \
                                    shared(cd, hi_b, lo_b, dfo, NN, BATCHV, run_ok) private(cnt, w)
                                for (off = 0; off < (size_t)NN; off += BATCHV) {
                                    cnt = ((size_t)NN - off < BATCHV) ? (size_t)NN - off : BATCHV;
                                    w[0] = hi_b + off*16; w[1] = lo_b + off*16;
                                    if (fp_dag_run(cd, w, cnt, dfo + off*8, cnt*8) != 0) run_ok = 0;
                                }
                                t1 = now_seconds();
                                if (run_ok) {
                                    ns = (t1 - t0) * 1e9 / N;
                                    mism = 0;
                                    #pragma omp parallel for default(none) \
                                        shared(dfo, dor, NN) reduction(+:mism)
                                    for (kk = 0; kk < (size_t)NN * 8; ++kk)
                                        if (dfo[kk] != dor[kk]) mism++;
                                    mm = fp_dag_min_margin(cd);
                                    printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                                           "D5 chunk", ns, dbase_ns / ns, (unsigned long)mism, mm,
                                           (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
                                    printf("   (chunk %lu hidden, verified %lu/%lu, teacher-aborts %lu)\n",
                                           (unsigned long)chunk.hidden_count,
                                           (unsigned long)rep.verified, (unsigned long)rep.samples,
                                           (unsigned long)rep.teacher_aborts);
                                } else {
                                    printf(" D5 chunk   | run failed\n");
                                }
                                fp_dag_free(cd);
                            }
                            dag_free(&cplan);
                        }
                        registry_free(&creg);
                        btn_free(&chunk);
                    }
                }

                dag_free(&dplan);
            }
            registry_free(&dreg);
            btn_free(&combine);
        }
        free(hi_b); free(lo_b); free(dor); free(dfo);
    }

    /* ===== Circuit lane: split(byte) -> {hi nibble, lo nibble}, one shared node ===== */
    {
        BinaryTransformNetwork csplit = {0};
        PrimitiveRegistry creg2;
        DagSource csrc[1];
        Port cgoals[2];
        CircuitPlan ccp = {0};
        Port byte_t = P(PORT_BINARY_MSB, 8, 1);
        Port nib_t  = P(PORT_BINARY_MSB, 4, 1);
        Port cstypes[1];
        double seed[8] = {0};
        double *cin = malloc((size_t)N * 8 * sizeof(double));
        double *cor = malloc((size_t)N * 8 * sizeof(double));   /* circuit oracle out */
        double *cfo = malloc((size_t)N * 8 * sizeof(double));   /* circuit fast out  */
        double cbase_ns = 0.0;
        size_t kk;

        /* split's circuit needs matching semantic tags. */
        port_set_tag(&byte_t, "byte_value");
        port_set_tag(&nib_t, "nibble_value");

        if (!cin || !cor || !cfo || btn_load(&csplit, "split_weights.txt") != 0) {
            fprintf(stderr, "skip circuit study (split load / OOM)\n");
        } else {
            registry_init(&creg2);
            registry_add(&creg2, &csplit, "split");
            csrc[0].type = byte_t; csrc[0].values = seed;
            cgoals[0] = nib_t; cgoals[1] = nib_t;
            cstypes[0] = byte_t;

            for (kk = 0; kk < (size_t)N; ++kk) {
                size_t bit; unsigned bv = (unsigned)(kk & 0xFF);
                for (bit = 0; bit < 8; ++bit)
                    cin[kk*8 + bit] = (double)((bv >> (7 - bit)) & 1u);
            }

            if (dag_plan_circuit(&creg2, csrc, 1, cgoals, 2, &ccp) != 0) {
                fprintf(stderr, "skip circuit study (no plan)\n");
            } else {
                struct { const char *name; FastPrecision prec; } clevers[] = {
                    { "C1 f64*1", { FP_F64, 0 } },
                    { "C2 f64",   { FP_F64, 0 } },
                    { "C3 f32",   { FP_F32, 0 } },
                    { "C4 int8",  { FP_INT, 8 } },
                };
                size_t L;

                printf("\nCIRCUIT: split(byte) -> {hi,lo} (one shared node), N=%d\n\n", N);
                printf(" lever      | ns/sample | speedup | mism/N  | min margin | certified\n");
                printf(" -----------|-----------|---------|---------|------------|----------\n");

                /* C0 oracle: dag_execute_circuit per sample (fills cor). Now parallel. */
                t0 = now_seconds();
                #pragma omp parallel for default(none) \
                    shared(ccp, csrc, cin, cor, NN)
                for (kk = 0; kk < (size_t)NN; ++kk) {
                    csrc[0].values = cin + kk*8;
                    dag_execute_circuit(&ccp, csrc, 1, cor + kk*8, 8);
                }
                t1 = now_seconds();
                cbase_ns = (t1 - t0) * 1e9 / N;
                printf(" C0 oracle  | %9.1f | %7s | %7d | %10s | %s\n",
                       cbase_ns, "1.00x", 0, "-", "ref (parallel)");

                for (L = 0; L < sizeof clevers / sizeof clevers[0]; ++L) {
                    size_t batch = (L == 0) ? 1 : BATCH, off, mism = 0;
                    int run_ok = 1; double ns, mm;
                    CompiledCircuit *cc = fp_circuit_compile(&ccp, cstypes, 1,
                                                             clevers[L].prec, batch);
                    if (cc == NULL) { printf(" %-10s | compile failed\n", clevers[L].name); continue; }
                    fp_circuit_reset_margin(cc);
                    t0 = now_seconds();
                    size_t cnt;
                    const double *w[1];
                    #pragma omp parallel for default(none) \
                        shared(cc, cin, cfo, NN, batch, run_ok) private(cnt, w)
                    for (off = 0; off < (size_t)NN; off += batch) {
                        cnt = ((size_t)NN - off < batch) ? (size_t)NN - off : batch;
                        w[0] = cin + off*8;
                        if (fp_circuit_run(cc, w, cnt, cfo + off*8, cnt*8) != 0) run_ok = 0;
                    }
                    t1 = now_seconds();
                    if (run_ok) {
                        ns = (t1 - t0) * 1e9 / N;
                        mism = 0;
                        #pragma omp parallel for default(none) \
                            shared(cfo, cor, NN) reduction(+:mism)
                        for (kk = 0; kk < (size_t)NN * 8; ++kk)
                            if (cfo[kk] != cor[kk]) mism++;
                        mm = fp_circuit_min_margin(cc);
                        printf(" %-10s | %9.1f | %6.2fx | %7lu | %10.3f | %s\n",
                               clevers[L].name, ns, cbase_ns / ns, (unsigned long)mism, mm,
                               (mism == 0 && mm >= FP_MARGIN_FLOOR) ? "YES" : "no");
                    } else {
                        printf(" %-10s | run failed\n", clevers[L].name);
                    }
                    fp_circuit_free(cc);
                }
                circuit_free(&ccp);
            }
            registry_free(&creg2);
            btn_free(&csplit);
        }
        free(cin); free(cor); free(cfo);
    }

    registry_free(&reg);
    btn_free(&hexval);
    btn_free(&incr);
    free(in); free(oracle_out); free(fast_out);
    return 0;
}
