/* CNET-native GGUF token gen bench: CPU vs GPU (hipBLAS + OpenCL).
 * No llama.cpp. Usage:
 *   cnet_gguf_gpu_bench <model.gguf> [n_tokens] [backend]
 * backend: auto|hip|opencl|cpu  (default auto)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_clgemm.h"
#include "../include/cce/cce_hipgemm.h"
#include "../include/cce/cce_detect.h"

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int run_tokens(cce_gguf_qwen2 *m, int n, float *lg, double *out_tps) {
    int *toks, t;
    double t0, t1, dt;
    if (!m || n < 1 || !lg) return -1;
    toks = (int *)malloc((size_t)n * sizeof(int));
    if (!toks) return -1;
    for (t = 0; t < n; ++t)
        toks[t] = 100 + (t * 37 + 11) % 900;
    m->cur_pos = 0;
    t0 = wall_s();
    if (cce_gguf_qwen2_forward(m, &toks[0], 1, lg, m->vocab_size) != CCE_OK) {
        free(toks);
        return -1;
    }
    for (t = 1; t < n; ++t) {
        if (m->cur_pos >= m->max_ctx) m->cur_pos = 0;
        if (cce_gguf_qwen2_forward(m, &toks[t], 1, lg, m->vocab_size) !=
            CCE_OK) {
            free(toks);
            return -1;
        }
    }
    t1 = wall_s();
    free(toks);
    dt = t1 - t0;
    if (dt < 1e-9) dt = 1e-9;
    if (out_tps) *out_tps = (double)n / dt;
    return 0;
}

int main(int argc, char **argv) {
    const char *path, *be = "auto";
    int n = 32;
    cce_anymodel *am = NULL;
    cce_gguf_qwen2 *m = NULL;
    cce_clgemm *cl = NULL;
    cce_hipgemm *hip = NULL;
    float *lg = NULL;
    double tps_cpu = 0, tps_gpu = 0;
    char dname[160] = {0};
    int want_hip = 1, want_cl = 1, do_gpu = 1;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <model.gguf> [n_tokens] [auto|hip|opencl|cpu]\n",
                argv[0]);
        return 2;
    }
    path = argv[1];
    if (argc > 2) n = atoi(argv[2]);
    if (argc > 3) be = argv[3];
    if (n < 1) n = 1;

    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    /* Default: keep int8 specialists for OpenCL bandwidth (fast path).
       CNET_INFER_FP=1 forces FP (needed for pure hipBLAS coverage). */
    if (!getenv("CNET_INFER_FP") && !getenv("CNET_ORACLE_INT8"))
        setenv("CNET_ORACLE_INT8", "1", 0);

    if (strcmp(be, "cpu") == 0) {
        do_gpu = 0;
    } else if (strcmp(be, "hip") == 0) {
        want_cl = 0;
        setenv("CNET_GPU_BACKEND", "hip", 1);
    } else if (strcmp(be, "opencl") == 0 || strcmp(be, "cl") == 0) {
        want_hip = 0;
        setenv("CNET_GPU_BACKEND", "opencl", 1);
    }

    printf("== CNET GGUF GPU bench (no llama.cpp) ==\n");
    printf("  model=%s tokens=%d backend=%s\n", path, n, be);

    /* Architecture-detecting CNET loader (gemma / qwen35 / llama / …). */
    if (cce_anymodel_open(&am, path) != CCE_OK || !am || !am->transformer) {
        fprintf(stderr, "load failed (cce_anymodel_open): %s\n", path);
        return 1;
    }
    m = am->transformer;
    printf("  layers=%d d_model=%d vocab=%d max_ctx=%d\n", m->n_layer,
           m->n_embd, m->vocab_size, m->max_ctx);
    lg = (float *)calloc((size_t)m->vocab_size, sizeof(float));
    if (!lg) {
        cce_anymodel_free(am);
        return 1;
    }

    /* CPU */
    if (run_tokens(m, n, lg, &tps_cpu) != 0) {
        fprintf(stderr, "CPU forward failed\n");
        free(lg);
        cce_anymodel_free(am);
        return 1;
    }
    printf("  CPU   tok_s=%.2f\n", tps_cpu);

    if (do_gpu) {
        if (want_hip) {
            hip = cce_hipgemm_open(dname, sizeof dname);
            if (hip) {
                cce_gguf_qwen2_set_hipgemm(m, hip);
                printf("  hip   %s ndev=%zu\n", dname,
                       cce_hipgemm_device_count(hip));
            } else {
                printf("  hip   unavailable\n");
            }
        }
        if (want_cl) {
            char cn[128] = {0};
            cl = cce_clgemm_open(NULL, cn, sizeof cn);
            if (cl) {
                cce_gguf_qwen2_set_clgemm(m, cl);
                printf("  cl    %s ndev=%zu\n", cn, cce_clgemm_device_count(cl));
            } else {
                printf("  cl    unavailable\n");
            }
        }
        if (!hip && !cl) {
            printf("GGUF_GPU_BENCH_PASS cpu_tok_s=%.2f gpu=SKIP\n", tps_cpu);
            free(lg);
            cce_anymodel_free(am);
            return 0;
        }
        /* warm */
        (void)run_tokens(m, n > 4 ? 4 : n, lg, NULL);
        if (run_tokens(m, n, lg, &tps_gpu) != 0) {
            fprintf(stderr, "GPU forward failed\n");
            if (hip) {
                cce_gguf_qwen2_set_hipgemm(m, NULL);
                cce_hipgemm_close(hip);
            }
            if (cl) {
                cce_gguf_qwen2_set_clgemm(m, NULL);
                cce_clgemm_close(cl);
            }
            free(lg);
            cce_anymodel_free(am);
            return 1;
        }
        printf("  GPU   tok_s=%.2f  speedup=%.2fx  hip_res=%.1fMB cl_res=%.1fMB\n",
               tps_gpu, tps_cpu > 0 ? tps_gpu / tps_cpu : 0.0,
               hip ? (double)cce_hipgemm_resident_bytes(hip) / (1024 * 1024)
                   : 0.0,
               cl ? (double)cce_clgemm_resident_bytes(cl) / (1024 * 1024)
                  : 0.0);
        if (hip) {
            cce_gguf_qwen2_set_hipgemm(m, NULL);
            cce_hipgemm_close(hip);
        }
        if (cl) {
            cce_gguf_qwen2_set_clgemm(m, NULL);
            cce_clgemm_close(cl);
        }
    }

    free(lg);
    cce_anymodel_free(am);
    printf("GGUF_GPU_BENCH_PASS cpu_tok_s=%.2f gpu_tok_s=%.2f n=%d path=%s\n",
           tps_cpu, tps_gpu, n, path);
    return 0;
}
