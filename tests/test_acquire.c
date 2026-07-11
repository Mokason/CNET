/* Gap-triggered acquisition loop (v1) — vertical slice gate.
   Fixture: 4-bit increment (BINARY_MSB w4 "nibble" -> BINARY_MSB w4
   "nibble_next"), withheld from the registry; its reference implementation
   is the oracle. Sections are numbered; first failure exits non-zero. */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/acquire.h"
#include "../include/contract/unit.h"

static int checks_run = 0;

static void check(int cond, const char *what) {
    ++checks_run;
    if (!cond) {
        printf("FAIL: %s\n", what);
        exit(1);
    }
    printf("  ok: %s\n", what);
}

/* ---- fixture: ports ---------------------------------------------------- */

static Port make_port(PortFamily fam, size_t w, size_t c, const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = fam;
    p.field_width = w;
    p.field_count = c;
    if (port_set_tag(&p, tag) != 0) { printf("FAIL: bad tag %s\n", tag); exit(1); }
    return p;
}

/* nibble value v (0..15) -> 4 MSB-first bits into out[4] */
static void nibble_bits(unsigned v, double *out) {
    out[0] = (v >> 3) & 1u; out[1] = (v >> 2) & 1u;
    out[2] = (v >> 1) & 1u; out[3] = v & 1u;
}

static unsigned bits_nibble(const double *in) {
    return (unsigned)(((in[0] > 0.5) << 3) | ((in[1] > 0.5) << 2) |
                      ((in[2] > 0.5) << 1) | (in[3] > 0.5));
}

/* ---- fixture: oracles -------------------------------------------------- */

/* Reference implementation of increment: (v + 1) & 0xF, MSB bits. */
static int oracle_increment(const double *in, double *out, void *ctx) {
    (void)ctx;
    nibble_bits((bits_nibble(in) + 1u) & 0xFu, out);
    return 0;
}

/* Broken oracle: emits ambiguous garbage (fails port_validate). */
static int oracle_broken(const double *in, double *out, void *ctx) {
    (void)in; (void)ctx;
    out[0] = 0.5; out[1] = 0.5; out[2] = 0.5; out[3] = 0.5;
    return 0;
}

/* Train the hex_value companion: ONEHOT16 "hex_sym" -> BINARY_MSB4 "nibble".
   Returns a heap BTN certified against a contract built from its own
   exemplar table (caller owns btn). */
static BinaryTransformNetwork *make_hex_value_certified(PrimitiveRegistry *reg) {
    static double inputs[16 * 16];
    static double targets[16 * 4];
    BinaryTransformNetwork *btn = calloc(1, sizeof *btn);
    Contract c;
    Port hex = make_port(PORT_ONEHOT, 16, 1, "hex_sym");
    Port nib = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
    unsigned v;

    memset(inputs, 0, sizeof inputs);
    for (v = 0; v < 16; ++v) {
        inputs[v * 16 + v] = 1.0;
        nibble_bits(v, targets + v * 4);
    }
    if (!btn || btn_init(btn, 16, 4, 8, 64, 0.5, 7) != 0 ||
        btn_set_ports(btn, hex, nib) != 0) { free(btn); return NULL; }
    btn_train_dynamic(btn, inputs, targets, 16, 4000, 200, 1e-4, 1e-6);
    if (contract_init_borrowed(&c, "hex_value", btn, inputs, targets, 16) != 0 ||
        registry_add_certified(reg, btn, "hex_value", &c) != 0) {
        btn_free(btn); free(btn); return NULL;
    }
    return btn;
}

int main(void) {
    printf("== acquire: gap-triggered acquisition loop ==\n");

    printf("[1] ledger basics\n");
    {
        AcquireLedger led;
        OracleRegistry orc;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        int idx, idx2;

        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);

        check(acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                      oracle_increment, NULL) == 0,
              "oracle registers");
        check(acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                      oracle_increment, NULL) == -1,
              "duplicate oracle name refused");
        check(acquire_oracle_register(&orc, "bad name!", nib, nibn,
                                      oracle_increment, NULL) == -1,
              "non-atom oracle name refused");

        idx = acquire_note_no_plan(&led, nib, nibn);
        check(idx == 0 && led.count == 1, "no_plan gap recorded");
        check(led.gaps[0].kind == GAP_NO_PLAN && led.gaps[0].status == GAP_OPEN,
              "gap is OPEN NO_PLAN");
        check(led.gaps[0].times_hit == 1, "times_hit starts at 1");

        idx2 = acquire_note_no_plan(&led, nib, nibn);
        check(idx2 == 0 && led.count == 1 && led.gaps[0].times_hit == 2,
              "same signature coalesces (times_hit 2, no duplicate)");

        check(acquire_note_low_reliability(&led, "increment", 0.14, 0.5) == 1 &&
              led.count == 2 && led.gaps[1].kind == GAP_LOW_RELIABILITY &&
              strcmp(led.gaps[1].subject, "increment") == 0,
              "low_reliability gap recorded with subject");

        check(acquire_note_health(&led, "increment", "resource_anomaly") == 1 &&
              led.count == 2 && led.gaps[1].times_hit == 2,
              "health on same subject coalesces onto the rebuild record");

        check(acquire_note_health(&led, "hex_value", "resource_anomaly") == 2 &&
              led.count == 3,
              "health on a different subject is a new record");

        acquire_ledger_free(&led);
        check(led.count == 0 && led.gaps == NULL, "free resets the ledger");
    }

    printf("[2] sidecar round-trip\n");
    {
        AcquireLedger led, led2;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        const char *path = "acquire_gaps_test.txt";
        FILE *f;

        acquire_ledger_init(&led);
        acquire_note_no_plan(&led, nib, nibn);
        acquire_note_no_plan(&led, nib, nibn);           /* times_hit 2 */
        acquire_note_health(&led, "increment", "resource_anomaly");
        led.gaps[1].status = GAP_DEFERRED;
        snprintf(led.gaps[1].defer_reason, ACQUIRE_REASON_MAX, "oracle_unfit");

        check(acquire_ledger_save(&led, path) == 0, "ledger saves");

        acquire_ledger_init(&led2);
        check(acquire_ledger_load(&led2, path) == 0, "ledger loads");
        check(led2.count == 2, "record count survives");
        check(led2.gaps[0].kind == GAP_NO_PLAN && led2.gaps[0].times_hit == 2,
              "NO_PLAN record survives with counters");
        check(acquire_port_eq_public(led2.gaps[0].goal_port, nibn),
              "port signature + tag survive");
        check(led2.gaps[1].status == GAP_DEFERRED &&
              strcmp(led2.gaps[1].defer_reason, "oracle_unfit") == 0 &&
              strcmp(led2.gaps[1].subject, "increment") == 0,
              "DEFERRED status + reason + subject survive");

        /* malformed -> -1 with the ledger untouched */
        f = fopen(path, "w");
        fprintf(f, "CNET_GAPS 1\nnot_a_count\n");
        fclose(f);
        check(acquire_ledger_load(&led2, path) == -1 && led2.count == 2,
              "malformed file refused, ledger untouched");

        /* wrong magic -> -1 */
        f = fopen(path, "w");
        fprintf(f, "CNET_STATS 1\n3 4\n");
        fclose(f);
        check(acquire_ledger_load(&led2, path) == -1, "wrong magic refused");

        acquire_ledger_free(&led);
        acquire_ledger_free(&led2);
        remove(path);
    }

    printf("[3] oracle fallback + capture\n");
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        double in[4], out[4];
        unsigned v;
        int rc;

        registry_init(&reg);              /* EMPTY: no plan can exist */
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);

        /* no plan + no oracle -> -1, gap still recorded */
        nibble_bits(5, in);
        rc = acquire_execute_or_fallback(&reg, &led, &orc, &cfg,
                                         nib, nibn, in, 4, out, 4);
        check(rc == -1 && led.count == 1 && led.gaps[0].kind == GAP_NO_PLAN,
              "no plan + no oracle: error, gap recorded");

        /* with the oracle: answered + captured */
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);
        for (v = 0; v < 6; ++v) {
            nibble_bits(v, in);
            rc = acquire_execute_or_fallback(&reg, &led, &orc, &cfg,
                                             nib, nibn, in, 4, out, 4);
            check(rc == 0, "fallback answers");
            check(bits_nibble(out) == ((v + 1u) & 0xFu), "fallback answer correct");
        }
        check(led.count == 1, "fallback coalesces onto the same gap");
        check(led.gaps[0].cap_count == 6, "six exemplars captured");
        check(strcmp(led.gaps[0].oracle, "increment_ref") == 0,
              "gap remembers which oracle answered");
        /* captured rows are canonical pairs */
        check(bits_nibble(led.gaps[0].cap_inputs + 0 * 4) == 0 &&
              bits_nibble(led.gaps[0].cap_targets + 0 * 4) == 1,
              "captured pair is (input, oracle target)");

        acquire_ledger_free(&led);
        registry_free(&reg);
    }

    printf("[4] drain: NO_PLAN acquisition (headline)\n");
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        RoutePlan plan;
        double in[4], out[4];
        unsigned v;
        char cnu_path[256];

        registry_init(&reg);              /* increment WITHHELD */
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        cfg.unit_dir = ".";               /* seal into cwd; cleaned below */
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);

        check(route_plan(&reg, nib, nibn, &plan) == -1,
              "planner genuinely fails before acquisition");
        acquire_note_no_plan(&led, nib, nibn);

        memset(&rep, 0, sizeof rep);
        {
            clock_t t0 = clock(), t1;
            check(acquire_drain(&reg, &led, &orc, &cfg, &rep) == 0, "drain runs");
            t1 = clock();
            printf("  [bench] drain (mine+train+certify+seal+register+replan): %.1f ms, "
                   "verdict=%d, unit=%s\n",
                   1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC,
                   (int)rep.last_verdict, rep.last_unit_name);
        }
        check(rep.examined == 1 && rep.closed == 1 && rep.deferred == 0,
              "drain closed the gap");
        check(led.gaps[0].status == GAP_CLOSED, "gap is CLOSED");
        check(rep.last_verdict == CERT_PROVEN,
              "16-point domain fully enumerated -> PROOF");
        check(reg.count == 1 && reg.entries[0].certified == 1 &&
              reg.entries[0].state == PRIM_FROZEN,
              "acquired unit registered certified FROZEN");

        /* the router now plans through it */
        check(route_plan(&reg, nib, nibn, &plan) == 0 && plan.length == 1,
              "replan finds the acquired unit");
        plan.strict = 1;
        for (v = 0; v < 16; ++v) {
            nibble_bits(v, in);
            check(route_execute(&plan, in, 4, out, 4) == 0 &&
                  bits_nibble(out) == ((v + 1u) & 0xFu),
                  "strict execution correct");
        }

        /* sealed unit round-trips (seal verified) */
        snprintf(cnu_path, sizeof cnu_path, "./%s.cnu", rep.last_unit_name);
        {
            BinaryTransformNetwork rbtn;
            Contract rc;
            check(unit_load(&rbtn, &rc, cnu_path) == 0 && rc.seal_verified == 1,
                  ".cnu exists and its seal verifies");
            check(btn_certify(&rbtn, &rc, NULL) == 0,
                  "reloaded unit still certifies its own contract");
            btn_free(&rbtn);
            contract_free(&rc);
        }
        remove(cnu_path);

        acquire_ledger_free(&led);       /* frees the acquired BTN */
        registry_free(&reg);
    }

    printf("[5] acquire_now (inline mode)\n");
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        RoutePlan plan;

        registry_init(&reg);
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);      /* unit_dir NULL: no sealing here */
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);

        memset(&rep, 0, sizeof rep);
        check(acquire_now(&reg, &led, &orc, &cfg, nib, nibn, &rep) == 0,
              "acquire_now closes the gap in one call");
        check(route_plan(&reg, nib, nibn, &plan) == 0,
              "plan exists after acquire_now");
        check(led.count == 1 && led.gaps[0].status == GAP_CLOSED,
              "acquire_now noted + closed its own gap");

        acquire_ledger_free(&led);
        registry_free(&reg);
    }

    printf("[6] composition: acquired + frozen unit, certified end-to-end\n");
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port hex  = make_port(PORT_ONEHOT, 16, 1, "hex_sym");
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        BinaryTransformNetwork *hexv;
        RoutePlan plan;
        double in[16], out[4];
        unsigned v;

        registry_init(&reg);
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);

        hexv = make_hex_value_certified(&reg);
        check(hexv != NULL, "hex_value companion trains + certifies");

        /* the 2-hop task has NO plan: the nibble->nibble_next link is missing */
        check(route_plan(&reg, hex, nibn, &plan) == -1,
              "2-hop task unplannable before acquisition");

        /* acquire the MISSING LINK (not the whole task) */
        memset(&rep, 0, sizeof rep);
        check(acquire_now(&reg, &led, &orc, &cfg, nib, nibn, &rep) == 0,
              "missing link acquired");

        /* the router composes frozen hex_value with the acquired unit,
           certified end-to-end */
        reg.require_certified = 1;
        check(route_plan(&reg, hex, nibn, &plan) == 0 && plan.length == 2,
              "router discovers the 2-hop certified composition");
        plan.strict = 1;
        for (v = 0; v < 16; ++v) {
            memset(in, 0, sizeof in);
            in[v] = 1.0;
            check(route_execute(&plan, in, 16, out, 4) == 0 &&
                  bits_nibble(out) == ((v + 1u) & 0xFu),
                  "composed strict execution correct");
        }
        reg.require_certified = 0;

        acquire_ledger_free(&led);
        registry_free(&reg);
        btn_free(hexv); free(hexv);
    }

    printf("[7] DEFER totality + counter hygiene (broken oracle)\n");
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        BinaryTransformNetwork *hexv;
        unsigned long s_before, f_before;
        size_t count_before;
        FILE *probe;

        registry_init(&reg);
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        cfg.unit_dir = ".";

        /* a bystander certified unit whose counters must not move */
        hexv = make_hex_value_certified(&reg);
        check(hexv != NULL, "bystander unit present");
        hexv->output_successes = 7;   /* nonzero so 'unchanged' is meaningful */
        hexv->output_failures = 3;
        s_before = hexv->output_successes;
        f_before = hexv->output_failures;
        count_before = reg.count;

        acquire_oracle_register(&orc, "broken_ref", nib, nibn,
                                oracle_broken, NULL);
        acquire_note_no_plan(&led, nib, nibn);

        memset(&rep, 0, sizeof rep);
        check(acquire_drain(&reg, &led, &orc, &cfg, &rep) == 0, "drain runs");
        check(rep.deferred == 1 && rep.closed == 0, "drain deferred");
        check(led.gaps[0].status == GAP_DEFERRED &&
              strcmp(led.gaps[0].defer_reason, "oracle_unfit") == 0,
              "DEFERRED with reason oracle_unfit");

        /* DEFER totality */
        check(reg.count == count_before, "registry count unchanged");
        check(hexv->output_successes == s_before &&
              hexv->output_failures == f_before,
              "bystander reliability counters byte-identical (Delta-4)");
        probe = fopen("./acq_nibble_next.cnu", "r");
        check(probe == NULL, "no .cnu leaked");
        if (probe) fclose(probe);

        /* a re-hit reopens the DEFERRED gap for a future retry */
        acquire_note_no_plan(&led, nib, nibn);
        check(led.gaps[0].status == GAP_OPEN &&
              led.gaps[0].defer_reason[0] == '\0',
              "re-hit reopens a DEFERRED gap");

        acquire_ledger_free(&led);
        registry_free(&reg);
        btn_free(hexv); free(hexv);
    }

    printf("[8] rebuild: LOW_RELIABILITY + HEALTH\n");
    {
        PrimitiveRegistry reg;
        AcquireLedger led;
        OracleRegistry orc;
        AcquireConfig cfg;
        AcquireReport rep;
        Port nib  = make_port(PORT_BINARY_MSB, 4, 1, "nibble");
        Port nibn = make_port(PORT_BINARY_MSB, 4, 1, "nibble_next");
        static double inc_inputs[16 * 4], inc_targets[16 * 4];
        BinaryTransformNetwork *inc = calloc(1, sizeof *inc);
        Contract c;
        RoutePlan plan;
        double in[4], out[4];
        unsigned v;

        registry_init(&reg);
        reg.lifecycle_enabled = 1;        /* RESET-skip active */
        acquire_ledger_init(&led);
        memset(&orc, 0, sizeof orc);
        acquire_config_defaults(&cfg);
        acquire_oracle_register(&orc, "increment_ref", nib, nibn,
                                oracle_increment, NULL);

        /* a real certified increment */
        for (v = 0; v < 16; ++v) {
            nibble_bits(v, inc_inputs + v * 4);
            nibble_bits((v + 1u) & 0xFu, inc_targets + v * 4);
        }
        check(inc && btn_init(inc, 4, 4, 8, 64, 0.5, 7) == 0 &&
              btn_set_ports(inc, nib, nibn) == 0, "incumbent inits");
        btn_train_dynamic(inc, inc_inputs, inc_targets, 16, 4000, 200,
                          1e-4, 1e-6);
        check(contract_init_borrowed(&c, "increment", inc,
                                     inc_inputs, inc_targets, 16) == 0 &&
              registry_add_certified(&reg, inc, "increment", &c) == 0,
              "incumbent certifies + registers");

        /* Case A: healthy incumbent, poisoned counters -> incumbent_healthy */
        inc->output_successes = 0;
        inc->output_failures = 50;         /* rel ~ 0.02: 'unreliable' */
        acquire_note_low_reliability(&led, "increment", 0.02, 0.5);
        memset(&rep, 0, sizeof rep);
        acquire_drain(&reg, &led, &orc, &cfg, &rep);
        check(led.gaps[0].status == GAP_DEFERRED &&
              strcmp(led.gaps[0].defer_reason, "incumbent_healthy") == 0,
              "healthy incumbent is not churned (DEFER incumbent_healthy)");
        check(reg.entries[0].state == PRIM_FROZEN, "incumbent stays FROZEN");

        /* Case B: actually-broken incumbent (HEALTH trigger) -> rebuild */
        {   /* corrupt: flip the sign of every output-layer weight */
            size_t k, n = inc->hidden_count * 4;
            for (k = 0; k < n; ++k)
                inc->hidden_output_weights[k] = -inc->hidden_output_weights[k];
        }
        acquire_note_health(&led, "increment", "resource_anomaly");
        check(led.gaps[0].status == GAP_OPEN, "re-hit reopened the gap");
        memset(&rep, 0, sizeof rep);
        acquire_drain(&reg, &led, &orc, &cfg, &rep);
        check(rep.closed == 1 && led.gaps[0].status == GAP_CLOSED,
              "broken incumbent rebuilt");
        check(reg.entries[0].state == PRIM_RESET,
              "incumbent demoted to RESET");
        check(reg.count == 2 && reg.entries[1].certified == 1 &&
              strcmp(reg.entries[1].name, "increment_r2") == 0,
              "replacement registered certified under a fresh name");
        check(reg.entries[1].btn->output_successes == 0 &&
              reg.entries[1].btn->output_failures == 0,
              "replacement starts with fresh counters (no fabricated evidence)");

        /* the router routes around the RESET incumbent */
        check(route_plan(&reg, nib, nibn, &plan) == 0 && plan.length == 1,
              "replan succeeds");
        plan.strict = 1;
        for (v = 0; v < 16; ++v) {
            nibble_bits(v, in);
            check(route_execute(&plan, in, 4, out, 4) == 0 &&
                  bits_nibble(out) == ((v + 1u) & 0xFu),
                  "rebuilt behavior correct");
        }

        acquire_ledger_free(&led);
        registry_free(&reg);
        btn_free(inc); free(inc);
    }

    printf("checks run: %d\n", checks_run);
    printf("ALL ACQUIRE TESTS PASSED\n");
    return 0;
}
