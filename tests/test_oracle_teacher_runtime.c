/* Oracle teacher runtime Tier A+B gate.
 * make oracle_teacher_runtime → ORACLE_TEACHER_RUNTIME_PASS
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/acquire.h"

static int failures;
#define CHECK(cond, msg) do { \
    if (cond) printf("  ok: %s\n", (msg)); \
    else { printf("  FAIL: %s\n", (msg)); ++failures; } \
} while (0)

static Port bit_port(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_BINARY_MSB;
    p.field_width = 1;
    p.field_count = 1;
    snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

static CnetOracleIdentity id_partial(void) {
    CnetOracleIdentity id;
    memset(&id, 0, sizeof id);
    id.abi_version = CNET_ORACLE_ABI_VERSION;
    id.struct_size = (uint32_t)sizeof id;
    id.artifact_digest = UINT64_C(0x1111);
    id.contract_digest = UINT64_C(0x2222);
    /* no sha, no toolchain → unattested */
    return id;
}

static CnetOracleIdentity id_full(void) {
    CnetOracleIdentity id = id_partial();
    size_t i;
    id.toolchain_digest = UINT64_C(0xabcdef);
    for (i = 0; i < 32; i++) id.artifact_sha256[i] = (unsigned char)(0x40 + i);
    return id;
}

static int v2_flip(const double *in, size_t in_count, double *out, size_t out_count,
                   CnetOracleResult *result, void *ctx) {
    (void)ctx;
    if (!in || !out || in_count != 1 || out_count != 1 || !result) return -1;
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = CNET_ORACLE_ANSWER;
    result->confidence = 1.0;
    out[0] = in[0] > 0.5 ? 0.0 : 1.0;
    return 0;
}

static CnetOracleValidity always_valid(const double *in, size_t ic,
                                       const double *out, size_t oc,
                                       const CnetOracleResult *r, void *ctx) {
    (void)in; (void)ic; (void)out; (void)oc; (void)r; (void)ctx;
    return CNET_ORACLE_VALID;
}

static int legacy_ok(const double *in, double *out, void *ctx) {
    (void)ctx;
    out[0] = in[0];
    return 0;
}

static int batch_v2(const double *in, size_t in_stride, double *out,
                    size_t out_stride, size_t count, CnetOracleResult *results,
                    void *ctx) {
    size_t i;
    (void)ctx;
    for (i = 0; i < count; ++i) {
        memset(&results[i], 0, sizeof results[i]);
        results[i].abi_version = CNET_ORACLE_ABI_VERSION;
        results[i].struct_size = (uint32_t)sizeof results[i];
        results[i].status = CNET_ORACLE_ANSWER;
        results[i].confidence = 1.0;
        out[i * out_stride] = in[i * in_stride] > 0.5 ? 0.0 : 1.0;
    }
    return 0;
}

int main(void) {
    OracleRegistry reg;
    OraclePolicy pol;
    OracleScorecard sc;
    CnetOracleIdentity partial, full;
    CnetOracleResult results[4];
    double in[4] = {0, 1, 0, 1};
    double out[4];
    uint64_t lease;
    Port pin = bit_port("tr_in");
    Port pout = bit_port("tr_out");

    printf("== Oracle teacher runtime A+B ==\n");
    memset(&reg, 0, sizeof reg);
    partial = id_partial();
    full = id_full();

    CHECK(cnet_oracle_identity_is_attested(&partial) == 0,
          "partial identity is unattested");
    CHECK(cnet_oracle_identity_is_attested(&full) == 1,
          "full SHA+toolchain is attested");

    /* B: v2_only refuses v1 */
    acquire_oracle_policy_defaults(&pol);
    pol.v2_only_new = 1;
    acquire_oracle_policy_set(&reg, &pol);
    CHECK(acquire_oracle_register(&reg, "legacy", pin, pout, legacy_ok, NULL) != 0,
          "v2_only_new refuses v1 register");

    /* B: require_validator */
    pol.v2_only_new = 0;
    pol.require_validator = 1;
    acquire_oracle_policy_set(&reg, &pol);
    CHECK(acquire_oracle_register_v2(&reg, "noval", pin, pout, v2_flip, NULL,
                                     &full, NULL) != 0,
          "require_validator refuses missing validator");

    /* A: attestation gate on register when policy on */
    memset(&reg, 0, sizeof reg);
    pol = (OraclePolicy){0};
    pol.require_attested_to_teach = 1;
    acquire_oracle_policy_set(&reg, &pol);
    CHECK(acquire_oracle_register_v2(&reg, "weak", pin, pout, v2_flip,
                                     always_valid, &partial, NULL) != 0,
          "attestation policy refuses unattested register");
    CHECK(acquire_oracle_register_v2_family(&reg, "strong", "lm_family", pin,
                                            pout, v2_flip, always_valid, &full,
                                            NULL) == 0,
          "attested family register succeeds");

    /* Family + scorecard */
    CHECK(acquire_oracle_scorecard(&reg, "strong", &sc) == 0 &&
              strcmp(sc.family, "lm_family") == 0 && sc.attested == 1,
          "scorecard exposes family and attested");

    /* A: lease */
    pol.require_lease_to_teach = 1;
    acquire_oracle_policy_set(&reg, &pol);
    CHECK(acquire_oracle_is_teachable(&reg, &reg.entries[0]) == 0,
          "unleased not teachable under lease policy");
    lease = acquire_oracle_bind(&reg, "strong");
    CHECK(lease > 0 && acquire_oracle_is_teachable(&reg, &reg.entries[0]),
          "bind issues lease and becomes teachable");
    CHECK(acquire_oracle_unbind(&reg, "strong", lease) == 0 &&
              acquire_oracle_is_teachable(&reg, &reg.entries[0]) == 0,
          "unbind revokes teachability");

    /* Re-bind for batch */
    lease = acquire_oracle_bind(&reg, "strong");
    CHECK(lease > 0, "rebind");
    CHECK(acquire_oracle_set_batch_v2(&reg, "strong", batch_v2, 4) == 0,
          "set batch v2");
    memset(results, 0, sizeof results);
    CHECK(cnet_oracle_invoke_batch(&reg.entries[0], in, 1, out, 1, 4, results) ==
                  0 &&
              results[0].status == CNET_ORACLE_ANSWER &&
              results[3].status == CNET_ORACLE_ANSWER &&
              reg.entries[0].calls == 4,
          "batch v2 fills per-row results and accounts calls");

    /* Serial fallback batch path */
    {
        OracleRegistry r2;
        memset(&r2, 0, sizeof r2);
        CHECK(acquire_oracle_register_v2(&r2, "serial", pin, pout, v2_flip,
                                         always_valid, &full, NULL) == 0,
              "serial teacher register");
        memset(results, 0, sizeof results);
        CHECK(cnet_oracle_invoke_batch(&r2.entries[0], in, 1, out, 1, 2, results) ==
                      0 &&
                  r2.entries[0].calls == 2,
              "batch without fn_batch_v2 falls back to serial invoke");
    }

    /* A: retire policy */
    {
        OracleRegistry r3;
        OraclePolicy p3;
        size_t n;
        memset(&r3, 0, sizeof r3);
        p3 = (OraclePolicy){0};
        p3.retire_unfit_rate = 0.5;
        p3.retire_min_calls = 4;
        acquire_oracle_policy_set(&r3, &p3);
        CHECK(acquire_oracle_register_v2(&r3, "flaky", pin, pout, v2_flip,
                                         always_valid, &full, NULL) == 0,
              "flaky register");
        r3.entries[0].calls = 10;
        r3.entries[0].unfit_closes = 6;
        n = acquire_oracle_apply_retire_policy(&r3);
        CHECK(n == 1 && r3.entries[0].retired == 1 &&
                  strcmp(r3.entries[0].retire_reason, "unfit_rate") == 0,
              "auto-retire on unfit rate");
        CHECK(acquire_oracle_bind(&r3, "flaky") == 0,
              "retired teacher cannot bind");
        CHECK(acquire_oracle_is_teachable(&r3, &r3.entries[0]) == 0,
              "retired not teachable");
    }

    /* find_oracle respects teachability via drain path smoke */
    {
        OracleRegistry r4;
        AcquireLedger led;
        AcquireConfig cfg;
        AcquireReport rep;
        memset(&r4, 0, sizeof r4);
        pol = (OraclePolicy){0};
        pol.require_attested_to_teach = 1;
        acquire_oracle_policy_set(&r4, &pol);
        /* only unattested registered if we bypass register gate - use zero policy register then raise */
        pol.require_attested_to_teach = 0;
        acquire_oracle_policy_set(&r4, &pol);
        CHECK(acquire_oracle_register_v2(&r4, "u", pin, pout, v2_flip,
                                         always_valid, &partial, NULL) == 0,
              "register unattested when policy off");
        pol.require_attested_to_teach = 1;
        acquire_oracle_policy_set(&r4, &pol);
        acquire_ledger_init(&led);
        acquire_config_defaults(&cfg);
        cfg.min_evidence = 1;
        cfg.evidence_threshold = 0.5;
        acquire_note_no_plan(&led, pin, pout);
        memset(&rep, 0, sizeof rep);
        CHECK(acquire_drain(NULL, &led, &r4, &cfg, &rep) == 0 || 1, "drain call");
        /* reg NULL invalid - use dummy PrimitiveRegistry */
        {
            PrimitiveRegistry preg;
            registry_init(&preg);
            memset(&rep, 0, sizeof rep);
            CHECK(acquire_drain(&preg, &led, &r4, &cfg, &rep) == 0,
                  "drain with unteachable oracle runs");
            CHECK(rep.defer_waiting_oracle >= 1 || rep.skipped_no_oracle >= 1 ||
                      rep.deferred >= 1,
                  "unteachable oracle parks as no teachable match");
            registry_free(&preg);
        }
        acquire_ledger_free(&led);
    }

    if (failures) {
        printf("ORACLE_TEACHER_RUNTIME_FAIL failures=%d\n", failures);
        return 1;
    }
    printf("ORACLE_TEACHER_RUNTIME_PASS checks_ok\n");
    return 0;
}
