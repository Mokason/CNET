/* tests/distillation_gate_bench.c -- EVIDENCE_CLEAR overhead + canonical-digest sweep cost.
 * NOT part of make test; budgeted study. */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/scan.h"
#include "../include/library.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ---- port helpers ---- */
static Port PTG(PortFamily family, size_t field_width, size_t field_count) {
    Port p; p.family = family; p.field_width = field_width;
    p.field_count = field_count; p.tag[0] = '\0';
    return p;
}

/* ---- evidence seeder ---- */
static void gate_set_ev(BinaryTransformNetwork *b, unsigned long s, unsigned long f) {
    b->output_successes = s; b->output_failures = f;
}

/* ---- dec/inc training (mirrors test_distillation_gate.c) ---- */
static void msb2g(int i, double *o) { o[0] = (double)((i >> 1) & 1); o[1] = (double)(i & 1); }
static void msb3g(int i, double *o) {
    o[0] = (double)((i >> 2) & 1); o[1] = (double)((i >> 1) & 1); o[2] = (double)(i & 1);
}

static int ev_make_dec(BinaryTransformNetwork *b) {
    double in[4][4] = {{0}}; double tg[4][2]; int i;
    if (btn_init(b, 4, 2, 1, 16, 0.8, 11u) != 0) return -1;
    if (btn_set_ports(b, PTG(PORT_ONEHOT, 4, 1), PTG(PORT_BINARY_MSB, 2, 1)) != 0) return -1;
    for (i = 0; i < 4; ++i) { in[i][i] = 1.0; msb2g(i, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4, 60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

static int ev_make_inc(BinaryTransformNetwork *b) {
    double in[4][2]; double tg[4][3]; int i;
    if (btn_init(b, 2, 3, 1, 16, 0.8, 13u) != 0) return -1;
    if (btn_set_ports(b, PTG(PORT_BINARY_MSB, 2, 1), PTG(PORT_BINARY_MSB, 3, 1)) != 0) return -1;
    for (i = 0; i < 4; ++i) { msb2g(i, in[i]); msb3g(i + 1, tg[i]); }
    return btn_train_dynamic(b, &in[0][0], &tg[0][0], 4, 60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* Build dec+inc trained primitives into dec_out and inc_out.
   Returns 0 on success, -1 on training failure. */
static int build_base_prims(BinaryTransformNetwork *dec_out,
                             BinaryTransformNetwork *inc_out) {
    memset(dec_out, 0, sizeof *dec_out);
    memset(inc_out, 0, sizeof *inc_out);
    if (ev_make_dec(dec_out) != 0) return -1;
    if (ev_make_inc(inc_out) != 0) { btn_free(dec_out); memset(dec_out, 0, sizeof *dec_out); return -1; }
    return 0;
}

/* One run of the evolve operation (legacy or gated). Builds a fresh registry from
   pre-trained prototypes (weights are copied via btn_save_to_buffer + restore approach;
   since we have no copy API, we re-train from scratch -- which is expensive but
   necessary to isolate the evolve overhead).

   We use pre-trained weights arrays to avoid re-training: caller provides
   serialized weight buffers. Actually, since btn_save/load would add more code,
   we just accept the timing will include the distillation overhead per-run,
   and note that the delta between legacy and gated is what matters. */

/* ---- lure fixture for canonical digest ---- */
static int gate_build_prod(BinaryTransformNetwork *b, const char *goal_tag) {
    Port in = PTG(PORT_ONEHOT, 4, 1);
    Port out = PTG(PORT_ONEHOT, 4, 1);
    port_set_tag(&in, "x");
    port_set_tag(&out, goal_tag);
    memset(b, 0, sizeof *b);
    if (btn_init(b, 4, 4, 1, 4, 0.5, 1u) != 0) return -1;
    return btn_set_ports(b, in, out);
}

int main(void) {
    /* ================================================================
     * PART 1: EVIDENCE_CLEAR gate overhead
     * We time R full library_evolve / library_evolve_gated calls,
     * rebuilding the registry from pre-trained primitives each time.
     * Since training dominates, we time ONLY the evolve call,
     * training the prototypes ONCE outside the timed loop and
     * re-using the weights by re-training with 0 iterations (which
     * is not possible). Instead: train once, record that the fixture
     * is valid, then do a single "warm" evolve timing pair.
     *
     * The real question is: how much does the gate check cost
     * RELATIVE to the bulk work of library_evolve? We time both
     * over R runs where each run rebuilds the registry (not the
     * training, just the registry + evolve call). To do this without
     * a copy API, we time many registry-rebuild+evolve cycles on
     * already-trained btn structs by calling btn_init + setting ports
     * + copying weights manually via the btn's weight arrays.
     *
     * Simplest correct approach: train once, time evolve only by
     * calling it R times each on a freshly-init'd registry that
     * borrows the same already-trained primitives (no retraining).
     * This IS the right denominator since evolve=plan+consolidate.
     * ================================================================ */

    printf("=== distillation_gate_bench ===\n\n");

    /* --- train the prototype primitives once --- */
    BinaryTransformNetwork dec_proto = {0}, inc_proto = {0};
    printf("Training prototype primitives (once)...\n");
    if (build_base_prims(&dec_proto, &inc_proto) != 0) {
        printf("FAIL: prototype training failed\n");
        return 1;
    }
    printf("Training done.\n\n");

    /* Seed evidence so the gate clears (reliability = 17/18 >= 0.9) */
    gate_set_ev(&dec_proto, 16, 0);
    gate_set_ev(&inc_proto, 16, 0);

    LibraryTask task;
    memset(&task, 0, sizeof task);
    task.name = "chunk_inc4";
    task.sources[0] = PTG(PORT_ONEHOT, 4, 1);
    task.n_sources = 1;
    task.goal = PTG(PORT_BINARY_MSB, 3, 1);

    LibraryGateConfig gconf;
    library_gate_config_defaults(&gconf);
    gconf.enabled = 1;

    /* Determine R: run a calibration pass of 1 legacy evolve to see how long it takes */
    {
        PrimitiveRegistry reg;
        LibraryReport report;
        clock_t t0, t1;
        double single_ms;

        registry_init(&reg);
        registry_add(&reg, &dec_proto, "dec");
        registry_add(&reg, &inc_proto, "inc");

        t0 = clock();
        library_evolve(&reg, &task, 1, NULL, 0, NULL, 4, &report);
        t1 = clock();

        single_ms = (double)(t1 - t0) / (double)CLOCKS_PER_SEC * 1000.0;
        printf("Calibration: 1 legacy evolve = %.1f ms  (chunks=%zu)\n\n",
               single_ms, report.chunk_count);

        library_report_free(&report);
        registry_free(&reg);
    }

    /* Choose R so total is roughly >= 200 ms; minimum 3 runs */
    /* We'll time 5 runs each (enough for a meaningful delta at the ms level) */
    const int R = 5;

    /* --- time legacy library_evolve --- */
    {
        clock_t t0, t1;
        size_t total_chunks = 0;
        int r;

        t0 = clock();
        for (r = 0; r < R; ++r) {
            PrimitiveRegistry reg;
            LibraryReport report;
            /* Reset evidence each iteration so behavior is identical */
            gate_set_ev(&dec_proto, 16, 0);
            gate_set_ev(&inc_proto, 16, 0);
            registry_init(&reg);
            registry_add(&reg, &dec_proto, "dec");
            registry_add(&reg, &inc_proto, "inc");
            library_evolve(&reg, &task, 1, NULL, 0, NULL, 4, &report);
            total_chunks += report.chunk_count;
            library_report_free(&report);
            registry_free(&reg);
        }
        t1 = clock();

        double legacy_total_ms = (double)(t1 - t0) / (double)CLOCKS_PER_SEC * 1000.0;
        double legacy_per_ms   = legacy_total_ms / (double)R;
        size_t avg_chunks      = total_chunks / (size_t)R;

        printf("EVIDENCE_CLEAR gate overhead (dec->inc route, R=%d runs):\n", R);
        printf("  legacy library_evolve  : %7.3f ms/run  (chunks=%zu)\n",
               legacy_per_ms, avg_chunks);

        /* --- time gated library_evolve_gated (enabled, seeded) --- */
        {
            clock_t tg0, tg1;
            size_t gated_chunks = 0;

            tg0 = clock();
            for (r = 0; r < R; ++r) {
                PrimitiveRegistry reg;
                LibraryReport report;
                gate_set_ev(&dec_proto, 16, 0);
                gate_set_ev(&inc_proto, 16, 0);
                registry_init(&reg);
                registry_add(&reg, &dec_proto, "dec");
                registry_add(&reg, &inc_proto, "inc");
                library_evolve_gated(&reg, &task, 1, NULL, 0, NULL, &gconf, 4, &report);
                gated_chunks += report.chunk_count;
                library_report_free(&report);
                registry_free(&reg);
            }
            tg1 = clock();

            double gated_total_ms = (double)(tg1 - tg0) / (double)CLOCKS_PER_SEC * 1000.0;
            double gated_per_ms   = gated_total_ms / (double)R;
            size_t gavg_chunks    = gated_chunks / (size_t)R;

            printf("  gated  (enabled,seeded): %7.3f ms/run  (chunks=%zu)\n",
                   gated_per_ms, gavg_chunks);
            printf("  gate overhead          : %+7.3f ms/run\n",
                   gated_per_ms - legacy_per_ms);

            if (avg_chunks != gavg_chunks) {
                printf("  WARNING: chunk_count mismatch! legacy=%zu gated=%zu\n",
                       avg_chunks, gavg_chunks);
            } else {
                printf("  chunk_count match: PASS (both=%zu)\n", avg_chunks);
            }
        }
    }

    btn_free(&dec_proto);
    btn_free(&inc_proto);

    /* ================================================================
     * PART 2: structural_canonical_digest sweep cost
     * Build the lure registry (two untrained alt_a/alt_b producers).
     * Call structural_canonical_digest N times; report total + ms/call.
     * ================================================================ */
    printf("\n");

    {
        BinaryTransformNetwork alt_a = {0}, alt_b = {0};
        PrimitiveRegistry reg;
        Port goal = PTG(PORT_ONEHOT, 4, 1);
        Port src_t = PTG(PORT_ONEHOT, 4, 1);
        double sv[4] = {1, 0, 0, 0};
        DagSource src;
        uint64_t d = 0;
        clock_t t0, t1;
        const int N = 200;
        int n;

        gate_build_prod(&alt_a, "gl");
        gate_build_prod(&alt_b, "gl");
        port_set_tag(&goal, "gl");
        port_set_tag(&src_t, "x");
        src.type = src_t;
        src.values = sv;

        registry_init(&reg);
        registry_add(&reg, &alt_a, "alt_a");
        registry_add(&reg, &alt_b, "alt_b");

        /* warmup */
        d = structural_canonical_digest(&reg, &src, 1, goal);

        t0 = clock();
        for (n = 0; n < N; ++n) {
            d = structural_canonical_digest(&reg, &src, 1, goal);
        }
        t1 = clock();
        (void)d;

        double total_ms  = (double)(t1 - t0) / (double)CLOCKS_PER_SEC * 1000.0;
        double per_call  = total_ms / (double)N;

        printf("structural_canonical_digest sweep cost (lure, N=%d calls):\n", N);
        printf("  total: %.2f ms over N=%d   ->  %.4f ms/call\n",
               total_ms, N, per_call);

        registry_free(&reg);
        btn_free(&alt_a);
        btn_free(&alt_b);
    }

    return 0;
}
