/*
 * Loader-robustness gate: systematic single-byte corruption + truncation
 * sweeps over every on-disk artifact loader.
 *
 *   sealed formats (.cnu unit, .cnb base):
 *       EVERY single-byte flip and EVERY truncation must be REFUSED —
 *       seal checked before parsing. This generalizes the handful of
 *       hand-picked tamper cases in test_unit.c / test_base.c to the
 *       whole file, byte by byte.
 *
 *   probe/archive formats (GGUF, safetensors, .cce archive):
 *       no format-level seal exists, so accepting a payload flip is legal;
 *       the property is crash-freedom across the full sweep (a segfault
 *       kills this process and fails the gate) plus clean error returns.
 *       Refusal rates are printed as info, not asserted.
 */
#include "../include/nn.h"
#include "../include/router.h"
#include "../include/contract/contract.h"
#include "../include/contract/unit.h"
#include "../include/base.h"
#include "../include/cce/cce_archive.h"
#include "../include/cce/cce_detect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int failures = 0;

#define CHECK(cond, desc) do {                          \
    if (cond) { printf("  ok   %s\n", (desc)); }        \
    else { printf("  FAIL %s\n", (desc)); ++failures; } \
} while (0)

/* ---------- fixture: the trained decoder from test_unit.c ---------- */

static Port PT(PortFamily family, size_t field_width, size_t field_count,
               const char *tag) {
    Port p;
    p.family = family;
    p.field_width = field_width;
    p.field_count = field_count;
    p.tag[0] = '\0';
    if (tag != NULL) {
        port_set_tag(&p, tag);
    }
    return p;
}

static void msb2(int i, double *out) {
    out[0] = (double)((i >> 1) & 1);
    out[1] = (double)(i & 1);
}

static double g_inputs[4][4];
static double g_targets[4][2];

static int make_decoder(BinaryTransformNetwork *b) {
    int i;
    memset(g_inputs, 0, sizeof g_inputs);
    if (btn_init(b, 4, 2, 1, 16, 0.8, 11u) != 0) return -1;
    if (btn_set_ports(b, PT(PORT_ONEHOT, 4, 1, "sym"),
                      PT(PORT_BINARY_MSB, 2, 1, "val")) != 0) return -1;
    for (i = 0; i < 4; ++i) {
        g_inputs[i][i] = 1.0;
        msb2(i, g_targets[i]);
    }
    return btn_train_dynamic(b, &g_inputs[0][0], &g_targets[0][0], 4,
                             60000, 500, 0.0015, 0.01) <= 0.05 ? 0 : -1;
}

/* ---------- byte-level helpers ---------- */

static unsigned char *slurp(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *len_out = (size_t)n;
    return buf;
}

static int spit(const char *path, const unsigned char *bytes, size_t len) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) return -1;
    if (len > 0 && fwrite(bytes, 1, len, f) != len) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static int write_flipped(const char *path, const unsigned char *bytes,
                         size_t len, size_t off) {
    unsigned char *copy = malloc(len);
    int rc;
    if (copy == NULL) return -1;
    memcpy(copy, bytes, len);
    copy[off] ^= 0xA5;
    rc = spit(path, copy, len);
    free(copy);
    return rc;
}

/* truncation ladder: structural prefixes + fractions, dedup'd, all < len */
static size_t trunc_ladder(size_t len, size_t *out, size_t cap) {
    size_t cand[16];
    size_t n = 0, i, j, k = 0;
    for (i = 0; i <= 8; ++i) cand[n++] = i;
    cand[n++] = len / 4;
    cand[n++] = len / 2;
    cand[n++] = len - 1;
    for (i = 0; i < n && k < cap; ++i) {
        int dup = 0;
        if (cand[i] >= len) continue;
        for (j = 0; j < k; ++j) if (out[j] == cand[i]) { dup = 1; break; }
        if (!dup) out[k++] = cand[i];
    }
    return k;
}

static size_t sweep_stride(size_t len) {
    return len > 8192 ? len / 8192 : 1;
}

/* ---------- sealed sweeps: every mutation must be refused ---------- */

static void sweep_cnu(const unsigned char *bytes, size_t len) {
    const char *mut = "mutate_unit_m.cnu";
    size_t off, stride = sweep_stride(len);
    size_t tried = 0, accepted = 0;
    size_t lad[16], nl, i;

    for (off = 0; off < len; off += stride) {
        BinaryTransformNetwork lb;
        Contract lc;
        if (write_flipped(mut, bytes, len, off) != 0) { ++failures; return; }
        ++tried;
        if (unit_load(&lb, &lc, mut) == 0) {
            ++accepted;
            contract_free(&lc);
            btn_free(&lb);
        }
    }
    printf("  info .cnu flip sweep: %zu/%zu offsets refused (stride %zu)\n",
           tried - accepted, tried, stride);
    CHECK(accepted == 0, ".cnu: EVERY single-byte flip refused");

    nl = trunc_ladder(len, lad, 16);
    accepted = 0;
    for (i = 0; i < nl; ++i) {
        BinaryTransformNetwork lb;
        Contract lc;
        if (spit(mut, bytes, lad[i]) != 0) { ++failures; return; }
        if (unit_load(&lb, &lc, mut) == 0) {
            ++accepted;
            contract_free(&lc);
            btn_free(&lb);
        }
    }
    CHECK(accepted == 0, ".cnu: every truncation refused");
    remove(mut);
}

static void sweep_cnb(const unsigned char *bytes, size_t len) {
    const char *mut = "mutate_base_m.cnb";
    size_t off, stride = sweep_stride(len);
    size_t tried = 0, accepted = 0;
    size_t lad[16], nl, i;

    for (off = 0; off < len; off += stride) {
        CnetBase mb;
        if (write_flipped(mut, bytes, len, off) != 0) { ++failures; return; }
        ++tried;
        cnb_init(&mb);
        if (cnb_load(&mb, mut) == 0) ++accepted;
        cnb_free(&mb);
    }
    printf("  info .cnb flip sweep: %zu/%zu offsets refused (stride %zu)\n",
           tried - accepted, tried, stride);
    CHECK(accepted == 0, ".cnb: EVERY single-byte flip refused");

    nl = trunc_ladder(len, lad, 16);
    accepted = 0;
    for (i = 0; i < nl; ++i) {
        CnetBase mb;
        if (spit(mut, bytes, lad[i]) != 0) { ++failures; return; }
        cnb_init(&mb);
        if (cnb_load(&mb, mut) == 0) ++accepted;
        cnb_free(&mb);
    }
    CHECK(accepted == 0, ".cnb: every truncation refused");
    remove(mut);
}

/* ---------- probe sweeps: crash-freedom, refusal rate as info ---------- */

/* tiny GGUF fixture (same shape as cce_detect_test.c's qwen2ish writer) */
static void w_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }
static void w_str(FILE *f, const char *s) { w_u64(f, strlen(s)); fwrite(s, 1, strlen(s), f); }
static void w_kv_str(FILE *f, const char *k, const char *v) {
    w_str(f, k); w_u32(f, 8 /*GGUF_TYPE_STRING*/); w_str(f, v);
}
static void w_kv_u32(FILE *f, const char *k, uint32_t v) {
    w_str(f, k); w_u32(f, 4 /*GGUF_TYPE_UINT32*/); w_u32(f, v);
}
static void w_tensor(FILE *f, const char *name, int d0, int d1, uint32_t t) {
    w_str(f, name);
    w_u32(f, d1 > 0 ? 2 : 1);
    w_u64(f, (uint64_t)d0);
    if (d1 > 0) w_u64(f, (uint64_t)d1);
    w_u32(f, t);
    w_u64(f, 0);
}

static void write_gguf_fixture(const char *path) {
    FILE *f = fopen(path, "wb");
    int l;
    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 11);
    w_u64(f, 7);
    w_kv_str(f, "general.architecture", "qwen2");
    w_kv_u32(f, "qwen2.block_count", 2);
    w_kv_u32(f, "qwen2.embedding_length", 64);
    w_kv_u32(f, "qwen2.attention.head_count", 8);
    w_kv_u32(f, "qwen2.attention.head_count_kv", 2);
    w_kv_u32(f, "qwen2.context_length", 512);
    w_kv_u32(f, "qwen2.feed_forward_length", 128);
    w_tensor(f, "token_embd.weight", 64, 1000, 8);
    for (l = 0; l < 2; ++l) {
        char n[96];
        snprintf(n, sizeof(n), "blk.%d.attn_q.weight", l);      w_tensor(f, n, 64, 64, 8);
        snprintf(n, sizeof(n), "blk.%d.attn_k.weight", l);      w_tensor(f, n, 64, 16, 8);
        snprintf(n, sizeof(n), "blk.%d.attn_v.weight", l);      w_tensor(f, n, 64, 16, 8);
        snprintf(n, sizeof(n), "blk.%d.attn_output.weight", l); w_tensor(f, n, 64, 64, 8);
        snprintf(n, sizeof(n), "blk.%d.ffn_gate.weight", l);    w_tensor(f, n, 64, 128, 8);
    }
    fclose(f);
}

/* tiny safetensors fixture: valid JSON header + zeroed data section */
static void write_st_fixture(const char *path) {
    const char *names[] = {
        "model.layers.0.self_attn.q_proj.weight",
        "model.layers.0.self_attn.k_proj.weight",
        "model.layers.0.self_attn.v_proj.weight",
        "model.layers.0.mlp.gate_proj.weight",
    };
    char json[4096];
    size_t j = 0;
    uint64_t off = 0, hlen;
    size_t i;
    FILE *f;

    j += (size_t)snprintf(json + j, sizeof(json) - j, "{");
    for (i = 0; i < 4; ++i) {
        uint64_t sz = 16; /* 2x2 f32 */
        j += (size_t)snprintf(json + j, sizeof(json) - j,
                      "%s\"%s\":{\"dtype\":\"F32\",\"shape\":[2,2],\"data_offsets\":[%llu,%llu]}",
                      i > 0 ? "," : "", names[i],
                      (unsigned long long)off, (unsigned long long)(off + sz));
        off += sz;
    }
    j += (size_t)snprintf(json + j, sizeof(json) - j, "}");

    f = fopen(path, "wb");
    hlen = (uint64_t)j;
    fwrite(&hlen, 8, 1, f);
    fwrite(json, 1, j, f);
    {
        char *zeros = calloc(1, (size_t)off);
        fwrite(zeros, 1, (size_t)off, f);
        free(zeros);
    }
    fclose(f);
}

static void sweep_detect(const char *label, const unsigned char *bytes,
                         size_t len, const char *mut) {
    size_t off, stride = sweep_stride(len);
    size_t tried = 0, probed_ok = 0;
    size_t lad[16], nl, i;

    for (off = 0; off < len; off += stride) {
        cce_model_info info;
        if (write_flipped(mut, bytes, len, off) != 0) { ++failures; return; }
        ++tried;
        if (cce_detect_file(mut, &info) == CCE_OK) ++probed_ok;
    }
    printf("  info %s flip sweep: %zu/%zu probes returned CCE_OK, "
           "rest failed cleanly, 0 crashes (stride %zu)\n",
           label, probed_ok, tried, stride);

    nl = trunc_ladder(len, lad, 16);
    for (i = 0; i < nl; ++i) {
        cce_model_info info;
        if (spit(mut, bytes, lad[i]) != 0) { ++failures; return; }
        (void)cce_detect_file(mut, &info);
    }
    CHECK(1, label); /* reached = no crash across flips + truncations */
    remove(mut);
}

static void sweep_archive(void) {
    const char *src = "mutate_arch.cce";
    const char *mut = "mutate_arch_m.cce";
    unsigned char *bytes;
    size_t len, off, stride, tried = 0, opened = 0;
    size_t lad[16], nl, i;

    remove(src);
    {
        cce_archive *ar = NULL;
        float data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        size_t soff = 0;
        if (cce_archive_open(&ar, src) != CCE_OK ||
            cce_archive_append_section(ar, "sec", data, sizeof data, &soff) != CCE_OK) {
            printf("  FAIL archive fixture build\n");
            ++failures;
            return;
        }
        cce_archive_close(ar);
    }
    bytes = slurp(src, &len);
    if (bytes == NULL) { ++failures; return; }

    stride = sweep_stride(len);
    for (off = 0; off < len; off += stride) {
        cce_archive *ar = NULL;
        if (write_flipped(mut, bytes, len, off) != 0) { ++failures; free(bytes); return; }
        ++tried;
        if (cce_archive_open(&ar, mut) == CCE_OK) {
            size_t foff = 0, fsz = 0;
            ++opened;
            if (cce_archive_find_section(ar, "sec", &foff, &fsz) == CCE_OK &&
                fsz <= 64) {
                unsigned char buf[64];
                (void)cce_archive_read_raw(ar, foff, buf, fsz);
            }
            cce_archive_close(ar);
        }
    }
    printf("  info .cce flip sweep: %zu/%zu opens succeeded, "
           "rest failed cleanly, 0 crashes (stride %zu)\n",
           opened, tried, stride);

    nl = trunc_ladder(len, lad, 16);
    for (i = 0; i < nl; ++i) {
        cce_archive *ar = NULL;
        if (spit(mut, bytes, lad[i]) != 0) { ++failures; free(bytes); return; }
        if (cce_archive_open(&ar, mut) == CCE_OK) cce_archive_close(ar);
    }
    CHECK(1, ".cce archive: no crash across flips + truncations");
    free(bytes);
    remove(src);
    remove(mut);
}

int main(void) {
    BinaryTransformNetwork decoder;
    Contract c;

    /* unbuffered: a crash mid-sweep must not eat the progress log */
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("loader robustness (mutation + truncation sweeps):\n");

    if (make_decoder(&decoder) != 0) {
        printf("  FAIL fixture training\n");
        return 1;
    }
    if (contract_init_borrowed(&c, "decoder", &decoder,
                               &g_inputs[0][0], &g_targets[0][0], 4) != 0) {
        printf("  FAIL contract init\n");
        return 1;
    }

    /* ---- sealed unit file (.cnu) ---- */
    {
        unsigned char *bytes;
        size_t len;
        BinaryTransformNetwork lb;
        Contract lc;
        CHECK(unit_save(&decoder, &c, "mutate_unit.cnu") == 0, "unit fixture saves");
        CHECK(unit_load(&lb, &lc, "mutate_unit.cnu") == 0, "pristine unit loads");
        contract_free(&lc);
        btn_free(&lb);
        bytes = slurp("mutate_unit.cnu", &len);
        CHECK(bytes != NULL && len > 0, "unit fixture readable");
        if (bytes != NULL) {
            sweep_cnu(bytes, len);
            free(bytes);
        }
        remove("mutate_unit.cnu");
    }

    /* ---- sealed base container (.cnb) ---- */
    {
        CnetBase b, chk;
        unsigned char *bytes;
        size_t len;
        int reused = -1;
        cnb_init(&b);
        CHECK(cnb_add_unit(&b, &decoder, &c, &reused) == 0, "base fixture ingests");
        CHECK(cnb_save(&b, "mutate_base.cnb") == 0, "base fixture saves");
        cnb_free(&b);
        cnb_init(&chk);
        CHECK(cnb_load(&chk, "mutate_base.cnb") == 0, "pristine base loads");
        cnb_free(&chk);
        bytes = slurp("mutate_base.cnb", &len);
        CHECK(bytes != NULL && len > 0, "base fixture readable");
        if (bytes != NULL) {
            sweep_cnb(bytes, len);
            free(bytes);
        }
        remove("mutate_base.cnb");
    }

    /* ---- unsealed probes: crash-freedom ---- */
    {
        unsigned char *bytes;
        size_t len;
        cce_model_info info;

        write_gguf_fixture("mutate_fix.gguf");
        CHECK(cce_detect_file("mutate_fix.gguf", &info) == CCE_OK,
              "pristine gguf probes");
        bytes = slurp("mutate_fix.gguf", &len);
        CHECK(bytes != NULL && len > 0, "gguf fixture readable");
        if (bytes != NULL) {
            sweep_detect("gguf probe: no crash across flips + truncations",
                         bytes, len, "mutate_fix_m.gguf");
            free(bytes);
        }
        remove("mutate_fix.gguf");

        write_st_fixture("mutate_fix.safetensors");
        CHECK(cce_detect_file("mutate_fix.safetensors", &info) == CCE_OK,
              "pristine safetensors probes");
        bytes = slurp("mutate_fix.safetensors", &len);
        CHECK(bytes != NULL && len > 0, "safetensors fixture readable");
        if (bytes != NULL) {
            sweep_detect("safetensors probe: no crash across flips + truncations",
                         bytes, len, "mutate_fix_m.safetensors");
            free(bytes);
        }
        remove("mutate_fix.safetensors");
    }

    /* ---- unsealed archive (.cce): crash-freedom ---- */
    sweep_archive();

    printf(failures == 0 ? "\nAll mutation-sweep gates passed.\n"
                         : "\n%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
