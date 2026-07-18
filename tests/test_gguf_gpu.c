/* GGUF GPU integration gate (OpenCL / cce_clgemm — primary AMD path).
 *
 * Pure-C path on discrete AMD (dual R9700 on this host). Soft-skips when no
 * OpenCL GPU. When present:
 *   (a) CPU vs GPU argmax agreement 100% on synthetic tiny qwen2
 *   (b) max |Δlogit| reported
 *   (c) both wall times reported (tiny models may be PCIe-bound — identity is the gate)
 *
 * make gguf_gpu → GGUF_GPU_PASS or GGUF_GPU_SKIP
 */
#define TL_CTX 128
#include "tiny_model_fixture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_clgemm.h"
#include "../include/cce/cce_infer_backend.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static double wall_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int argmax_v(const float *v, int n) {
    int i, am = 0;
    for (i = 1; i < n; i++)
        if (v[i] > v[am]) am = i;
    return am;
}

static double dmax_abs(const float *a, const float *b, int n) {
    double m = 0;
    int i;
    for (i = 0; i < n; i++) {
        double d = fabs((double)a[i] - (double)b[i]);
        if (d > m) m = d;
    }
    return m;
}

int main(void) {
    static tl_weights w;
    tl_entry ents[64];
    int n_ents;
    const char *path = "gguf_gpu_fixture.gguf";
    cce_gguf_qwen2 *mc = NULL, *mg = NULL;
    cce_clgemm *gpu = NULL;
    char dev[160] = {0};
    float lc[TL_V], lg[TL_V];
    static const int toks[8] = { 3, 7, 11, 5, 9, 13, 2, 17 };
    const int n_steps = 8;
    int i, n_cmp = 0, n_agree = 0;
    double max_dl = 0, t_cpu = 0, t_gpu = 0;
    cce_infer_session *sess = NULL;
    cce_infer_opts opts;

    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    setenv("CNET_INFER_FP", "1", 1);

    printf("== CNET GGUF GPU (OpenCL primary, pure C) ==\n");
    printf("  plan: plans/amd_gpu_backend.md (Vulkan deferred; hipBLAS later)\n");

    tl_gen(&w, 0);
    n_ents = tl_entries(&w, ents, 0);
    tl_write_gguf(path, ents, n_ents);

    gpu = cce_clgemm_open(NULL, dev, sizeof dev);
    if (!gpu) {
        printf("GGUF_GPU_SKIP no discrete OpenCL GPU (CPU path remains default)\n");
        remove(path);
        return 0;
    }
    printf("  OpenCL device: %s (n_dev=%zu)\n",
           dev[0] ? dev : "?", cce_clgemm_device_count(gpu));
    check(cce_clgemm_device_count(gpu) >= 1, "≥1 discrete GPU opened");

    check(cce_gguf_load_qwen2(&mc, path) == CCE_OK && mc, "CPU model load");
    check(cce_gguf_load_qwen2(&mg, path) == CCE_OK && mg, "GPU model load");
    if (!mc || !mg) {
        if (mc) cce_gguf_qwen2_free(mc);
        if (mg) cce_gguf_qwen2_free(mg);
        cce_clgemm_close(gpu);
        remove(path);
        return 1;
    }
    cce_gguf_qwen2_set_clgemm(mg, gpu);

    mc->cur_pos = 0;
    mg->cur_pos = 0;

    /* prefill 2 tokens */
    {
        double t0 = wall_s();
        check(cce_gguf_qwen2_forward(mc, toks, 2, lc, TL_V) == CCE_OK, "CPU prefill");
        t_cpu += wall_s() - t0;
        t0 = wall_s();
        check(cce_gguf_qwen2_forward(mg, toks, 2, lg, TL_V) == CCE_OK, "GPU prefill");
        t_gpu += wall_s() - t0;
        n_cmp++;
        if (argmax_v(lc, TL_V) == argmax_v(lg, TL_V)) n_agree++;
        {
            double d = dmax_abs(lc, lg, TL_V);
            if (d > max_dl) max_dl = d;
        }
    }

    /* teacher-forced decode steps 2..7 */
    for (i = 2; i < n_steps; i++) {
        double t0 = wall_s();
        if (cce_gguf_qwen2_forward(mc, &toks[i], 1, lc, TL_V) != CCE_OK) {
            check(0, "CPU decode");
            break;
        }
        t_cpu += wall_s() - t0;
        t0 = wall_s();
        if (cce_gguf_qwen2_forward(mg, &toks[i], 1, lg, TL_V) != CCE_OK) {
            check(0, "GPU decode");
            break;
        }
        t_gpu += wall_s() - t0;
        n_cmp++;
        if (argmax_v(lc, TL_V) == argmax_v(lg, TL_V)) n_agree++;
        {
            double d = dmax_abs(lc, lg, TL_V);
            if (d > max_dl) max_dl = d;
        }
    }

    check(n_cmp == n_steps - 1, "all compare steps ran");
    check(n_agree == n_cmp, "argmax agreement 100% vs CPU");
    printf("    max|Δlogit|=%.6g  argmax_agree=%d/%d\n", max_dl, n_agree, n_cmp);
    printf("    CPU wall=%.4fs  GPU wall=%.4fs  (tiny d=%d: PCIe may dominate GPU)\n",
           t_cpu, t_gpu, TL_D);
    if (t_cpu > 1e-9 && t_gpu > 1e-9)
        printf("    CPU ~%.0f tok/s  GPU ~%.0f tok/s over %d tokens\n",
               (double)n_steps / t_cpu, (double)n_steps / t_gpu, n_steps);

    /* infer_backend GPU open + short bench */
    cce_infer_opts_default(&opts, CCE_INFER_KIND_GGUF, CCE_INFER_DEVICE_GPU);
    opts.gguf_path = path;
    opts.synthetic = 0;
    opts.dsa_enable = 0;
    opts.sparse_kv = 0.0f;
    check(cce_infer_open(&sess, &opts) == CCE_OK && sess,
          "infer_backend GGUF GPU open");
    if (sess) {
        double tps = 0;
        check(cce_infer_bench(sess, 16, &tps) == CCE_OK && tps > 0,
              "infer_backend GGUF GPU bench");
        printf("    infer_backend GPU tok_s=%.1f kind=%s device=%s\n",
               tps, cce_infer_kind_name(cce_infer_get_kind(sess)),
               cce_infer_device_name(cce_infer_get_device(sess)));
        cce_infer_close(sess);
    }

    cce_gguf_qwen2_set_clgemm(mg, NULL);
    cce_gguf_qwen2_free(mc);
    cce_gguf_qwen2_free(mg);
    cce_clgemm_close(gpu);
    remove(path);

    printf("GGUF_GPU_PASS checks=%d failures=%d max_dl=%.6g agree=%d/%d\n",
           checks, failures, max_dl, n_agree, n_cmp);
    return failures ? 1 : 0;
}
