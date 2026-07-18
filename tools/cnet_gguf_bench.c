/* Microbench for GGUF token generation. Usage:
 *   cnet_gguf_bench [n_tokens] [path.gguf]
 * With no path: hermetic synthetic tiny qwen2 GGUF (via cce_infer_backend).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_infer_backend.h"

int main(int argc, char** argv) {
    int n = argc > 1 ? atoi(argv[1]) : 128;
    const char* path = argc > 2 ? argv[2] : NULL;
    cce_infer_opts o;
    cce_infer_session* s = NULL;
    double tps = 0;
    cce_result rc;

    if (n < 1) n = 1;

    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    setenv("CNET_INFER_FP", "1", 1);

    cce_infer_opts_default(&o, CCE_INFER_KIND_GGUF, CCE_INFER_DEVICE_CPU);
    if (path && path[0]) {
        o.gguf_path = path;
        o.synthetic = 0;
    } else {
        o.synthetic = 1;
        o.synthetic_path = "gguf_bench_synth.gguf";
    }
    o.dsa_enable = 0;
    o.sparse_kv = 0.0f;
    o.max_ctx = n + 8;

    rc = cce_infer_open(&s, &o);
    if (rc != CCE_OK || !s) {
        fprintf(stderr, "open failed rc=%d\n", (int)rc);
        return 1;
    }
    if (cce_infer_bench(s, n, &tps) != CCE_OK) {
        fprintf(stderr, "bench failed\n");
        cce_infer_close(s);
        return 1;
    }
    printf("GGUF_BENCH kind=%s device=%s layers=%d d_model=%d vocab=%d "
           "tokens=%d tok_s=%.1f path=%s\n",
           cce_infer_kind_name(cce_infer_get_kind(s)),
           cce_infer_device_name(cce_infer_get_device(s)),
           cce_infer_n_layer(s), cce_infer_d_model(s), cce_infer_vocab(s),
           n, tps, path && path[0] ? path : "(synthetic)");
    cce_infer_close(s);
    return 0;
}
