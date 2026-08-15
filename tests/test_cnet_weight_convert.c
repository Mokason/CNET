/* Weight conversion door gate — GGUF labeler slice.
 * make cnet_weight_convert → CNET_WEIGHT_CONVERT_PASS
 *
 * Reader is cce_gguf_load (not parse_lut). Labeler is a silent named-tensor
 * read. residual_gguf_oracle is a mouth and is not called. Attest → lease
 * → plan_table_build → native student → ≥0.95 → unbind. Unattested cannot
 * teach. Tensor remap is refused. Fixture is not Bonsai. No auto-CERT.
 * No Python.
 */
#include <stdio.h>
#include <string.h>

#include "../include/cnet_weight_convert.h"
#include "../include/cce/cce_gguf.h"

static int failures, checks;
static size_t residual_oracle_calls;
static size_t residual_batch_calls;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-62s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* Mouth stubs: if convert ever speaks next-tokens, these fire.
   residual_gguf.c is not linked. */
int residual_gguf_oracle(const double *in, double *out, void *ctx) {
    (void)in;
    (void)out;
    (void)ctx;
    residual_oracle_calls++;
    return -1;
}

int residual_gguf_label_batch(void *r, const int *slot_order, int n_slots,
                              double *inputs, double *targets, int in_dim,
                              int out_dim) {
    (void)r;
    (void)slot_order;
    (void)n_slots;
    (void)inputs;
    (void)targets;
    (void)in_dim;
    (void)out_dim;
    residual_batch_calls++;
    return -1;
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
    cce_gguf *probe = NULL;
    size_t reads_at_unbind;
    unsigned x;
    int serve_ok;

    printf("== weight conversion door (u8_inc16, cce_gguf labeler, fixture) ==\n");
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

    check(cce_gguf_load(path, &probe) == CCE_OK && probe != NULL,
          "file loads with cce_gguf_load (not parse_lut)");
    check(cce_gguf_find_tensor(probe, CNET_WEIGHT_LUT_TENSOR) == 0,
          "cce_gguf_find_tensor locates u8_inc16.lut");
    {
        float lut[16];
        int idx = cce_gguf_find_tensor(probe, CNET_WEIGHT_LUT_TENSOR);
        check(idx >= 0 && cce_gguf_load_f32(probe, idx, lut, 16) == CCE_OK &&
                  lut[0] == 1.0f && lut[15] == 0.0f,
              "cce_gguf_load_f32 reads the increment LUT");
    }
    cce_gguf_free(probe);
    probe = NULL;

    check(cnet_weight_mmap(&file, path) == 0 && file.lut &&
              file.lut_n == CNET_WEIGHT_U8_INC16_COMBOS && file.gguf,
          "open fixture through cce_gguf (LUT, no FP16 workspace)");
    check(file.reader_cce_gguf == 1, "reader is cce_gguf_load");
    check(file.lut_via_load_f32 == 1, "LUT filled by cce_gguf_load_f32");
    check(file.fixture == 1, "fixture flag set (not a real host GGUF)");
    check(file.file_len < 4096, "fixture stays tiny (not a multi-GB materialize)");

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
    {
        cce_gguf *bad = (cce_gguf *)(void *)1;
        check(cce_gguf_load(junk, &bad) != CCE_OK,
              "unattested junk is not a GGUF (cce_gguf_load refuses)");
    }

    acquire_oracle_policy_defaults(&pol);
    pol.require_attested_to_teach = 1;
    pol.require_lease_to_teach = 1;
    acquire_oracle_policy_set(&reg, &pol);

    check(acquire_oracle_register_v2(&reg, "unattested_weight", pin, pout,
                                     dummy_v2, NULL, &weak, NULL) != 0,
          "unattested identity cannot register under policy");

    {
        OracleRegistry r2;
        memset(&r2, 0, sizeof r2);
        acquire_oracle_policy_set(&r2, &pol);
        check(acquire_oracle_register_v2_family(&r2, "weak_oracle",
                                                "weight_gguf", pin, pout,
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
    check(rep.reader_cce_gguf == 1, "report records cce_gguf reader");
    check(strcmp(rep.domain, CNET_WEIGHT_DOMAIN_U8_INC16) == 0 &&
              rep.combos == 16,
          "first domain is u8_inc16 with 16 combos");
    check(wo.teacher_calls == 0 && rep.residual_speak == 0,
          "labeler never increments teacher_calls / residual speak");
    check(wo.residual_speak == 0 && residual_oracle_calls == 0 &&
              residual_batch_calls == 0,
          "residual_gguf_oracle / label_batch were not called");
    check(wo.file.gguf_reads >= CNET_WEIGHT_U8_INC16_COMBOS,
          "labeler read the named tensor on the full domain");

    reads_at_unbind = wo.file.gguf_reads;
    cnet_weight_unmap(&file);
    check(file.gguf == NULL, "GGUF handle released after unbind");

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
    check(serve_ok, "serve student only after GGUF is gone");
    check(wo.file.gguf_reads == reads_at_unbind,
          "serve does not touch the GGUF after unbind");
    check(wo.teacher_calls == 0 && wo.residual_speak == 0,
          "serve does not increment teacher_calls / residual speak");
    check(cnet_weight_labeler(&wo, NULL, 0, NULL, 0) == -1,
          "unbound labeler refuses (teacher cannot speak)");

    {
        OracleScorecard sc;
        memset(&sc, 0, sizeof sc);
        check(acquire_oracle_scorecard(&reg, CNET_WEIGHT_ORACLE_NAME, &sc) == 0 &&
                  sc.attested == 1 && sc.leased == 0,
              "scorecard: attested, lease released");
    }

    check(residual_oracle_calls == 0 && residual_batch_calls == 0,
          "residual mouth counters still zero at exit");

    btn_free(&student);
    remove(path);
    remove(junk);

    printf("python=0\n");
    printf("host_gguf=0 fixture_through_cce_gguf=1\n");

    if (failures) {
        printf("CNET_WEIGHT_CONVERT_FAIL failures=%d checks=%d\n",
               failures, checks);
        return 1;
    }
    printf("CNET_WEIGHT_CONVERT_PASS\n");
    printf("checks=%d\n", checks);
    return 0;
}
