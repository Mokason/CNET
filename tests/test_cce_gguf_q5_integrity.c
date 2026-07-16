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
 *   1b) At least TWO consecutive Q5_K blocks with NONUNIFORM scales/mins/
 *       high bits, matching the independent reference decoder.  The existing
 *       single-block test uses uniform scales (all 0x3C) and a correlated
 *       qh pattern — this variant exercises per-sub-block scale/min unpacking
 *       and qh bit rotation across block boundaries.
 *   2) cce_gguf_load fails closed (returns error, no leak) when a KV
 *      metadata record is truncated/malformed.  Verified under ASan/LSan
 *      for the partially-initialized ARRAY KV record: the current entry's
 *      arr allocation must be freed, not just prior entries.
 *   3) cce_gguf_load fails closed when a tensor metadata record has
 *      ndim==0 or ndim>CCE_MAX_DIMS — rejecting, not clamping/misparsing.
 *   4) cce_gguf_load fails closed on truncated tensor metadata (existing).
 *   5) All tests are CPU-only; no GPU, no real model files.
 *
 * Build (normal):
 *   gcc -std=c11 -Wall -Wextra -pedantic -Werror -O2 -D_DEFAULT_SOURCE \
 *       -I include -o build/test_cce_gguf_q5_integrity \
 *       tests/test_cce_gguf_q5_integrity.c tests/stubs_q5_test.c \
 *       src/cce/cce_gguf.c -lm -lpthread
 *
 * Build (ASan + LSan):
 *   gcc -std=c11 -Wall -Wextra -pedantic -Werror -O1 -g -D_DEFAULT_SOURCE \
 *       -fsanitize=address -fsanitize=leak \
 *       -I include -o build/test_cce_gguf_q5_integrity_asan \
 *       tests/test_cce_gguf_q5_integrity.c tests/stubs_q5_test.c \
 *       src/cce/cce_gguf.c -lm -lpthread
 *
 * Run:  timeout 30 ./build/test_cce_gguf_q5_integrity
 *       timeout 60 ./build/test_cce_gguf_q5_integrity_asan
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
/*  Nonuniform Q5_K test block — exercises ALL code paths:
 *    - Different d and dmin per block
 *    - scales[12] with diverse 6-bit packed values (not all 0x3C)
 *    - qh[32] with non-trivial bit patterns (all 8 bit positions)
 *    - qs[128] with varied nibbles
 *
 *  block_idx selects a deterministic but varied parameter set.
 * ------------------------------------------------------------------ */
static void build_nonuniform_q5k_block(uint8_t *block, int block_idx) {
    uint8_t *p = block;

    /* Vary d and dmin per block: d = 1.0 + 0.5*idx, dmin = 0.25*idx */
    /* f16 encode: we'll just use a few known f16 values */
    /* idx 0: d=1.0 (0x3C00), dmin=0.5 (0x3800) */
    /* idx 1: d=2.0 (0x4000), dmin=0.25 (0x3500) */
    /* idx 2: d=0.5 (0x3800), dmin=1.0 (0x3C00) */
    uint16_t d_tab[]   = { 0x3C00, 0x4000, 0x3800, 0x4200 };
    uint16_t dmin_tab[] = { 0x3800, 0x3500, 0x3C00, 0x3300 };
    int si = block_idx % 4;

    uint16_t d16 = d_tab[si];
    memcpy(p, &d16, 2); p += 2;
    uint16_t dmin16 = dmin_tab[si];
    memcpy(p, &dmin16, 2); p += 2;

    /* Nonuniform scales[12]: each byte is different, exercising both
       the j<4 path (scales[j]&63, scales[j+4]&63) and the j>=4 path
       (scales[j+4]&0xF | scales[j-4]>>6<<4, etc.). */
    /* The 6-bit scale/min values are embedded in a 12-byte array.
       We pick values where the top 2 bits (used as extension bits
       for j>=4 entries) are nonzero. */
    for (int i = 0; i < 12; i++) {
        /* deterministic varied values: ensure top-2-bits vary */
        p[i] = (uint8_t)((block_idx * 17 + i * 23 + 0xA5) & 0xFF);
        /* avoid all-zero which would make scale/min both 0 (trivial) */
        if (p[i] == 0) p[i] = 0x3C;
    }
    p += 12;

    /* qh[32]: non-trivial bit pattern — every bit position exercised.
       qh[i] = a rotating pattern so that u1/u2 bit positions (which
       shift by <<2 per 64-group) all hit nonzero bits. */
    for (int i = 0; i < 32; i++) {
        /* Each byte gets a different combination of bits */
        p[i] = (uint8_t)((0x55 ^ (i * 37) ^ (block_idx * 0xAA)) & 0xFF);
    }
    p += 32;

    /* qs[128]: varied nibbles, not sequential */
    for (int i = 0; i < 128; i++) {
        uint8_t lo = (uint8_t)((i * 3 + block_idx * 7) & 0xF);
        uint8_t hi = (uint8_t)((i * 5 + block_idx * 11) & 0xF);
        p[i] = lo | (hi << 4);
    }
    p += 128;
    /* total = 176 bytes */
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
static const char *TMP_VALID       = "/tmp/test_q5k_valid.gguf";
static const char *TMP_VALID_MULTI = "/tmp/test_q5k_valid_multi.gguf";
static const char *TMP_TRUNC_KV    = "/tmp/test_q5k_trunc_kv.gguf";
static const char *TMP_TRUNC_KV_ARR= "/tmp/test_q5k_trunc_kv_arr.gguf";
static const char *TMP_TRUNC_TEN   = "/tmp/test_q5k_trunc_ten.gguf";
static const char *TMP_BAD_NDIM0   = "/tmp/test_q5k_bad_ndim0.gguf";
static const char *TMP_BAD_NDIM_HI = "/tmp/test_q5k_bad_ndim_hi.gguf";

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

/* Build a GGUF with 2+ consecutive Q5_K blocks with nonuniform params. */
static int build_valid_multi_block_q5k_gguf(int nblocks) {
    FILE *f = fopen(TMP_VALID_MULTI, "wb");
    if (!f) return -1;

    int total_elems = nblocks * 256;

    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 1);   /* n_tensors = 1 */
    w_u64(f, 1);   /* n_kv = 1 */

    w_str(f, "general.alignment");
    w_u32(f, 4);
    w_u32(f, 32);

    w_str(f, "test.q5k.multi.weight");
    w_u32(f, 1);              /* ndim = 1 */
    w_u64(f, total_elems);    /* dim[0] */
    w_u32(f, 13);             /* Q5_K */
    long data_offset_pos = ftell(f);
    w_u64(f, 0); /* placeholder */

    long info_end = ftell(f);
    uint64_t alignment = 32;
    uint64_t data_off = ((uint64_t)info_end + alignment - 1) / alignment * alignment;
    fseek(f, data_offset_pos, SEEK_SET);
    w_u64(f, 0);
    fseek(f, (long)data_off, SEEK_SET);

    for (int b = 0; b < nblocks; b++) {
        uint8_t block[176];
        build_nonuniform_q5k_block(block, b);
        fwrite(block, 1, 176, f);
    }

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

/* Build a GGUF where:
 *   KV[0] = general.alignment (UINT32, valid)
 *   KV[1] = bad.array (ARRAY of UINT32, n=1000, but file truncates
 *                       after only a few elements)
 * The array allocation (calloc for 1000 int64_t = 8000 bytes) is made
 * on the CURRENT KV entry, then fread fails mid-array.  The bug is that
 * the caller only frees j < i, so kv[1].arr leaks.
 *
 * Under ASan/LSan this should report a leak of ~8000 bytes.
 */
static int build_truncated_kv_array_gguf(void) {
    FILE *f = fopen(TMP_TRUNC_KV_ARR, "wb");
    if (!f) return -1;

    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 0);  /* n_tensors = 0 */
    w_u64(f, 2);  /* n_kv = 2 */

    /* KV[0]: general.alignment = UINT32(32) — fully valid */
    w_str(f, "general.alignment");
    w_u32(f, 4);  /* GGUF_TYPE_UINT32 */
    w_u32(f, 32);

    /* KV[1]: bad.array = ARRAY(UINT32, n=1000)
       GGUF array: u32 elem_type, u64 n, then n * elem_size bytes */
    w_str(f, "bad.array");
    w_u32(f, 9);  /* GGUF_TYPE_ARRAY */
    w_u32(f, 4);  /* elem_type = GGUF_TYPE_UINT32 */
    w_u64(f, 1000); /* n = 1000 elements */

    /* Write only 3 of the 1000 u32 elements, then truncate. The reader
       allocates the current entry's array before the fourth read fails;
       cleanup must therefore include the current partially initialized KV. */
    for (int k = 0; k < 3; k++)
        w_u32(f, (uint32_t)(k * 10));
    /* File ends here — 997 elements missing */
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

/* Build a GGUF with a tensor that has ndim=0 — malformed. */
static int build_bad_ndim0_gguf(void) {
    FILE *f = fopen(TMP_BAD_NDIM0, "wb");
    if (!f) return -1;

    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 1);  /* n_tensors = 1 */
    w_u64(f, 1);  /* n_kv = 1 */

    /* KV: general.alignment = UINT32(32) */
    w_str(f, "general.alignment");
    w_u32(f, 4);
    w_u32(f, 32);

    /* Tensor: name, ndim=0 (NO dims follow), ggml_type, data_offset */
    w_str(f, "test.bad.ndim0");
    w_u32(f, 0);  /* ndim = 0 — MALFORMED */
    /* No dim entries follow when ndim=0 */
    w_u32(f, 13);   /* Q5_K */
    w_u64(f, 0);    /* data_offset = 0 */

    fclose(f);
    return 0;
}

/* Build a GGUF with a tensor that has ndim=CCE_MAX_DIMS+1 — malformed. */
static int build_bad_ndim_hi_gguf(void) {
    FILE *f = fopen(TMP_BAD_NDIM_HI, "wb");
    if (!f) return -1;

    fwrite("GGUF", 1, 4, f);
    w_u32(f, 3);
    w_u64(f, 1);  /* n_tensors = 1 */
    w_u64(f, 1);  /* n_kv = 1 */

    w_str(f, "general.alignment");
    w_u32(f, 4);
    w_u32(f, 32);

    /* Tensor: name, ndim=CCE_MAX_DIMS+1, dims, type, offset */
    w_str(f, "test.bad.ndimhi");
    w_u32(f, CCE_MAX_DIMS + 1);  /* ndim = 9 > CCE_MAX_DIMS(8) */
    /* Write CCE_MAX_DIMS+1 dim entries */
    for (int d = 0; d < CCE_MAX_DIMS + 1; d++)
        w_u64(f, 2);  /* each dim = 2 */
    w_u32(f, 13);   /* Q5_K */
    w_u64(f, 0);    /* data_offset = 0 */

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 1: Q5_K dequant matches reference decoder (single block, uniform)
 * ------------------------------------------------------------------ */
static int test_q5k_dequant(void) {
    printf("[TEST 1] Q5_K dequant vs independent reference decoder (uniform)...\n");
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
/*  Test 1b: Q5_K dequant with 2+ consecutive nonuniform blocks
 * ------------------------------------------------------------------ */
static int test_q5k_dequant_multi_block(void) {
    int nblocks = 3;  /* at least 2 consecutive */
    int total = nblocks * 256;
    printf("[TEST 1b] Q5_K dequant vs reference (%d nonuniform blocks)...\n", nblocks);
    fflush(stdout);

    if (build_valid_multi_block_q5k_gguf(nblocks) != 0) {
        printf("  FAIL: could not build multi-block test GGUF\n");
        return 1;
    }

    cce_gguf *g = NULL;
    cce_result rc = cce_gguf_load(TMP_VALID_MULTI, &g);
    if (rc != CCE_OK) {
        printf("  FAIL: cce_gguf_load returned %d (expected CCE_OK)\n", (int)rc);
        return 1;
    }
    printf("  loaded OK, tensors=%d\n", cce_gguf_tensor_count(g));

    float *prod = (float*)calloc(total, sizeof(float));
    if (!prod) { cce_gguf_free(g); return 1; }
    rc = cce_gguf_load_f32(g, 0, prod, total);
    if (rc != CCE_OK) {
        printf("  FAIL: cce_gguf_load_f32 returned %d\n", (int)rc);
        free(prod); cce_gguf_free(g);
        return 1;
    }

    /* Build reference blocks and decode with independent reference */
    uint8_t *blocks = (uint8_t*)malloc((size_t)nblocks * 176);
    if (!blocks) { free(prod); cce_gguf_free(g); return 1; }
    for (int b = 0; b < nblocks; b++)
        build_nonuniform_q5k_block(blocks + (size_t)b * 176, b);

    float *ref = (float*)calloc(total, sizeof(float));
    if (!ref) { free(blocks); free(prod); cce_gguf_free(g); return 1; }
    ref_dequant_q5_K(blocks, ref, nblocks);

    int mismatches = 0;
    for (int i = 0; i < total; i++) {
        if (fabsf(prod[i] - ref[i]) > 1e-4f * (1.0f + fabsf(ref[i]))) {
            if (mismatches < 10) {
                printf("  MISMATCH at [%d]: prod=%.6f ref=%.6f (diff=%.6f)\n",
                       i, prod[i], ref[i], fabsf(prod[i] - ref[i]));
            }
            mismatches++;
        }
    }

    free(prod);
    free(ref);
    free(blocks);
    cce_gguf_free(g);

    if (mismatches > 0) {
        printf("  FAIL: %d mismatches out of %d values\n", mismatches, total);
        return 1;
    }
    printf("  PASS: all %d values match reference within tolerance\n", total);
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
/*  Test 2b: fail-closed on truncated ARRAY KV — leak detection.
 *
 *  This test triggers the partially-initialized KV array leak.  Under
 *  ASan/LSan, the leaked calloc(1000, 8) will be reported at exit.
 *  Without the fix, the current entry's arr is not freed.  With the fix,
 *  gguf_free_kv is called on the current entry too, and no leak occurs.
 *
 *  We also verify the load returns an error (fail-closed).
 * ------------------------------------------------------------------ */
static int test_truncated_kv_array_leak(void) {
    printf("[TEST 2b] cce_gguf_load fails closed on truncated ARRAY KV (leak test)...\n");
    fflush(stdout);

    if (build_truncated_kv_array_gguf() != 0) {
        printf("  FAIL: could not build truncated array KV GGUF\n");
        return 1;
    }

    cce_gguf *g = NULL;
    cce_result rc = cce_gguf_load(TMP_TRUNC_KV_ARR, &g);
    if (rc == CCE_OK) {
        printf("  FAIL: load succeeded (expected error), rc=%d\n", (int)rc);
        cce_gguf_free(g);
        return 1;
    }
    /* g must be NULL — the caller must not receive a partially initialized
       handle (it would have dangling pointers / stale state). */
    if (g != NULL) {
        printf("  FAIL: load returned error but g is non-NULL (should be freed)\n");
        cce_gguf_free(g);
        return 1;
    }
    printf("  PASS: load correctly refused (rc=%d), g=NULL\n", (int)rc);
    printf("  NOTE: leak safety verified by ASan/LSan exit — no leak report = pass\n");
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
    if (g != NULL) {
        printf("  FAIL: load returned error but g is non-NULL\n");
        cce_gguf_free(g);
        return 1;
    }
    printf("  PASS: load correctly refused (rc=%d), g=NULL\n", (int)rc);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 3b: fail-closed on ndim=0 tensor metadata
 *
 *  The current code clamps ndim > CCE_MAX_DIMS but does NOT reject
 *  ndim==0.  With ndim==0, elems = 1 (empty product), which is wrong
 *  for any tensor.  The fix must reject ndim==0 as malformed.
 * ------------------------------------------------------------------ */
static int test_bad_ndim0(void) {
    printf("[TEST 3b] cce_gguf_load fails closed on ndim=0 tensor...\n");
    fflush(stdout);

    if (build_bad_ndim0_gguf() != 0) {
        printf("  FAIL: could not build ndim=0 GGUF\n");
        return 1;
    }

    cce_gguf *g = NULL;
    cce_result rc = cce_gguf_load(TMP_BAD_NDIM0, &g);
    if (rc == CCE_OK) {
        printf("  FAIL: load succeeded (expected error for ndim=0), rc=%d\n", (int)rc);
        cce_gguf_free(g);
        return 1;
    }
    if (g != NULL) {
        printf("  FAIL: load returned error but g is non-NULL\n");
        cce_gguf_free(g);
        return 1;
    }
    printf("  PASS: load correctly refused ndim=0 (rc=%d), g=NULL\n", (int)rc);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Test 3c: fail-closed on ndim>CCE_MAX_DIMS tensor metadata
 *
 *  The current code CLAMPS ndim to CCE_MAX_DIMS (line 417) instead of
 *  rejecting the file as malformed.  Clamping silently drops dimensions
 *  and produces a misshapen tensor — the file is corrupt and must be
 *  refused.  The fix must reject ndim > CCE_MAX_DIMS.
 * ------------------------------------------------------------------ */
static int test_bad_ndim_hi(void) {
    printf("[TEST 3c] cce_gguf_load fails closed on ndim>CCE_MAX_DIMS tensor...\n");
    fflush(stdout);

    if (build_bad_ndim_hi_gguf() != 0) {
        printf("  FAIL: could not build ndim_hi GGUF\n");
        return 1;
    }

    cce_gguf *g = NULL;
    cce_result rc = cce_gguf_load(TMP_BAD_NDIM_HI, &g);
    if (rc == CCE_OK) {
        printf("  FAIL: load succeeded (expected error for ndim>CCE_MAX_DIMS), rc=%d\n", (int)rc);
        /* Check: the clamped tensor would have ndim=CCE_MAX_DIMS */
        cce_gguf_tensor_meta meta;
        if (cce_gguf_get_tensor_meta(g, 0, &meta) == CCE_OK) {
            printf("  BUG: tensor ndim=%d (was clamped from %d)\n",
                   meta.ndim, CCE_MAX_DIMS + 1);
        }
        cce_gguf_free(g);
        return 1;
    }
    if (g != NULL) {
        printf("  FAIL: load returned error but g is non-NULL\n");
        cce_gguf_free(g);
        return 1;
    }
    printf("  PASS: load correctly refused ndim>CCE_MAX_DIMS (rc=%d), g=NULL\n", (int)rc);
    return 0;
}

/* ------------------------------------------------------------------ */
int main(void) {
    printf("=== CNET 5.1.1 Q5_K / GGUF Integrity Slice ===\n");
    fflush(stdout);

    int failures = 0;
    failures += test_q5k_dequant();
    failures += test_q5k_dequant_multi_block();
    failures += test_truncated_kv();
    failures += test_truncated_kv_array_leak();
    failures += test_truncated_tensor();
    failures += test_bad_ndim0();
    failures += test_bad_ndim_hi();

    /* Cleanup temp files */
    unlink(TMP_VALID);
    unlink(TMP_VALID_MULTI);
    unlink(TMP_TRUNC_KV);
    unlink(TMP_TRUNC_KV_ARR);
    unlink(TMP_TRUNC_TEN);
    unlink(TMP_BAD_NDIM0);
    unlink(TMP_BAD_NDIM_HI);

    if (failures == 0) {
        printf("\nGGUF_INTEGRITY_PASS\n");
        return 0;
    }
    printf("\nGGUF_INTEGRITY_FAIL (%d test(s) failed)\n", failures);
    return 1;
}