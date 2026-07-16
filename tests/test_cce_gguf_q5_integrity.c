/*
 * test_cce_gguf_q5_integrity.c — CNET 5.1.1 Q5_K/GGUF integrity slice.
 *
 * Strict TDD: this test file is written BEFORE the source fix.
 *
 * Test coverage:
 *   1) Q5_K dequant exact-match against an independent reference decoder
 *      that mirrors llama.cpp's dequantize_row_q5_K + get_scale_min_k4.
 *      Expected vectors are computed by the reference decoder embedded here
 *      (NOT the production function), using hand-crafted block data.
 *   2) cce_gguf_load fails closed (returns error, no leak) when a KV
 *      metadata record is truncated/malformed.
 *   3) cce_gguf_load fails closed when a tensor metadata record is
 *      truncated/malformed.
 *   4) All tests are CPU-only; no GPU, no real model files.
 *
 * Build:  manually with the same C11 flags the project uses:
 *   gcc -std=c11 -Wall -Wextra -pedantic -O2 -D_DEFAULT_SOURCE \
 *       -I include -o build/test_cce_gguf_q5_integrity \
 *       tests/test_cce_gguf_q5_integrity.c \
 *       src/cce/cce_gguf.c src/cce/cce_tensor.c src/cce/cce_block.c \
 *       src/cce/cce_cascade.c src/cce/cce_forest.c \
 *       src/cce/cce_compression.c src/cce/cce_detect.c \
 *       src/cce/cce_weight_store.c \
 *       -lm
 *   (Only the translation units needed for cce_gguf_load + cce_gguf_load_f32.)
 *
 * Run:  timeout 30 ./build/test_cce_gguf_q5_integrity
 *   Prints GGUF_INTEGRITY_PASS on success.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <inttypes.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_defs.h"

/* ------------------------------------------------------------------ */
/*  Independent f16->f32 (matches IEEE 754 half; NOT the production fn) */
/* ------------------------------------------------------------------ */
static float ref_f16_to_f32(uint16_t h) {
    uint32_t sign = (h >> 15) & 1;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t f;
    if (exp == 0) {
        if (mant == 0) {
            f = sign << 31;
        } else {
            /* subnormal */
            int e = -1;
            do {
                e++;
                mant <<= 1;
            } while ((mant & 0x400) == 0);
            mant &= 0x3FF;
            f = (sign << 31) | ((uint32_t)(127 - 15 - e) << 23) | (mant << 13);
        }
    } else if (exp == 0x1F) {
        /* inf/nan */
        f = (sign << 31) | (0xFF << 23) | (mant << 13);
    } else {
        f = (sign << 31) | ((uint32_t)(exp + 127 - 15) << 23) | (mant << 13);
    }
    float result;
    memcpy(&result, &f, 4);
    return result;
}

/* ------------------------------------------------------------------ */
/*  Independent Q5_K reference decoder — mirrors llama.cpp exactly.
 *
 *  block_q5_K layout (on-disk order):
 *    f16  d          (super-block scale)
 *    f16  dmin       (super-block min scale)
 *    u8   scales[12] (6-bit packed scales+mins, 8 sub-blocks)
 *    u8   qh[32]     (high-bit quants, 1 bit per element)
 *    u8   qs[128]    (low 4-bit quants, 2 per byte)
 *
 *  Dequant (llama.cpp dequantize_row_q5_K):
 *    For each 64-element group (j=0,128,192...):
 *      get_scale_min_k4(is+0) -> sc,m  => d1=d*sc, m1=min*m
 *      get_scale_min_k4(is+1) -> sc,m  => d2=d*sc, m2=min*m
 *      for l in 0..31:
 *        y = d1 * ((qs[l] & 0xF) + (qh[l] & u1 ? 16 : 0)) - m1
 *        y = d2 * ((qs[l] >> 4)  + (qh[l] & u2 ? 16 : 0)) - m2
 *      u1 <<= 2, u2 <<= 2
 *      ql += 32, is += 2
 *
 *  get_scale_min_k4(j, scales, &d, &m):
 *    if j < 4:  d = scales[j] & 63;      m = scales[j+4] & 63;
 *    else:      d = (scales[j+4] & 0xF) | ((scales[j-4] >> 6) << 4);
 *               m = (scales[j+4] >> 4)  | ((scales[j-0] >> 6) << 4);
 * ------------------------------------------------------------------ */
static void ref_get_scale_min_k4(int j, const uint8_t *q, uint8_t *d, uint8_t *m) {
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (q[j + 4] & 0x0F) | ((q[j - 4] >> 6) << 4);
        *m = (q[j + 4] >> 4) | ((q[j - 0] >> 6) << 4);
    }
}

static void ref_dequant_q5_K(const uint8_t *block, float *y, int nblocks) {
    const int QK_K = 256;
    for (int b = 0; b < nblocks; b++) {
        const uint8_t *p = block + (size_t)b * (2 + 2 + 12 + 32 + 128);
        uint16_t d16, dmin16;
        memcpy(&d16, p, 2);    p += 2;
        memcpy(&dmin16, p, 2); p += 2;
        const uint8_t *scales = p; p += 12;
        const uint8_t *qh = p;     p += 32;
        const uint8_t *ql = p;     /* 128 bytes */

        float d   = ref_f16_to_f32(d16);
        float dmin = ref_f16_to_f32(dmin16);

        int is = 0;
        uint8_t u1 = 1, u2 = 2;
        float *out = y + b * QK_K;
        for (int j = 0; j < QK_K; j += 64) {
            uint8_t sc, m;
            ref_get_scale_min_k4(is + 0, scales, &sc, &m);
            float d1 = d * sc, m1 = dmin * m;
            ref_get_scale_min_k4(is + 1, scales, &sc, &m);
            float d2 = d * sc, m2 = dmin * m;
            for (int l = 0; l < 32; l++)
                out[j + l]      = d1 * ((ql[l] & 0xF) + (qh[l] & u1 ? 16 : 0)) - m1;
            for (int l = 0; l < 32; l++)
                out[j + 32 + l] = d2 * ((ql[l] >> 4)  + (qh[l] & u2 ? 16 : 0)) - m2;
            ql += 32;
            is += 2;
            u1 <<= 2;
            u2 <<= 2;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Hand-crafted Q5_K test block (256 values, 1 superblock).
 *  We choose simple values so we can hand-verify and also cross-check
 *  against the reference decoder. The block bytes are:
 *    d=1.0 (f16=0x3C00), dmin=0.5 (f16=0x3800)
 *    scales[12]: all 0x3C -> sc=60, m=60 for all 8 sub-blocks
 *    qh[32]:     alternating pattern
 *    qs[128]:    sequential nibbles
 * ------------------------------------------------------------------ */
static void build_test_q5k_block(uint8_t *block) {
    uint8_t *p = block;
    /* d = 1.0 -> f16 = 0x3C00 */
    uint16_t d16 = 0x3C00;
    memcpy(p, &d16, 2); p += 2;
    /* dmin = 0.5 -> f16 = 0x3800 */
    uint16_t dmin16 = 0x3800;
    memcpy(p, &dmin16, 2); p += 2;
    /* scales[12]: all 0x3C */
    for (int i = 0; i < 12; i++) p[i] = 0x3C;
    p += 12;
    /* qh[32]: bit pattern: qh[i] has bit (2*i%8) set etc.
       We use a simple pattern: qh[i] = (i % 4 == 0) ? 0xFF : 0x00 */
    for (int i = 0; i < 32; i++)
        p[i] = (i % 4 == 0) ? 0xFF : 0x00;
    p += 32;
    /* qs[128]: sequential nibbles: qs[i] = (i*2 & 0xF) | ((i*2+1 & 0xF) << 4) */
    for (int i = 0; i < 128; i++) {
        uint8_t lo = (uint8_t)((i * 2) & 0xF);
        uint8_t hi = (uint8_t)((i * 2 + 1) & 0xF);
        p[i] = lo | (hi << 4);
    }
    p += 128;
    /* total = 2+2+12+32+128 = 176 bytes */
}

/* ------------------------------------------------------------------ */
/*  Build a minimal GGUF file with one Q5_K tensor.
 *
 *  GGUF v3 format:
 *    magic "GGUF" (4 bytes)
 *    u32 version (3)
 *    u64 n_tensors (1)
 *    u64 n_kv (1 minimal KV: general.alignment=32)
 *    KV records...
 *    Tensor info records...
 *    Padding to alignment
 *    Tensor data (Q5_K blocks)
 *
 *  Returns path to temp file, or NULL on failure.
 * ------------------------------------------------------------------ */
static const char *TMP_VALID   = "/tmp/test_q5k_valid.gguf";
static const char *TMP_TRUNC_KV   = "/tmp/test_q5k_trunc_kv.gguf";
static const char *TMP_TRUNC_TEN  = "/tmp/test_q5k_trunc_ten.gguf";

/* Write a u32 LE */
static void w_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void w_u64(FILE *f, uint64_t v) { fwrite(&v, 8, 1, f); }

/* Write a GGUF string: u64 len + bytes (no null terminator in file) */
static void w_str(FILE *f, const char *s) {
    uint64_t len = strlen(s);
    w_u64(f, len);
    fwrite(s, 1, (size_t)len, f);
}

static int build_valid_q5k_gguf(void) {
    FILE *f = fopen(TMP_VALID, "wb");
    if (!f) return -1;

    /* Magic */
    fwrite("GGUF", 1, 4, f);
    /* Version 3 */
    w_u32(f, 3);
    /* n_tensors = 1 */
    w_u64(f, 1);
    /* n_kv = 1 (general.alignment) */
    w_u64(f, 1);

    /* KV: general.alignment = uint32(32) */
    w_str(f, "general.alignment");
    w_u32(f, 4); /* GGUF_TYPE_UINT32 */
    w_u32(f, 32);

    /* Tensor info: name, ndim, dims[], ggml_type, data_offset */
    w_str(f, "test.q5k.weight");
    w_u32(f, 1);    /* ndim = 1 */
    w_u64(f, 256);  /* dim[0] = 256 elements */
    w_u32(f, 13);   /* ggml_type = Q5_K */
    /* data_offset: we'll compute after padding */
    /* We need to know the current position to set data_offset correctly.
       The tensor data starts at the next alignment boundary after tensor info.
       Let's write a placeholder and fix it later. */
    long data_offset_pos = ftell(f);
    w_u64(f, 0); /* placeholder */

    /* Now we're at the end of tensor info. Compute aligned data offset. */
    long info_end = ftell(f);
    uint64_t alignment = 32;
    uint64_t data_off = ((uint64_t)info_end + alignment - 1) / alignment * alignment;
    /* The tensor's data_offset in GGUF is relative to the start of the
       data section (which begins at data_off). cce_gguf_load_f32 computes
       abs_off = g->data_offset + m->data_offset, so we store 0 (the first
       tensor starts at the beginning of the data section). */
    fseek(f, data_offset_pos, SEEK_SET);
    w_u64(f, 0); /* relative offset = 0 */
    fseek(f, (long)data_off, SEEK_SET);

    /* Write 1 Q5_K block (176 bytes) */
    uint8_t block[176];
    build_test_q5k_block(block);
    fwrite(block, 1, 176, f);

    fclose(f);
    return 0;
}

static int build_truncated_kv_gguf(void) {
    /* A GGUF file where the KV record is truncated: it declares n_kv=1
       but the value data is cut short (file ends mid-KV-value). */
    FILE *f = fopen(TMP_TRUNC_KV, "wb");
    if (!f) return -1;

    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 0); /* n_tensors = 0 */
    w_u64(f, 1); /* n_kv = 1 */

    /* KV: general.alignment, type=UINT32, but TRUNCATE before the value */
    w_str(f, "general.alignment");
    w_u32(f, 4); /* GGUF_TYPE_UINT32 */
    /* File ends here — the 4-byte u32 value is missing */
    fclose(f);
    return 0;
}

static int build_truncated_tensor_gguf(void) {
    /* A GGUF file where the tensor metadata is truncated: declares
       n_tensors=1 but the tensor name is cut short. */
    FILE *f = fopen(TMP_TRUNC_TEN, "wb");
    if (!f) return -1;

    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 1); /* n_tensors = 1 */
    w_u64(f, 0); /* n_kv = 0 */

    /* Tensor name string: write length but truncate the string body */
    w_u64(f, 30); /* claims 30-byte name */
    fwrite("test.q5k", 1, 8, f); /* only 8 bytes — truncated */
    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 1: Q5_K dequant matches reference decoder
 * ------------------------------------------------------------------ */
static int test_q5k_dequant(void) {
    printf("[TEST 1] Q5_K dequant vs independent reference decoder...\n");
    fflush(stdout);

    if (build_valid_q5k_gguf() != 0) {
        printf("  FAIL: could not build test GGUF\n");
        return 1;
    }

    cce_gguf *g = NULL;
    cce_result rc = cce_gguf_load(TMP_VALID, &g);
    if (rc != CCE_OK) {
        printf("  FAIL: cce_gguf_load returned %d (expected CCE_OK)\n", (int)rc);
        return 1;
    }
    printf("  loaded OK, tensors=%d\n", cce_gguf_tensor_count(g));

    /* Load the Q5_K tensor as f32 via the production dequant */
    float prod[256];
    memset(prod, 0, sizeof(prod));
    rc = cce_gguf_load_f32(g, 0, prod, 256);
    if (rc != CCE_OK) {
        printf("  FAIL: cce_gguf_load_f32 returned %d\n", (int)rc);
        cce_gguf_free(g);
        return 1;
    }

    /* Compute reference values */
    uint8_t block[176];
    build_test_q5k_block(block);
    float ref[256];
    ref_dequant_q5_K(block, ref, 1);

    /* Compare */
    int mismatches = 0;
    for (int i = 0; i < 256; i++) {
        if (fabsf(prod[i] - ref[i]) > 1e-4f * (1.0f + fabsf(ref[i]))) {
            if (mismatches < 10) {
                printf("  MISMATCH at [%d]: prod=%.6f ref=%.6f (diff=%.6f)\n",
                       i, prod[i], ref[i], fabsf(prod[i] - ref[i]));
            }
            mismatches++;
        }
    }

    cce_gguf_free(g);

    if (mismatches > 0) {
        printf("  FAIL: %d mismatches out of 256 values\n", mismatches);
        return 1;
    }
    printf("  PASS: all 256 values match reference within tolerance\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 2: fail-closed on truncated KV metadata
 * ------------------------------------------------------------------ */
static int test_truncated_kv(void) {
    printf("[TEST 2] cce_gguf_load fails closed on truncated KV record...\n");
    fflush(stdout);

    if (build_truncated_kv_gguf() != 0) {
        printf("  FAIL: could not build truncated KV GGUF\n");
        return 1;
    }

    cce_gguf *g = NULL;
    cce_result rc = cce_gguf_load(TMP_TRUNC_KV, &g);
    if (rc == CCE_OK) {
        printf("  FAIL: load succeeded (expected error), rc=%d\n", (int)rc);
        cce_gguf_free(g);
        return 1;
    }
    printf("  PASS: load correctly refused (rc=%d)\n", (int)rc);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 3: fail-closed on truncated tensor metadata
 * ------------------------------------------------------------------ */
static int test_truncated_tensor(void) {
    printf("[TEST 3] cce_gguf_load fails closed on truncated tensor record...\n");
    fflush(stdout);

    if (build_truncated_tensor_gguf() != 0) {
        printf("  FAIL: could not build truncated tensor GGUF\n");
        return 1;
    }

    cce_gguf *g = NULL;
    cce_result rc = cce_gguf_load(TMP_TRUNC_TEN, &g);
    if (rc == CCE_OK) {
        printf("  FAIL: load succeeded (expected error), rc=%d\n", (int)rc);
        cce_gguf_free(g);
        return 1;
    }
    printf("  PASS: load correctly refused (rc=%d)\n", (int)rc);
    return 0;
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("=== CNET 5.1.1 Q5_K / GGUF Integrity Slice ===\n");
    fflush(stdout);

    int failures = 0;
    failures += test_q5k_dequant();
    failures += test_truncated_kv();
    failures += test_truncated_tensor();

    /* Cleanup temp files */
    unlink(TMP_VALID);
    unlink(TMP_TRUNC_KV);
    unlink(TMP_TRUNC_TEN);

    if (failures == 0) {
        printf("\nGGUF_INTEGRITY_PASS\n");
        return 0;
    }
    printf("\nGGUF_INTEGRITY_FAIL (%d test(s) failed)\n", failures);
    return 1;
}