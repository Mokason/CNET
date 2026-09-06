/* Manufacture 27B PQ2_0 → .lut hashtable bricks. Not a mouth. Not CERT admit.
 * make pq2_brick_mint
 *   CNET_GGUF_MMAP=1 ./bin/cnet_pq2_brick_mint --gguf FILE --out DIR
 * Add --layer N to mint only that nonzero attention layer (fused QKV first).
 */
#include "cnet_core_bus.h"
#include "cnet_core_serve.h"
#include "cce/cce_gguf.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *k_default_gguf =
    "/home/marble/AI/Models/Ternary-Bonsai-27B-gguf/"
    "Ternary-Bonsai-27B-PQ2_0.gguf";

static int mint_layer(const char *gguf, const char *out, int layer) {
    cce_gguf *g = NULL;
    CnetCoreBus bus;
    CnetWeightConvertReport report;
    char tensor[96], tag[32], name[64], path[768];
    int rc;
    if (cce_gguf_load(gguf, &g) != CCE_OK || !g) return 1;
    snprintf(tensor, sizeof tensor, "blk.%d.attn_qkv.weight", layer);
    if (cce_gguf_find_tensor(g, tensor) < 0)
        snprintf(tensor, sizeof tensor, "blk.%d.attn_q.weight", layer);
    rc = cce_gguf_find_tensor(g, tensor);
    cce_gguf_free(g);
    if (rc < 0) {
        fprintf(stderr, "cnet_pq2_brick_mint: missing attention layer %d\n", layer);
        return 1;
    }
    snprintf(tag, sizeof tag, "q1_27b%d", layer);
    snprintf(name, sizeof name, "brick_27b_l%d", layer);
    if (snprintf(path, sizeof path, "%s/%s.lut", out, tag) >= (int)sizeof path ||
        access(path, F_OK) == 0) {
        fprintf(stderr, "cnet_pq2_brick_mint: output exists or path too long\n");
        return 1;
    }
    if (mkdir(out, 0755) != 0 && errno != EEXIST) return 1;
    if (snprintf(path, sizeof path, "%s/%s.gguf", out, tag) >= (int)sizeof path)
        return 1;
    if (access(path, F_OK) == 0) return 1;
    cnet_core_bus_init(&bus);
    memset(&report, 0, sizeof report);
    rc = cnet_core_bus_make_brick(&bus, gguf, tensor, path, name, tag, 0,
                                  &report);
    if (rc == 0)
        rc = cnet_serve_save_lut(out, tag, name, bus.bricks[0].lut_table);
    unlink(path);
    if (rc == 0)
        printf("PQ2_BRICK_MINT_OK n=1 tensor=%s tag=%s teacher_unbound=%d "
               "certified=0 out=%s\n", report.host_tensor, tag,
               report.teacher_unbound, out);
    else
        fprintf(stderr, "cnet_pq2_brick_mint: layer %d failed rc=%d\n", layer, rc);
    cnet_core_bus_free(&bus);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *gguf = k_default_gguf;
    const char *out = NULL;
    char tmp[768];
    CnetCoreBus bus;
    CnetWeightConvertReport r[17];
    int i;
    int layer = -1;
    static const struct {
        const char *tensor;
        const char *name;
        const char *tag;
        int mode;
    } spec[17] = {
        {"blk.0.attn_q.weight", "brick_27b_qkv", "q1_27add", 0},
        {"blk.0.attn_k.weight", "brick_27b_k", "q1_27xor", 1},
        {"blk.2.attn_q.weight", "brick_27b_l2", "q1_27b2", 0},
        {"blk.0.ffn_down.weight", "brick_27b_ffn", "q1_27ffn", 1},
        {"blk.4.attn_q.weight", "brick_27b_l3", "q1_27b3", 0},
        {"blk.5.attn_q.weight", "brick_27b_l5", "q1_27b5", 0},
        {"blk.6.attn_q.weight", "brick_27b_l6", "q1_27b6", 0},
        {"blk.1.ffn_down.weight", "brick_27b_f1", "q1_27f1", 1},
        {"blk.3.attn_q.weight", "brick_27b_s3", "q1_27s3", 0},
        {"blk.7.attn_q.weight", "brick_27b_s7", "q1_27s7", 0},
        {"blk.8.attn_q.weight", "brick_27b_l8", "q1_27b8", 0},
        {"blk.2.ffn_down.weight", "brick_27b_f2", "q1_27f2", 1},
        {"blk.9.attn_q.weight", "brick_27b_l9", "q1_27b9", 0},
        {"blk.10.attn_q.weight", "brick_27b_l10", "q1_27b10", 0},
        {"blk.11.attn_q.weight", "brick_27b_s11", "q1_27s11", 0},
        {"blk.12.attn_q.weight", "brick_27b_l12", "q1_27b12", 0},
        {"blk.13.attn_q.weight", "brick_27b_l13", "q1_27b13", 0},
    };

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--gguf") == 0 && i + 1 < argc)
            gguf = argv[++i];
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
            out = argv[++i];
        else if (strcmp(argv[i], "--layer") == 0 && i + 1 < argc) {
            char *end;
            long value;
            errno = 0;
            value = strtol(argv[++i], &end, 10);
            if (errno || end == argv[i] || *end || value <= 0 || value > INT_MAX) {
                fprintf(stderr, "cnet_pq2_brick_mint: --layer requires a positive nonzero integer\n");
                return 2;
            }
            layer = (int)value;
        }
        else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "cnet_pq2_brick_mint --gguf FILE --out DIR [--layer N]\n");
            return 0;
        } else {
            fprintf(stderr, "cnet_pq2_brick_mint: unknown or incomplete option %s\n", argv[i]);
            return 2;
        }
    }
    if (!out || !out[0]) {
        fprintf(stderr, "cnet_pq2_brick_mint: --out DIR required\n");
        return 2;
    }
    if (access(gguf, R_OK) != 0) {
        fprintf(stderr, "cnet_pq2_brick_mint: cannot read %s\n", gguf);
        return 2;
    }
    if (layer > 0) return mint_layer(gguf, out, layer);
    mkdir(out, 0755);
    cnet_core_bus_init(&bus);
    memset(r, 0, sizeof r);
    for (i = 0; i < 17; ++i) {
        snprintf(tmp, sizeof tmp, "%s/%s.gguf", out, spec[i].tag);
        if (cnet_core_bus_make_brick(&bus, gguf, spec[i].tensor, tmp,
                                     spec[i].name, spec[i].tag, spec[i].mode,
                                     &r[i]) != 0) {
            fprintf(stderr, "cnet_pq2_brick_mint: brick %s failed\n",
                    spec[i].tag);
            cnet_core_bus_free(&bus);
            return 1;
        }
        if (cnet_serve_save_lut(out, spec[i].tag, spec[i].name,
                                bus.bricks[i].lut_table) != 0) {
            fprintf(stderr, "cnet_pq2_brick_mint: lut %s failed\n", spec[i].tag);
            cnet_core_bus_free(&bus);
            return 1;
        }
        unlink(tmp);
    }
    printf("PQ2_BRICK_MINT_OK n=17 "
           "t15=%s t16=%s teacher_unbound=%d certified=0 out=%s\n",
           r[15].host_tensor, r[16].host_tensor,
           r[0].teacher_unbound && r[15].teacher_unbound && r[16].teacher_unbound,
           out);
    cnet_core_bus_free(&bus);
    return 0;
}
