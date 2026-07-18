/* Dual CPU microbench: DS residual host + GGUF token path, side by side.
 * Establishes separate CPU baselines before GPU integration per backend.
 * make dual_cpu_bench → DUAL_CPU_BENCH_PASS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/cce/cce_infer_backend.h"
#include "../include/cce/cce_ds_runtime.h"

static int failures, checks;

static void check(int ok, const char *name) {
    checks++;
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

int main(void) {
    cce_infer_opts ods, ogg;
    cce_infer_session *sds = NULL, *sgg = NULL;
    cce_ds_hparams hp;
    double tps_ds = 0, tps_gg = 0;
    const int N = 64;

    setenv("CNET_FOREST_NO_PERSIST", "1", 1);
    setenv("CNET_INFER_FP", "1", 1);

    printf("== Dual CPU bench: DS residual vs GGUF token gen ==\n");

    /* ---- DS CPU ---- */
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

    cce_infer_opts_default(&ods, CCE_INFER_KIND_DS, CCE_INFER_DEVICE_CPU);
    ods.ds_hp = &hp;
    cce_ds_host_opts_default(&ods.ds_opts, "dual_ds.cce", NULL);
    ods.ds_opts.synthetic = 1;
    ods.ds_opts.max_ctx = 128;
    ods.ds_opts.dsa_enable = 1;
    ods.ds_opts.dsa_fraction = 0.25f;
    ods.ds_opts.cold_autoload = 1;

    check(cce_infer_open(&sds, &ods) == CCE_OK && sds, "DS CPU open");
    if (sds) {
        check(cce_infer_bench(sds, N, &tps_ds) == CCE_OK && tps_ds > 0,
              "DS CPU bench");
        printf("    DS  kind=%s device=%s layers=%d d=%d tok_s=%.1f\n",
               cce_infer_kind_name(cce_infer_get_kind(sds)),
               cce_infer_device_name(cce_infer_get_device(sds)),
               cce_infer_n_layer(sds), cce_infer_d_model(sds), tps_ds);
        cce_infer_close(sds);
        sds = NULL;
    }
    remove("dual_ds.cce");

    /* ---- GGUF CPU (synthetic) ---- */
    cce_infer_opts_default(&ogg, CCE_INFER_KIND_GGUF, CCE_INFER_DEVICE_CPU);
    ogg.synthetic = 1;
    ogg.synthetic_path = "dual_gguf_synth.gguf";
    ogg.dsa_enable = 0;
    ogg.sparse_kv = 0.0f;

    check(cce_infer_open(&sgg, &ogg) == CCE_OK && sgg, "GGUF CPU open");
    if (sgg) {
        check(cce_infer_bench(sgg, N, &tps_gg) == CCE_OK && tps_gg > 0,
              "GGUF CPU bench");
        printf("    GGUF kind=%s device=%s layers=%d d=%d vocab=%d tok_s=%.1f\n",
               cce_infer_kind_name(cce_infer_get_kind(sgg)),
               cce_infer_device_name(cce_infer_get_device(sgg)),
               cce_infer_n_layer(sgg), cce_infer_d_model(sgg),
               cce_infer_vocab(sgg), tps_gg);
        cce_infer_close(sgg);
        sgg = NULL;
    }

    /* DS GPU must refuse cleanly (not yet wired) */
    {
        cce_infer_session *g = NULL;
        cce_infer_opts go;
        cce_result rc;
        cce_infer_opts_default(&go, CCE_INFER_KIND_DS, CCE_INFER_DEVICE_GPU);
        go.ds_hp = &hp;
        cce_ds_host_opts_default(&go.ds_opts, "dual_ds_gpu.cce", NULL);
        go.ds_opts.synthetic = 1;
        rc = cce_infer_open(&g, &go);
        check(rc == CCE_ERR_UNSUPPORTED && !g,
              "DS GPU open → UNSUPPORTED (hook reserved)");
        if (g) cce_infer_close(g);
        remove("dual_ds_gpu.cce");
    }

    printf("DUAL_CPU_BENCH_PASS checks=%d failures=%d "
           "ds_tok_s=%.2f gguf_tok_s=%.2f n=%d\n",
           checks, failures, tps_ds, tps_gg, N);
    return failures ? 1 : 0;
}
