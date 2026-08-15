/* Weight conversion door — weights nominate, table certifies, teacher leaves.
   Native C. Residual / teacher never speaks. Fixture GGUF is not Bonsai. */
#include "../include/cnet_weight_convert.h"
#include "../include/cce/cce_campaign_provenance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
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

static int need(const unsigned char **p, const unsigned char *end, size_t n) {
    if (!p || !*p || !end || (size_t)(end - *p) < n) return -1;
    *p += n;
    return 0;
}

static int rd_u32(const unsigned char **p, const unsigned char *end,
                  uint32_t *o) {
    if (need(p, end, 4) != 0) return -1;
    memcpy(o, *p - 4, 4);
    return 0;
}

static int rd_u64(const unsigned char **p, const unsigned char *end,
                  uint64_t *o) {
    if (need(p, end, 8) != 0) return -1;
    memcpy(o, *p - 8, 8);
    return 0;
}

static int skip_value(const unsigned char **p, const unsigned char *end,
                      uint32_t type) {
    uint64_t n = 0, i;
    uint32_t at = 0;
    switch (type) {
    case 0:
    case 1:
    case 7:
        return need(p, end, 1);
    case 2:
    case 3:
        return need(p, end, 2);
    case 4:
    case 5:
    case 6:
        return need(p, end, 4);
    case 10:
    case 11:
    case 12:
        return need(p, end, 8);
    case 8:
        if (rd_u64(p, end, &n) != 0) return -1;
        if (n > (uint64_t)(end - *p)) return -1;
        return need(p, end, (size_t)n);
    case 9:
        if (rd_u32(p, end, &at) != 0) return -1;
        if (rd_u64(p, end, &n) != 0) return -1;
        if (n > 4096) return -1;
        for (i = 0; i < n; ++i)
            if (skip_value(p, end, at) != 0) return -1;
        return 0;
    default:
        return -1;
    }
}

static int parse_lut(const unsigned char *base, size_t len,
                     const float **lut_out) {
    const unsigned char *p = base;
    const unsigned char *end = base + len;
    uint32_t ver = 0, nd = 0, gtype = 0;
    uint64_t nt = 0, nk = 0, i, dim0 = 0, off = 0, nlen = 0;
    uint32_t align = GGUF_ALIGN;
    size_t data0;
    if (len < 24 || memcmp(p, "GGUF", 4) != 0) return -1;
    p += 4;
    if (rd_u32(&p, end, &ver) != 0 || ver < 2 || ver > 3) return -1;
    if (rd_u64(&p, end, &nt) != 0 || rd_u64(&p, end, &nk) != 0) return -1;
    if (nt == 0 || nt > 64 || nk > 256) return -1;
    for (i = 0; i < nk; ++i) {
        uint32_t t = 0;
        char key[96];
        if (rd_u64(&p, end, &nlen) != 0 || nlen > 80) return -1;
        if ((size_t)(end - p) < (size_t)nlen) return -1;
        memset(key, 0, sizeof key);
        memcpy(key, p, (size_t)nlen);
        p += (size_t)nlen;
        if (rd_u32(&p, end, &t) != 0) return -1;
        if (strcmp(key, "general.alignment") == 0 && t == 4) {
            if (rd_u32(&p, end, &align) != 0 || align == 0) return -1;
        } else if (skip_value(&p, end, t) != 0) {
            return -1;
        }
    }
    for (i = 0; i < nt; ++i) {
        char name[96];
        if (rd_u64(&p, end, &nlen) != 0 || nlen > 80) return -1;
        if ((size_t)(end - p) < (size_t)nlen) return -1;
        memset(name, 0, sizeof name);
        memcpy(name, p, (size_t)nlen);
        p += (size_t)nlen;
        if (rd_u32(&p, end, &nd) != 0 || nd != 1) {
            /* skip remaining tensors that are not our 1-D LUT */
            uint32_t d;
            uint64_t dummy;
            if (nd == 0 || nd > 8) return -1;
            for (d = 0; d < nd; ++d)
                if (rd_u64(&p, end, &dummy) != 0) return -1;
            if (rd_u32(&p, end, &gtype) != 0) return -1;
            if (rd_u64(&p, end, &dummy) != 0) return -1;
            continue;
        }
        if (rd_u64(&p, end, &dim0) != 0) return -1;
        if (rd_u32(&p, end, &gtype) != 0) return -1;
        if (rd_u64(&p, end, &off) != 0) return -1;
        if (strcmp(name, CNET_WEIGHT_LUT_TENSOR) != 0) continue;
        if (gtype != GGML_F32 || dim0 != U8_INC16_N) return -1;
        data0 = (size_t)(p - base);
        data0 = (data0 + (size_t)align - 1u) & ~((size_t)align - 1u);
        if (data0 + (size_t)off + U8_INC16_N * sizeof(float) > len) return -1;
        *lut_out = (const float *)(base + data0 + (size_t)off);
        return 0;
    }
    return -1;
}

int cnet_weight_mmap(CnetWeightFile *f, const char *path) {
    const float *lut = NULL;
#if defined(_WIN32)
    FILE *fp;
    long n;
    void *buf;
#else
    int fd;
    struct stat st;
    void *map;
#endif
    if (!f || !path || !path[0]) return -1;
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    f->fd = -1;
    if (cce_sha256_file_hex(path, f->sha256_hex) != 0) return -1;
    if (hex_to_sha(f->sha256_hex, f->sha256) != 0) return -1;
#if defined(_WIN32)
    fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0 || (n = ftell(fp)) <= 0 ||
        n > 1024L * 1024L || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    buf = malloc((size_t)n);
    if (!buf) {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    f->map = buf;
    f->map_len = (size_t)n;
#else
    fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 ||
        (size_t)st.st_size > (size_t)1024 * 1024) {
        close(fd);
        return -1;
    }
    map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return -1;
    }
    f->fd = fd;
    f->map = map;
    f->map_len = (size_t)st.st_size;
#endif
    if (parse_lut((const unsigned char *)f->map, f->map_len, &lut) != 0) {
        cnet_weight_unmap(f);
        return -1;
    }
    f->lut = lut;
    f->lut_n = U8_INC16_N;
    f->fixture = 1;
    f->attested = 0;
    return 0;
}

void cnet_weight_unmap(CnetWeightFile *f) {
    if (!f) return;
#if defined(_WIN32)
    free(f->map);
#else
    if (f->map && f->map_len)
        munmap(f->map, f->map_len);
    if (f->fd >= 0) close(f->fd);
#endif
    memset(f, 0, sizeof *f);
    f->fd = -1;
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
    if (!wo || !o || !f || !f->lut) return -1;
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
    if (acquire_oracle_register_v2_family(o, nm, "weight_fixture", pin, pout,
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
    return 0;
}

int cnet_weight_labeler(void *ctx, const double *in, size_t in_total,
                        double *out, size_t out_total) {
    CnetWeightOracle *wo = (CnetWeightOracle *)ctx;
    unsigned x = 0;
    unsigned y;
    float lab;
    const OracleEntry *e;
    size_t i;
    if (!wo || !in || !out) return -1;
    if (!wo->bound || wo->lease == 0) return -1;
    if (!wo->file.lut || wo->file.lut_n != U8_INC16_N) return -1;
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
    lab = wo->file.lut[x];
    if (lab < 0.0f || lab > 15.0f) return -1;
    y = (unsigned)(lab + 0.5f);
    if (y > 15u) return -1;
    encode_bin4(y, out);
    wo->teacher_calls++;
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
    rep->certified = 0;
    pin = cnet_weight_u8_inc16_in_port();
    pout = cnet_weight_u8_inc16_out_port();
    if (!wo->bound || wo->lease == 0) return -1;
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
    plan_table_free(&table);
    return rc;
}

int cnet_weight_serve(const CnetWeightOracle *wo,
                      BinaryTransformNetwork *student,
                      const double *in, double *out) {
    const double *raw;
    if (!wo || !student || !in || !out) return -1;
    if (wo->bound || wo->lease != 0) return -1;
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
