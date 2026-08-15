/* Weight conversion door — weights nominate, table certifies, teacher leaves.
   Reader is cce_gguf. Residual / teacher never speaks. Fixture is not Bonsai. */
#include "../include/cnet_weight_convert.h"
#include "../include/cce/cce_campaign_provenance.h"
#include "../include/cce/cce_gguf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(CNET_RESIDUAL_GGUF_H) || defined(RESIDUAL_GGUF_H)
#error "residual_gguf is a mouth; the conversion labeler must not include it"
#endif

#define U8_INC16_W 4u
#define U8_INC16_N 16u
#define GGUF_ALIGN 32u
#define GGML_F32 0u

static uint64_t fnv1a64(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    uint64_t h = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < n; ++i) {
        h ^= (uint64_t)b[i];
        h *= UINT64_C(1099511628211);
    }
    return h ? h : UINT64_C(1);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex_to_sha(const char hex[65], unsigned char out[32]) {
    size_t i;
    if (!hex || !out) return -1;
    for (i = 0; i < 32; ++i) {
        int hi = hex_nibble(hex[i * 2u]);
        int lo = hex_nibble(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (unsigned char)((hi << 4) | lo);
    }
    return 0;
}

static Port bin4(const char *tag) {
    Port p;
    memset(&p, 0, sizeof p);
    p.family = PORT_BINARY_MSB;
    p.field_width = U8_INC16_W;
    p.field_count = 1;
    if (tag) snprintf(p.tag, sizeof p.tag, "%s", tag);
    return p;
}

Port cnet_weight_u8_inc16_in_port(void) {
    return bin4("u8_inc16_in");
}

Port cnet_weight_u8_inc16_out_port(void) {
    return bin4("u8_inc16_out");
}

static int decode_bin4(const double *v, unsigned *out) {
    unsigned x = 0;
    size_t j;
    if (!v || !out) return -1;
    for (j = 0; j < U8_INC16_W; ++j) {
        if (v[j] >= 0.25 && v[j] <= 0.75) return -1;
        if (v[j] > 0.5)
            x |= 1u << (unsigned)(U8_INC16_W - 1u - (unsigned)j);
    }
    *out = x;
    return 0;
}

static void encode_bin4(unsigned x, double *v) {
    size_t j;
    for (j = 0; j < U8_INC16_W; ++j) {
        unsigned bit = (unsigned)(U8_INC16_W - 1u - (unsigned)j);
        v[j] = (double)((x >> bit) & 1u);
    }
}

static void wr_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void wr_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

static void wr_str(FILE *f, const char *s) {
    uint64_t n = (uint64_t)strlen(s);
    wr_u64(f, n);
    fwrite(s, 1, (size_t)n, f);
}

static void wr_kv_u32(FILE *f, const char *k, uint32_t v) {
    wr_str(f, k);
    wr_u32(f, 4); /* GGUF_TYPE_UINT32 */
    wr_u32(f, v);
}

static void wr_kv_str(FILE *f, const char *k, const char *v) {
    wr_str(f, k);
    wr_u32(f, 8); /* GGUF_TYPE_STRING */
    wr_str(f, v);
}

int cnet_weight_write_u8_inc16_fixture(const char *path) {
    FILE *f;
    float lut[U8_INC16_N];
    size_t i;
    long pos;
    int pad;
    if (!path || !path[0]) return -1;
    for (i = 0; i < U8_INC16_N; ++i)
        lut[i] = (float)((i + 1u) & 15u);
    f = fopen(path, "wb");
    if (!f) return -1;
    fwrite("GGUF", 1, 4, f);
    wr_u32(f, 3);
    wr_u64(f, 1); /* one tensor */
    wr_u64(f, 5); /* five KV */
    wr_kv_str(f, "general.architecture", "cnet_fixture");
    wr_kv_u32(f, "general.alignment", GGUF_ALIGN);
    wr_kv_str(f, "cnet.domain", CNET_WEIGHT_DOMAIN_U8_INC16);
    wr_kv_str(f, "cnet.conversion", "fixture_not_bonsai");
    wr_kv_u32(f, "cnet.combos", (uint32_t)U8_INC16_N);
    wr_str(f, CNET_WEIGHT_LUT_TENSOR);
    wr_u32(f, 1); /* ndim */
    wr_u64(f, U8_INC16_N);
    wr_u32(f, GGML_F32);
    wr_u64(f, 0);
    pos = ftell(f);
    if (pos < 0) {
        fclose(f);
        return -1;
    }
    pad = (int)((GGUF_ALIGN - (unsigned)(pos % (long)GGUF_ALIGN)) % GGUF_ALIGN);
    while (pad-- > 0) fputc(0, f);
    if (fwrite(lut, sizeof(float), U8_INC16_N, f) != U8_INC16_N) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0) return -1;
    return 0;
}

int cnet_weight_attest(const char *path, CnetOracleIdentity *id) {
    char hex[65];
    unsigned char sha[32];
    const char *atom = CNET_WEIGHT_TOOLCHAIN_ATOM;
    if (!path || !path[0] || !id) return -1;
    if (cce_sha256_file_hex(path, hex) != 0) return -1;
    if (hex_to_sha(hex, sha) != 0) return -1;
    memset(id, 0, sizeof *id);
    id->abi_version = CNET_ORACLE_ABI_VERSION;
    id->struct_size = (uint32_t)sizeof *id;
    memcpy(id->artifact_sha256, sha, 32);
    id->artifact_digest =
        ((uint64_t)sha[0] << 56) | ((uint64_t)sha[1] << 48) |
        ((uint64_t)sha[2] << 40) | ((uint64_t)sha[3] << 32) |
        ((uint64_t)sha[4] << 24) | ((uint64_t)sha[5] << 16) |
        ((uint64_t)sha[6] << 8) | (uint64_t)sha[7];
    if (id->artifact_digest == 0) id->artifact_digest = 1;
    id->contract_digest = fnv1a64(CNET_WEIGHT_DOMAIN_U8_INC16,
                                  strlen(CNET_WEIGHT_DOMAIN_U8_INC16));
    id->toolchain_digest = fnv1a64(atom, strlen(atom));
    id->runtime_libs_digest = cnet_runtime_libs_digest();
    return 0;
}

static int lut_sane(const float *lut) {
    size_t i;
    if (!lut) return -1;
    for (i = 0; i < U8_INC16_N; ++i) {
        if (lut[i] < 0.0f || lut[i] > 15.0f) return -1;
    }
    return 0;
}

/* Open the file with the real GGUF stack. private fixture parser is gone. */
int cnet_weight_mmap(CnetWeightFile *f, const char *path) {
    cce_gguf *g = NULL;
    cce_gguf_tensor_meta meta;
    const void *raw = NULL;
    size_t raw_n = 0;
    struct stat st;
    int idx;
    const char *arch;
    if (!f || !path || !path[0]) return -1;
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    if (stat(path, &st) != 0 || st.st_size <= 0) return -1;
    f->file_len = (size_t)st.st_size;
    if (cce_sha256_file_hex(path, f->sha256_hex) != 0) return -1;
    if (hex_to_sha(f->sha256_hex, f->sha256) != 0) return -1;
    if (cce_gguf_load(path, &g) != CCE_OK || !g) return -1;
    idx = cce_gguf_find_tensor(g, CNET_WEIGHT_LUT_TENSOR);
    if (idx < 0) {
        cce_gguf_free(g);
        return -1;
    }
    memset(&meta, 0, sizeof meta);
    if (cce_gguf_get_tensor_meta(g, idx, &meta) != CCE_OK ||
        meta.ndim != 1 || meta.shape[0] != (int)U8_INC16_N) {
        cce_gguf_free(g);
        return -1;
    }
    if (cce_gguf_tensor_bytes(g, idx, &raw, &raw_n) == CCE_OK && raw &&
        meta.ggml_type == GGML_F32 && raw_n >= U8_INC16_N * sizeof(float)) {
        memcpy(f->lut_store, raw, U8_INC16_N * sizeof(float));
        f->lut_via_tensor_bytes = 1;
    }
    if (cce_gguf_load_f32(g, idx, f->lut_store, U8_INC16_N) != CCE_OK) {
        cce_gguf_free(g);
        return -1;
    }
    f->lut_via_load_f32 = 1;
    if (lut_sane(f->lut_store) != 0) {
        cce_gguf_free(g);
        return -1;
    }
    arch = cce_gguf_get_arch(g);
    f->gguf = g;
    f->lut = f->lut_store;
    f->lut_n = U8_INC16_N;
    f->reader_cce_gguf = 1;
    f->fixture = (arch && strcmp(arch, "cnet_fixture") == 0) ? 1 : 0;
    f->attested = 0;
    return 0;
}

void cnet_weight_unmap(CnetWeightFile *f) {
    if (!f) return;
    if (f->gguf) cce_gguf_free(f->gguf);
    memset(f, 0, sizeof *f);
}

static int oracle_v2(const double *in, size_t in_count, double *out,
                     size_t out_count, CnetOracleResult *result, void *ctx) {
    CnetWeightOracle *wo = (CnetWeightOracle *)ctx;
    if (!result) return -1;
    memset(result, 0, sizeof *result);
    result->abi_version = CNET_ORACLE_ABI_VERSION;
    result->struct_size = (uint32_t)sizeof *result;
    if (cnet_weight_labeler(wo, in, in_count, out, out_count) != 0) {
        result->status = CNET_ORACLE_REFUSE_POLICY;
        return 0;
    }
    result->status = CNET_ORACLE_ANSWER;
    result->confidence = 1.0;
    return 0;
}

static CnetOracleValidity always_valid(const double *in, size_t ic,
                                       const double *out, size_t oc,
                                       const CnetOracleResult *r, void *ctx) {
    (void)in;
    (void)ic;
    (void)out;
    (void)oc;
    (void)r;
    (void)ctx;
    return CNET_ORACLE_VALID;
}

int cnet_weight_bind(CnetWeightOracle *wo, OracleRegistry *o,
                     CnetWeightFile *f, const char *name) {
    CnetOracleIdentity id;
    Port pin, pout;
    const char *nm;
    if (!wo || !o || !f || !f->gguf || !f->reader_cce_gguf || !f->lut)
        return -1;
    nm = (name && name[0]) ? name : CNET_WEIGHT_ORACLE_NAME;
    if (cnet_weight_attest(f->path, &id) != 0) return -1;
    if (!cnet_oracle_identity_is_attested(&id)) return -1;
    pin = cnet_weight_u8_inc16_in_port();
    pout = cnet_weight_u8_inc16_out_port();
    memset(wo, 0, sizeof *wo);
    wo->file = *f;
    wo->oracles = o;
    snprintf(wo->oracle_name, sizeof wo->oracle_name, "%s", nm);
    wo->abort_on = -1;
    wo->teacher_calls = 0;
    wo->residual_speak = 0;
    if (acquire_oracle_register_v2_family(o, nm, "weight_gguf", pin, pout,
                                          oracle_v2, always_valid, &id,
                                          wo) != 0)
        return -1;
    wo->lease = acquire_oracle_bind(o, nm);
    if (wo->lease == 0) return -1;
    wo->bound = 1;
    f->attested = 1;
    wo->file.attested = 1;
    return 0;
}

int cnet_weight_unbind(CnetWeightOracle *wo) {
    if (!wo || !wo->oracles || !wo->bound) return -1;
    if (acquire_oracle_unbind(wo->oracles, wo->oracle_name, wo->lease) != 0)
        return -1;
    wo->lease = 0;
    wo->bound = 0;
    /* Drop the borrowed GGUF view so serve/labeler cannot touch it.
       The caller's CnetWeightFile still owns the handle. */
    wo->file.gguf = NULL;
    wo->file.lut = NULL;
    return 0;
}

int cnet_weight_labeler(void *ctx, const double *in, size_t in_total,
                        double *out, size_t out_total) {
    CnetWeightOracle *wo = (CnetWeightOracle *)ctx;
    unsigned x = 0;
    unsigned y;
    float lab_buf[U8_INC16_N];
    float lab;
    const OracleEntry *e;
    size_t i;
    int idx;
    if (!wo || !in || !out) return -1;
    if (!wo->bound || wo->lease == 0) return -1;
    if (!wo->file.gguf || !wo->file.reader_cce_gguf) return -1;
    if (in_total != U8_INC16_W || out_total != U8_INC16_W) return -1;
    if (!wo->file.attested) return -1;
    e = NULL;
    for (i = 0; wo->oracles && i < wo->oracles->count; ++i) {
        if (strcmp(wo->oracles->entries[i].name, wo->oracle_name) == 0) {
            e = &wo->oracles->entries[i];
            break;
        }
    }
    if (!e || !acquire_oracle_is_teachable(wo->oracles, e)) return -1;
    if (decode_bin4(in, &x) != 0 || x >= U8_INC16_N) return -1;
    if (wo->abort_on >= 0 && (int)x == wo->abort_on) return -1;
    /* Silent typed read of the named LUT. Not residual next-token. */
    idx = cce_gguf_find_tensor(wo->file.gguf, CNET_WEIGHT_LUT_TENSOR);
    if (idx < 0) return -1;
    if (cce_gguf_load_f32(wo->file.gguf, idx, lab_buf, U8_INC16_N) != CCE_OK)
        return -1;
    wo->file.gguf_reads++;
    lab = lab_buf[x];
    if (lab < 0.0f || lab > 15.0f) return -1;
    y = (unsigned)(lab + 0.5f);
    if (y > 15u) return -1;
    encode_bin4(y, out);
    /* teacher_calls / residual_speak stay 0: this is not a mouth. */
    return 0;
}

int cnet_weight_convert(CnetWeightOracle *wo,
                        BinaryTransformNetwork *student,
                        CnetWeightConvertReport *rep) {
    PlanTable table;
    Port pin, pout;
    BinaryTransformNetwork st;
    double clean[U8_INC16_W];
    size_t s;
    int rc = -1;
    if (!wo || !student || !rep) return -1;
    memset(rep, 0, sizeof *rep);
    memset(&table, 0, sizeof table);
    memset(&st, 0, sizeof st);
    snprintf(rep->domain, sizeof rep->domain, "%s", CNET_WEIGHT_DOMAIN_U8_INC16);
    rep->combos = CNET_WEIGHT_U8_INC16_COMBOS;
    rep->fixture = wo->file.fixture;
    rep->reader_cce_gguf = wo->file.reader_cce_gguf;
    rep->certified = 0;
    pin = cnet_weight_u8_inc16_in_port();
    pout = cnet_weight_u8_inc16_out_port();
    if (!wo->bound || wo->lease == 0) return -1;
    if (!wo->file.gguf || !wo->file.reader_cce_gguf) return -1;
    if (plan_table_build(&pin, 1, U8_INC16_W, cnet_weight_labeler, wo, NULL, 0,
                         CNET_WEIGHT_U8_INC16_COMBOS, &table) != 0)
        return -1;
    rep->kept = table.kept;
    rep->aborts = table.aborts;
    if (table.kept == 0) {
        plan_table_free(&table);
        return -1;
    }
    if (btn_init(&st, U8_INC16_W, U8_INC16_W, 16, 64, 0.8, 131u) != 0) {
        plan_table_free(&table);
        return -1;
    }
    if (btn_set_io_ports(&st, &pin, 1, &pout, 1) != 0) {
        btn_free(&st);
        plan_table_free(&table);
        return -1;
    }
    (void)btn_set_momentum(&st, 0.6);
    (void)btn_train_dynamic_spec(&st, table.inputs, table.targets, table.kept,
                                 40000, 400, 0.0015, 0.01);
    for (s = 0; s < table.kept; ++s) {
        const double *raw = btn_forward(&st, table.inputs + s * table.in_total);
        const double *want = table.targets + s * table.out_total;
        int ok = raw != NULL;
        size_t i;
        if (!ok || !port_validate(pout, raw) ||
            port_canonicalize(pout, raw, clean) != 0) {
            ++rep->missed;
            continue;
        }
        ok = 1;
        for (i = 0; i < U8_INC16_W; ++i) {
            if (clean[i] != want[i]) {
                ok = 0;
                break;
            }
        }
        if (ok) ++rep->verified;
        else ++rep->missed;
    }
    rep->spec_rate = (double)rep->verified / (double)table.kept;
    if (rep->spec_rate + 1e-12 >= CNET_WEIGHT_SPEC_BAR) {
        if (cnet_weight_unbind(wo) == 0) rep->teacher_unbound = 1;
        *student = st;
        rc = 0;
    } else {
        btn_free(&st);
    }
    rep->residual_speak = wo->residual_speak;
    rep->gguf_reads = wo->file.gguf_reads;
    plan_table_free(&table);
    return rc;
}

int cnet_weight_serve(const CnetWeightOracle *wo,
                      BinaryTransformNetwork *student,
                      const double *in, double *out) {
    const double *raw;
    if (!wo || !student || !in || !out) return -1;
    if (wo->bound || wo->lease != 0) return -1;
    /* Student only. No GGUF, no labeler, no residual mouth. */
    raw = btn_forward(student, in);
    if (!raw) return -1;
    memcpy(out, raw, U8_INC16_W * sizeof(double));
    return 0;
}

int cnet_weight_tensor_remap_admit(const char *path) {
    (void)path;
    /* Copying W_q/W_k/W_v/MLP tensors is not a typed certified unit. */
    return -1;
}
