/* P3 Miner efficiency A/B — hermetic nibble-increment fixture.
 * Compares baseline acquire vs CNET_TRAIN_FAST=1 on the same seed/domain.
 * Reports oracle_calls, wall_ms, closed, student_bytes; asserts correctness
 * invariant and non-regression on teacher_efficiency or latency.
 * make miner_efficiency_bench → MINER_EFFICIENCY_BENCH_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../include/acquire.h"
#include "../include/contract/unit.h"
#include "../include/nn.h"
#include "../include/router.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static Port make_port(PortFamily fam, size_t w, size_t c, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = fam;
    p.field_width = w;
    p.field_count = c;
    if (port_set_tag(&p, tag) != 0) {
        fprintf(stderr, "bad tag %s\n", tag);
        exit(1);
    }
    return p;
}

static void nibble_bits(unsigned v, double *out) {
    out[0] = (v >> 3) & 1u;
    out[1] = (v >> 2) & 1u;
    out[2] = (v >> 1) & 1u;
    out[3] = v & 1u;
}

static unsigned bits_nibble(const double *in) {
    return (unsigned)(((in[0] > 0.5) << 3) | ((in[1] > 0.5) << 2) |
                      ((in[2] > 0.5) << 1) | (in[3] > 0.5));
}

static int oracle_increment(const double *in, double *out, void *ctx) {
    (void)ctx;
    nibble_bits((bits_nibble(in) + 1u) & 0xFu, out);
    return 0;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
    size_t oracle_calls;
    size_t oracle_rejects;
    size_t closed;
    size_t deferred;
    double wall_ms;
    long student_bytes;
    int semantic_ok;
    double teacher_efficiency;
} ArmResult;

static long file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    n = ftell(f);
    fclose(f);
    return n;
}

static int domain_semantic_ok(PrimitiveRegistry *reg, Port in, Port goal) {
    RoutePlan plan;
    double x[4], y[4];
    unsigned v;
    memset(&plan, 0, sizeof plan);
    if (route_plan(reg, in, goal, &plan) != 0 || plan.length == 0) return 0;
    for (v = 0; v < 16; ++v) {
        nibble_bits(v, x);
        memset(y, 0, sizeof y);
        if (route_execute(&plan, x, 4, y, 4) != 0) return 0;
        if (bits_nibble(y) != ((v + 1u) & 0xFu)) return 0;
    }
    return 1;
}

static ArmResult run_arm(const char *label, const char *unit_dir, int train_fast) {
    ArmResult ar;
    PrimitiveRegistry reg;
    AcquireLedger led;
    OracleRegistry orc;
    AcquireConfig cfg;
    AcquireReport rep;
    Port nib = make_port(PORT_BINARY_MSB, 4, 1, "me_nibble");
    Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "me_nibble_next");
    OracleEntry *oe;
    double t0;
    char path[256];
    char cmd[320];

    memset(&ar, 0, sizeof ar);
    registry_init(&reg);
    acquire_ledger_init(&led);
    memset(&orc, 0, sizeof orc);
    acquire_config_defaults(&cfg);
    cfg.unit_dir = unit_dir;
    cfg.min_evidence = 1;
    cfg.evidence_threshold = 0.5;
    /* Keep student small for speed; still certify-able on 16-point domain. */
    cfg.init_hidden = 8;
    cfg.max_hidden = 64;
    cfg.max_epochs = 8000;
    cfg.sample_count = 16;

    if (train_fast)
        setenv("CNET_TRAIN_FAST", "1", 1);
    else
        setenv("CNET_TRAIN_FAST", "0", 1);

    snprintf(cmd, sizeof cmd, "rm -rf %s && mkdir -p %s", unit_dir, unit_dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "mkdir failed for %s\n", unit_dir);
    }

    check(acquire_oracle_register(&orc, "me_inc", nib, nibn, oracle_increment, NULL) == 0,
          train_fast ? "arm FAST: oracle register" : "arm BASE: oracle register");
    acquire_note_no_plan(&led, nib, nibn);

    t0 = now_ms();
    memset(&rep, 0, sizeof rep);
    check(acquire_drain(&reg, &led, &orc, &cfg, &rep) == 0,
          train_fast ? "arm FAST: drain" : "arm BASE: drain");
    ar.wall_ms = now_ms() - t0;
    ar.closed = rep.closed;
    ar.deferred = rep.deferred;

    oe = &orc.entries[0];
    ar.oracle_calls = oe->calls;
    ar.oracle_rejects = oe->rejects;
    ar.teacher_efficiency =
        ar.oracle_calls ? (double)ar.closed / (double)ar.oracle_calls : 0.0;
    ar.semantic_ok = (ar.closed >= 1) && domain_semantic_ok(&reg, nib, nibn);

    /* Find sealed unit file if any. */
    snprintf(path, sizeof path, "%s/acq_me_nibble_next.cnu", unit_dir);
    ar.student_bytes = file_size(path);
    if (ar.student_bytes < 0) {
        /* name may vary; scan dir for any .cnu */
        FILE *p;
        char line[512];
        snprintf(cmd, sizeof cmd, "ls %s/*.cnu 2>/dev/null | head -1", unit_dir);
        p = popen(cmd, "r");
        if (p && fgets(line, sizeof line, p)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            ar.student_bytes = file_size(line);
        }
        if (p) pclose(p);
    }

    printf("  [%s] closed=%zu deferred=%zu oracle_calls=%zu rejects=%zu "
           "wall_ms=%.1f student_bytes=%ld teff=%.4f semantic=%d\n",
           label, ar.closed, ar.deferred, ar.oracle_calls, ar.oracle_rejects,
           ar.wall_ms, ar.student_bytes, ar.teacher_efficiency, ar.semantic_ok);

    acquire_ledger_free(&led);
    registry_free(&reg);
    return ar;
}

int main(void) {
    ArmResult base, fast;
    int improved;

    printf("== miner_efficiency_bench (P3) ==\n");
    base = run_arm("BASE", "tmp_miner_eff_base", 0);
    fast = run_arm("FAST", "tmp_miner_eff_fast", 1);

    check(base.closed >= 1, "baseline certifies >= 1 unit");
    check(base.semantic_ok, "baseline full-domain semantic correctness");
    check(fast.closed >= 1, "train_fast arm certifies >= 1 unit");
    check(fast.semantic_ok, "train_fast full-domain semantic correctness");
    check(base.oracle_calls > 0 && fast.oracle_calls > 0, "oracle_calls tracked");

    /* Improvement invariant: at least one of efficiency or latency improves,
       neither may collapse semantic correctness (already checked). Allow 5%
       wall-clock noise on latency comparison. */
    improved = 0;
    if (fast.teacher_efficiency + 1e-12 >= base.teacher_efficiency) improved = 1;
    if (fast.wall_ms <= base.wall_ms * 1.05) improved = 1;
    check(improved,
          "FAST arm improves teacher_efficiency or wall_ms (tol 5%) vs BASE");

    /* Cleanup */
    (void)system("rm -rf tmp_miner_eff_base tmp_miner_eff_fast");
    unsetenv("CNET_TRAIN_FAST");

    if (failures) {
        printf("MINER_EFFICIENCY_BENCH_FAIL failures=%d checks=%d\n", failures, checks);
        return 1;
    }
    printf("MINER_EFFICIENCY_BENCH_PASS checks=%d base_teff=%.4f fast_teff=%.4f "
           "base_ms=%.1f fast_ms=%.1f\n",
           checks, base.teacher_efficiency, fast.teacher_efficiency, base.wall_ms,
           fast.wall_ms);
    return 0;
}
