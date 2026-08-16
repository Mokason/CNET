/* Offline GGUF tensor list + Q1_0/F32 sample (silent, not mouth). */
#include "cce/cce_gguf.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static float fp16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp = (h >> 10) & 0x1f;
    uint32_t mant = h & 0x3ff;
    uint32_t u; float f;
    if (exp == 0) {
        if (mant == 0) u = sign;
        else {
            exp = 1;
            while ((mant & 0x400) == 0) { mant <<= 1; exp--; }
            mant &= 0x3ff;
            u = sign | ((exp + 127 - 15) << 23) | (mant << 13);
        }
    } else if (exp == 31) u = sign | 0x7f800000u | (mant << 13);
    else u = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    memcpy(&f, &u, 4);
    return f;
}

static int dequant_q1_0_head(const uint8_t *raw, size_t nbytes, float *out, int n) {
    const int qk = 128, bs = 18;
    int filled = 0, i, j;
    if (!raw || !out || n <= 0) return -1;
    for (i = 0; filled < n; ++i) {
        const uint8_t *blk = raw + (size_t)i * (size_t)bs;
        uint16_t d16; float d, neg;
        if ((size_t)(i + 1) * (size_t)bs > nbytes) break;
        memcpy(&d16, blk, 2);
        d = fp16_to_f32(d16); neg = -d;
        for (j = 0; j < qk && filled < n; ++j) {
            uint8_t bit = (blk[2 + j / 8] >> (j % 8)) & 1u;
            out[filled++] = bit ? d : neg;
        }
    }
    return filled;
}

int main(int argc, char **argv) {
    const char *path = argc >= 2 ? argv[1] : "/home/marble/AI/Models/Bonsai-8B-gguf/Bonsai-8B.gguf";
    const char *prefer = argc >= 3 ? argv[2] : "attn_q";
    cce_gguf *g = NULL;
    int n, i, printed = 0, sampled = 0;
    if (cce_gguf_load(path, &g) != CCE_OK || !g) {
        printf("CCE_GGUF_PEEK_FAIL load path=%s\n", path);
        return 1;
    }
    n = cce_gguf_tensor_count(g);
    printf("CCE_GGUF_PEEK path=%s\n", path);
    printf("arch=%s n_layer=%d hidden=%d heads=%d kv_heads=%d ctx=%d\n",
           cce_gguf_get_arch(g) ? cce_gguf_get_arch(g) : "?",
           cce_gguf_get_n_layer(g), cce_gguf_get_hidden_size(g),
           cce_gguf_get_n_heads(g), cce_gguf_get_n_kv_heads(g),
           cce_gguf_get_context_length(g));
    printf("n_tensors=%d\n", n);
    for (i = 0; i < n; ++i) {
        cce_gguf_tensor_meta m;
        char shape[128]; int d, pos = 0;
        if (cce_gguf_get_tensor_meta(g, i, &m) != CCE_OK) continue;
        shape[0] = 0;
        for (d = 0; d < m.ndim && d < 8; ++d) {
            int w = snprintf(shape + pos, sizeof shape - (size_t)pos, "%s%d", d ? "x" : "", m.shape[d]);
            if (w > 0) pos += w;
        }
        if (printed < 25) {
            printf("  [%d] %s shape=%s type=%u\n", i, m.name, shape, (unsigned)m.ggml_type);
            printed++;
        }
        if (sampled < 3 && prefer && strstr(m.name, prefer)) {
            const void *raw = NULL; size_t nbytes = 0; float buf[8]; int got, k;
            if (cce_gguf_tensor_bytes(g, i, &raw, &nbytes) != CCE_OK || !raw) continue;
            if (m.ggml_type == 41) {
                got = dequant_q1_0_head((const uint8_t *)raw, nbytes, buf, 8);
                if (got > 0) {
                    printf("  SAMPLE_Q1_0 %s:", m.name);
                    for (k = 0; k < got; ++k) printf(" %.6g", buf[k]);
                    printf("\n");
                    sampled++;
                }
            } else if (m.ggml_type == 0 && nbytes >= 32) {
                const float *f = (const float *)raw;
                printf("  SAMPLE_F32 %s:", m.name);
                for (k = 0; k < 8; ++k) printf(" %.6g", f[k]);
                printf("\n");
                sampled++;
            }
        }
    }
    if (n > 25) printf("  ... (%d more)\n", n - 25);
    /* also sample first F32 norm */
    for (i = 0; i < n && sampled < 5; ++i) {
        cce_gguf_tensor_meta m; const void *raw=NULL; size_t nbytes=0;
        if (cce_gguf_get_tensor_meta(g,i,&m)!=CCE_OK) continue;
        if (m.ggml_type!=0 || !strstr(m.name,"norm")) continue;
        if (cce_gguf_tensor_bytes(g,i,&raw,&nbytes)!=CCE_OK || !raw || nbytes<32) continue;
        { const float *f=(const float*)raw; int k;
          printf("  SAMPLE_F32 %s:", m.name);
          for(k=0;k<8;k++) printf(" %.6g", f[k]);
          printf("\n"); sampled++; }
    }
    cce_gguf_free(g);
    printf("CCE_GGUF_PEEK_OK tensors=%d samples=%d residual_speak=0 mouth=0\n", n, sampled);
    return 0;
}
