/* Weight conversion door gate.
 * make cnet_weight_convert → CNET_WEIGHT_CONVERT_PASS
 *
 * Attest fixture → lease → plan_table_build → native student → ≥0.95
 * → unbind. Unattested cannot teach. Tensor remap is refused.
 * Fixture is not Bonsai conversion. No auto-CERT.
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_weight_convert.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int aborting_teacher(void *ctx, const double *in, size_t in_total,
                            double *out, size_t out_total) {
    CnetWeightOracle *wo = (CnetWeightOracle *)ctx;
    wo->abort_on = 15;
    return cnet_weight_labeler(wo, in, in_total, out, out_total);
}

static int dummy_v2(const double *in, size_t in_count, double *out,
                    size_t out_count, CnetOracleResult *result, void *ctx) {
    (void)in; (void)in_count; (void)out; (void)out_count; (void)ctx;
    if (!result) return -1;
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    result->status = CNET_ORACLE_REFUSE_POLICY;
    return 0;
}

int main(void) {
    const char *path = "u8_inc16_fixture.gguf";
    const char *junk = "u8_inc16_unattested.bin";
    CnetWeightFile file;
    CnetWeightOracle wo;
    OracleRegistry reg;
    OraclePolicy pol;
    CnetOracleIdentity id, weak;
    BinaryTransformNetwork student;
    CnetWeightConvertReport rep;
    PlanTable table;
    Port pin, pout;
    FILE *jf;
    size_t teacher_at_unbind;
    unsigned x;
    int serve_ok;

    printf("== weight conversion door (u8_inc16, 16 combos, fixture) ==\n");
    memset(&file, 0, sizeof file);
    memset(&wo, 0, sizeof wo);
    memset(&reg, 0, sizeof reg);
    memset(&student, 0, sizeof student);
    memset(&rep, 0, sizeof rep);
    memset(&table, 0, sizeof table);
    pin = cnet_weight_u8_inc16_in_port();
    pout = cnet_weight_u8_inc16_out_port();

    check(cnet_weight_tensor_remap_admit(path) == -1,
          "naive tensor remap admit is refused");
    check(cnet_weight_tensor_remap_admit(NULL) == -1,
          "tensor remap admit refuses NULL path");

    check(cnet_weight_write_u8_inc16_fixture(path) == 0,
          "write tiny synthetic GGUF fixture");
    check(cnet_weight_mmap(&file, path) == 0 && file.lut &&
              file.lut_n == CNET_WEIGHT_U8_INC16_COMBOS,
          "mmap fixture GGUF (LUT view, no FP16 workspace)");
    check(file.fixture == 1, "fixture flag set (not a real host GGUF)");
    check(file.map_len < 4096, "fixture stays tiny (not a multi-GB materialize)");

    check(cnet_weight_attest(path, &id) == 0 &&
              cnet_oracle_identity_is_attested(&id) == 1,
          "attest fixture SHA-256 + toolchain");
    check(id.artifact_sha256[0] || id.artifact_sha256[15] ||
              id.artifact_sha256[31],
          "artifact SHA-256 is non-zero");

    jf = fopen(junk, "wb");
    check(jf != NULL, "create unattested junk file");
    if (jf) {
        fputs("not-a-weight-file", jf);
        fclose(jf);
    }
    memset(&weak, 0, sizeof weak);
    weak.abi_version = CNET_ORACLE_ABI_VERSION;
    weak.struct_size = (uint32_t)sizeof weak;
    weak.artifact_digest = 1;
    weak.contract_digest = 2;
    check(cnet_oracle_identity_is_attested(&weak) == 0,
          "zero SHA / zero toolchain is unattested");

    acquire_oracle_policy_defaults(&pol);
    pol.require_attested_to_teach = 1;
    pol.require_lease_to_teach = 1;
    acquire_oracle_policy_set(&reg, &pol);

    check(acquire_oracle_register_v2(&reg, "unattested_weight", pin, pout,
                                     dummy_v2, NULL, &weak, NULL) != 0,
          "unattested identity cannot register under policy");

    /* v2 register still needs a fn; the policy gate fires first on identity. */
    {
        OracleRegistry r2;
        memset(&r2, 0, sizeof r2);
        acquire_oracle_policy_set(&r2, &pol);
        check(acquire_oracle_register_v2_family(&r2, "weak_oracle",
                                                "weight_fixture", pin, pout,
                                                dummy_v2, NULL, &weak, NULL) != 0,
              "unattested file cannot teach (register refused)");
    }

    check(cnet_weight_bind(&wo, &reg, &file, CNET_WEIGHT_ORACLE_NAME) == 0 &&
              wo.lease > 0 && wo.bound,
          "bind attested fixture with lease");
    check(acquire_oracle_is_teachable(&reg, &reg.entries[0]) == 1,
          "leased attested oracle is teachable");

    check(plan_table_build(&pin, 1, 4, cnet_weight_labeler, &wo, NULL, 0,
                           CNET_WEIGHT_U8_INC16_COMBOS, &table) == 0,
          "plan_table_build over u8_inc16");
    check(table.kept == CNET_WEIGHT_U8_INC16_COMBOS && table.aborts == 0,
          "all 16 combos labeled, zero aborts");
    check(table.in_total == 4 && table.out_total == 4,
          "domain is 4-bit binary in/out (no RAW)");
    {
        int rows_ok = 1;
        for (x = 0; x < 16; ++x) {
            const double *in = table.inputs + (size_t)x * table.in_total;
            const double *out = table.targets + (size_t)x * table.out_total;
            unsigned a = 0, b = 0;
            size_t j;
            for (j = 0; j < 4; ++j) {
                if (in[j] > 0.5) a |= 1u << (3u - (unsigned)j);
                if (out[j] > 0.5) b |= 1u << (3u - (unsigned)j);
            }
            if (b != ((a + 1u) & 15u)) rows_ok = 0;
        }
        check(rows_ok, "LUT labels are increment-mod-16 on every combo");
    }
    plan_table_free(&table);

    wo.abort_on = -1;
    check(plan_table_build(&pin, 1, 4, aborting_teacher, &wo, NULL, 0,
                           CNET_WEIGHT_U8_INC16_COMBOS, &table) == 0 &&
              table.kept == 15 && table.aborts == 1,
          "strict teacher abort is excluded, not guessed");
    plan_table_free(&table);
    wo.abort_on = -1;

    check(cnet_weight_convert(&wo, &student, &rep) == 0,
          "convert: table → native student → unbind");
    check(rep.kept == 16 && rep.aborts == 0, "convert kept the full domain");
    check(rep.spec_rate + 1e-12 >= CNET_WEIGHT_SPEC_BAR,
          "full-domain spec-reproduction ≥ 0.95 (train=verify)");
    check(rep.verified >= 16, "student matches every labeled combo");
    check(rep.teacher_unbound == 1 && wo.bound == 0 && wo.lease == 0,
          "teacher unbound after table passes");
    check(rep.certified == 0, "fail-closed: door does not auto-CERT");
    check(rep.fixture == 1, "report records fixture (not Bonsai)");
    check(strcmp(rep.domain, CNET_WEIGHT_DOMAIN_U8_INC16) == 0 &&
              rep.combos == 16,
          "first domain is u8_inc16 with 16 combos");

    teacher_at_unbind = wo.teacher_calls;
    serve_ok = 1;
    for (x = 0; x < 16; ++x) {
        double in[4], out[4], clean[4];
        size_t j;
        for (j = 0; j < 4; ++j)
            in[j] = (double)((x >> (3u - (unsigned)j)) & 1u);
        if (cnet_weight_serve(&wo, &student, in, out) != 0) serve_ok = 0;
        if (!port_validate(pout, out) ||
            port_canonicalize(pout, out, clean) != 0)
            serve_ok = 0;
        {
            unsigned y = 0;
            for (j = 0; j < 4; ++j)
                if (clean[j] > 0.5) y |= 1u << (3u - (unsigned)j);
            if (y != ((x + 1u) & 15u)) serve_ok = 0;
        }
    }
    check(serve_ok, "serve student only over the full domain");
    check(wo.teacher_calls == teacher_at_unbind,
          "serve does not call the teacher after unbind");
    check(cnet_weight_labeler(&wo, NULL, 0, NULL, 0) == -1,
          "unbound labeler refuses (teacher cannot speak)");

    {
        OracleScorecard sc;
        memset(&sc, 0, sizeof sc);
        check(acquire_oracle_scorecard(&reg, CNET_WEIGHT_ORACLE_NAME, &sc) == 0 &&
                  sc.attested == 1 && sc.leased == 0,
              "scorecard: attested, lease released");
    }

    btn_free(&student);
    cnet_weight_unmap(&file);
    remove(path);
    remove(junk);

    if (failures) {
        printf("CNET_WEIGHT_CONVERT_FAIL failures=%d checks=%d\n",
               failures, checks);
        return 1;
    }
    printf("CNET_WEIGHT_CONVERT_PASS\n");
    printf("checks=%d\n", checks);
    return 0;
}
