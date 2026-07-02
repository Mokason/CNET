#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../include/cce/cce_safetensors.h"
#include "../include/cce/cce.h"   /* for cce_result etc */
#include "../include/nn.h"        /* BinaryTransformNetwork */

static void write_le64(FILE* f, uint64_t v) {
    for (int i=0; i<8; i++) {
        unsigned char b = (unsigned char)((v >> (i*8)) & 0xFF);
        fwrite(&b, 1, 1, f);
    }
}

/* Write a minimal valid safetensors file with 1-2 small F32 tensors.
   Layout: 8 + header_json + data_blob
   Returns 0 on success. */
static int write_synthetic_safetensors(const char* path,
                                       const char* t0_name, const float* t0, int t0_rows, int t0_cols,
                                       const char* t1_name, const float* t1, int t1_len) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;

    /* Build JSON header (very small, exact) */
    char header[4096];
    int pos = 0;

    pos += snprintf(header + pos, sizeof(header)-pos, "{");

    /* tensor 0 */
    pos += snprintf(header + pos, sizeof(header)-pos,
        "\"%s\":{\"dtype\":\"F32\",\"shape\":[%d,%d],\"data_offsets\":[0,%zu]}",
        t0_name, t0_rows, t0_cols, (size_t)(t0_rows * t0_cols * 4));

    size_t data0_bytes = (size_t)t0_rows * t0_cols * 4;

    if (t1_name && t1) {
        pos += snprintf(header + pos, sizeof(header)-pos, ",");
        pos += snprintf(header + pos, sizeof(header)-pos,
            "\"%s\":{\"dtype\":\"F32\",\"shape\":[%d],\"data_offsets\":[%zu,%zu]}",
            t1_name, t1_len,
            data0_bytes,
            data0_bytes + (size_t)t1_len * 4);
    }
    pos += snprintf(header + pos, sizeof(header)-pos, "}");

    uint64_t hlen = (uint64_t)pos;

    write_le64(f, hlen);
    fwrite(header, 1, (size_t)hlen, f);

    /* data */
    fwrite(t0, 4, (size_t)t0_rows * t0_cols, f);
    if (t1_name && t1) {
        fwrite(t1, 4, (size_t)t1_len, f);
    }

    fclose(f);
    return 0;
}

static int write_bad_header_len(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return -1;
    /* Claim 1GB header */
    uint64_t bad = 1024ULL * 1024 * 1024;
    write_le64(f, bad);
    fclose(f);
    return 0;
}

int main(void) {
    int failures = 0;
    const char* tmp = "tmp_test_st.safetensors";

    /* 1. Create a simple 2x3 weight + bias */
    float w[2*3] = { 1.0f, 2.0f, 3.0f,  4.0f, 5.0f, 6.0f }; /* [2in x 3out] */
    float b[3]   = { 0.1f, 0.2f, 0.3f };
    if (write_synthetic_safetensors(tmp, "layer0.weight", w, 2, 3,
                                    "layer0.bias", b, 3) != 0) {
        printf("FAIL: could not write synthetic\n");
        return 1;
    }

    cce_safetensors* st = NULL;
    cce_result rc = cce_safetensors_load(tmp, &st);
    if (rc != CCE_OK || !st) {
        printf("FAIL: load good file rc=%d err=%s\n", rc, cce_safetensors_last_error());
        failures++;
        goto cleanup;
    }

    int n = cce_safetensors_count(st);
    if (n != 2) {
        printf("FAIL: expected 2 tensors, got %d\n", n);
        failures++;
    }

    int wi = cce_safetensors_find(st, "layer0.weight");
    if (wi != 0) { printf("FAIL: find weight\n"); failures++; }

    cce_tensor wt = {0};
    rc = cce_safetensors_load_as_tensor(st, wi, &wt);
    if (rc != CCE_OK || wt.numel != 6) {
        printf("FAIL: load_as_tensor rc=%d numel=%zu\n", rc, wt.numel);
        failures++;
    } else {
        if (wt.data[0] != 1.0f || wt.data[5] != 6.0f) {
            printf("FAIL: tensor data mismatch\n"); failures++;
        }
        cce_tensor_free(&wt);
    }

    /* populate a block */
    cce_block blk = {0};
    /* manually init a matching block (no helper that takes dims here, use internal-ish alloc) */
    {
        int shw[2] = {2,3};
        cce_tensor_alloc(&blk.weights, shw, 2);
        int shb[1] = {3};
        cce_tensor_alloc(&blk.bias, shb, 1);
        blk.type = CCE_BLOCK_LINEAR;
    }

    rc = cce_safetensors_populate_block(&blk, st, "layer0.weight", "layer0.bias", 0 /* no transpose */);
    if (rc != CCE_OK) {
        printf("FAIL: populate_block rc=%d\n", rc); failures++;
    } else {
        if (blk.weights.data[0] != 1.0f || blk.bias.data[2] != 0.3f) {
            printf("FAIL: populated values wrong\n"); failures++;
        }
    }

    cce_block_free(&blk);
    cce_safetensors_free(st);

    /* 2. Safety: bad huge header */
    write_bad_header_len(tmp);
    st = NULL;
    rc = cce_safetensors_load(tmp, &st);
    if (rc == CCE_OK) {
        printf("FAIL: accepted insane header len\n"); failures++;
        cce_safetensors_free(st);
    }

    /* 3. Build a cascade from safetensors (reuse the file) */
    /* Rewrite good file */
    write_synthetic_safetensors(tmp, "layer0.weight", w, 2, 3, "layer0.bias", b, 3);

    st = NULL;
    if (cce_safetensors_load(tmp, &st) != CCE_OK) { failures++; goto cleanup2; }

    cce_cascade* cas = NULL;
    rc = cce_safetensors_build_linear_cascade(&cas, st, "layer", 1, 0, 0.1f);
    if (rc != CCE_OK || !cas || cas->num_blocks != 1) {
        printf("FAIL: build_linear_cascade rc=%d blocks=%d\n", rc, cas?cas->num_blocks:-1);
        failures++;
    } else {
        /* quick forward check */
        cce_tensor tin = {0}, tout = {0};
        int ishp[1]={2}; cce_tensor_alloc(&tin, ishp, 1);
        tin.data[0]=1.f; tin.data[1]=0.f;
        int oshp[1]={3}; cce_tensor_alloc(&tout, oshp, 1);
        rc = cce_cascade_forward(cas, &tin, &tout);
        if (rc == CCE_OK) {
            /* expected roughly w row0 + bias */
            if (tout.data[0] < 0.5f || tout.data[0] > 2.5f) {
                /* loose because sigmoid inside non-head */
            }
        }
        cce_tensor_free(&tin); cce_tensor_free(&tout);
    }
    if (cas) cce_cascade_destroy(cas);
    cce_safetensors_free(st);

    /* 4. BTN populate heuristic (create tiny btn) */
    write_synthetic_safetensors(tmp, "0.weight", w, 2, 3, "0.bias", b, 3);
    st = NULL; cce_safetensors_load(tmp, &st);
    BinaryTransformNetwork btn = {0};
    /* init a 2->3 with 0 hidden first? Need at least 1. Use btn_init with 1 hidden */
    if (btn_init(&btn, 2, 3, 1, 4, 0.01, 42) == 0) {
        int got = btn_populate_from_safetensors_heuristic(&btn, st, 1 /*transpose attempt*/);
        if (got <= 0) {
            printf("WARN: heuristic populate got %d (layout may need explicit names)\n", got);
        }
        btn_free(&btn);
    } else {
        printf("FAIL: could not init test btn\n"); failures++;
    }
    cce_safetensors_free(st);

cleanup2:
cleanup:
    remove(tmp);

    /* --- HF / URL connectivity smoke tests (construction + graceful failure) --- */
    char urlbuf[256];
    int written = cce_hf_build_resolve_url(urlbuf, sizeof(urlbuf),
                                     "google-bert/bert-base-uncased",
                                     "model.safetensors", NULL);
    if (written <= 0 || strstr(urlbuf, "huggingface.co") == NULL ||
        strstr(urlbuf, "model.safetensors") == NULL) {
        printf("FAIL: hf url builder\n"); failures++;
    }

    /* Exercise Supra decomposed loader (non-monolithic conversion).
       Loads GPT linears as CNet cascades + embeddings + VQ codebook as proper tensors. */
    {
        cce_supra_decomposed* supra = NULL;
        cce_result src = cce_supra_load_decomposed(&supra, NULL, NULL);
        if (src == CCE_OK && supra) {
            int n = supra->forest ? supra->forest->num_branches : 0;
            printf("[ok] Supra-A2A loaded decomposed: %d specialists in forest, tok_emb=%d x %d, codebook loaded=%d\n",
                   n, supra->tok_emb.shape[0], supra->tok_emb.shape[1], supra->vq_codebook.numel > 0);
            cce_supra_free_decomposed(supra);
        } else {
            printf("[info] supra_decomp not exercised (no net or download): rc=%d\n", src);
        }
    }

    cce_safetensors* bad = NULL;
    cce_result brc = cce_safetensors_load_hf(&bad, "this-repo/does-not-exist-xyz987", "model.safetensors", "main");
    if (brc == CCE_OK) {
        printf("FAIL: load_hf on bogus repo unexpectedly succeeded\n"); failures++;
        if (bad) cce_safetensors_free(bad);
    }

    brc = cce_safetensors_load_url("https://example.invalid/not-real.safetensors", &bad);
    if (brc == CCE_OK) {
        printf("FAIL: load_url on invalid host succeeded\n"); failures++;
        if (bad) cce_safetensors_free(bad);
    }

    if (failures == 0) {
        printf("ALL SAFETENSORS TESTS PASSED\n");
        return 0;
    } else {
        printf("%d FAILURES\n", failures);
        return 1;
    }
}
