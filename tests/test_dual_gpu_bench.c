/* Dual GPU bench scaffold: GGUF OpenCL vs CPU; DS CPU; DS GPU reserved.
 * Soft-skips GPU section when no discrete OpenCL device.
 * make dual_gpu_bench → DUAL_GPU_BENCH_PASS or DUAL_GPU_BENCH_SKIP
 */
#include <stdio.h>
#include "../include/cnet_platform.h"
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_infer_backend.h"
#include "../include/cce/cce_ds_runtime.h"
#include "../include/cce/cce_clgemm.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    cce_infer_opts o;
    cce_infer_session *s = NULL;
    cce_ds_hparams hp;
    cce_clgemm *probe = NULL;
    char dev[160] = {0};
    double tps_cpu = 0, tps_gpu = 0, tps_ds = 0;
    const int N = 32;
    int have_gpu = 0;

    cnet_setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    cnet_setenv("CNET_INFER_FP", "1", 1);

    printf("== Dual GPU bench scaffold (AMD OpenCL primary) ==\n");
    printf("  plan: plans/amd_gpu_backend.md\n");

    probe = cce_clgemm_open(NULL, dev, sizeof dev);
    have_gpu = probe != NULL;
    if (have_gpu) {
        printf("  GPU present: %s (n_dev=%zu)\n",
               dev[0] ? dev : "?", cce_clgemm_device_count(probe));
        cce_clgemm_close(probe);
        probe = NULL;
    } else {
        printf("  GPU absent — GGUF GPU section SKIP; DS CPU still runs\n");
    }

    /* ---- DS CPU (always) ---- */
    cce_ds_hparams_default_small(&hp);
    hp.n_layer = 2;
    hp.d_model = 128;
    hp.n_heads = 4;
    hp.qk_nope_head_dim = 16;
    hp.qk_rope_head_dim = 8;
    hp.v_head_dim = 16;
    hp.kv_lora_rank = 32;
    hp.n_expert = 4;
    hp.n_expert_used = 2;
    hp.n_ff_exp = 64;
    hp.vocab = 256;

    cce_infer_opts_default(&o, CCE_INFER_KIND_DS, CCE_INFER_DEVICE_CPU);
    o.ds_hp = &hp;
    cce_ds_host_opts_default(&o.ds_opts, "dual_gpu_ds.cce", NULL);
    o.ds_opts.synthetic = 1;
    o.ds_opts.max_ctx = 64;
    o.ds_opts.dsa_enable = 1;
    o.ds_opts.dsa_fraction = 0.25f;
    o.ds_opts.cold_autoload = 1;
    check(cce_infer_open(&s, &o) == CCE_OK && s, "DS CPU open");
    if (s) {
        check(cce_infer_bench(s, N, &tps_ds) == CCE_OK && tps_ds > 0, "DS CPU bench");
        printf("    DS  cpu tok_s=%.1f layers=%d d=%d\n",
               tps_ds, cce_infer_n_layer(s), cce_infer_d_model(s));
        cce_infer_close(s);
        s = NULL;
    }
    remove("dual_gpu_ds.cce");

    /* DS GPU still reserved (OpenCL MLA wire is phase D) */
    {
        cce_result rc;
        cce_infer_opts_default(&o, CCE_INFER_KIND_DS, CCE_INFER_DEVICE_GPU);
        o.ds_hp = &hp;
        cce_ds_host_opts_default(&o.ds_opts, "dual_gpu_ds_gpu.cce", NULL);
        o.ds_opts.synthetic = 1;
        rc = cce_infer_open(&s, &o);
        check(rc == CCE_ERR_UNSUPPORTED && !s,
              "DS GPU open → UNSUPPORTED until MLA clgemm wire");
        if (s) cce_infer_close(s);
        s = NULL;
        remove("dual_gpu_ds_gpu.cce");
    }

    /* ---- GGUF CPU ---- */
    cce_infer_opts_default(&o, CCE_INFER_KIND_GGUF, CCE_INFER_DEVICE_CPU);
    o.synthetic = 1;
    o.synthetic_path = "dual_gpu_gguf.gguf";
    o.dsa_enable = 0;
    check(cce_infer_open(&s, &o) == CCE_OK && s, "GGUF CPU open");
    if (s) {
        check(cce_infer_bench(s, N, &tps_cpu) == CCE_OK && tps_cpu > 0,
              "GGUF CPU bench");
        printf("    GGUF cpu tok_s=%.1f layers=%d d=%d\n",
               tps_cpu, cce_infer_n_layer(s), cce_infer_d_model(s));
        cce_infer_close(s);
        s = NULL;
    }

    /* ---- GGUF GPU (OpenCL) ---- */
    if (have_gpu) {
        cce_infer_opts_default(&o, CCE_INFER_KIND_GGUF, CCE_INFER_DEVICE_GPU);
        o.synthetic = 1;
        o.synthetic_path = "dual_gpu_gguf_gpu.gguf";
        o.dsa_enable = 0;
        check(cce_infer_open(&s, &o) == CCE_OK && s, "GGUF GPU open");
        if (s) {
            check(cce_infer_bench(s, N, &tps_gpu) == CCE_OK && tps_gpu > 0,
                  "GGUF GPU bench");
            printf("    GGUF gpu tok_s=%.1f (OpenCL; tiny model PCIe-bound)\n",
                   tps_gpu);
            cce_infer_close(s);
            s = NULL;
        }
    } else {
        printf("  GGUF GPU section skipped (no device)\n");
    }

    if (!have_gpu) {
        printf("DUAL_GPU_BENCH_SKIP checks=%d failures=%d "
               "ds_cpu=%.1f gguf_cpu=%.1f (no GPU)\n",
               checks, failures, tps_ds, tps_cpu);
        return failures ? 1 : 0;
    }

    printf("DUAL_GPU_BENCH_PASS checks=%d failures=%d "
           "ds_cpu=%.1f gguf_cpu=%.1f gguf_gpu=%.1f n=%d\n",
           checks, failures, tps_ds, tps_cpu, tps_gpu, N);
    return failures ? 1 : 0;
}
