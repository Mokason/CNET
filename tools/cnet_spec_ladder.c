/* Progressive specialist conversion ladder CLI.
 *
 * Usage:
 *   bin/cnet_spec_ladder <model.gguf> [family|schedule] [mode]
 *
 * family: down_proj | gate_proj | ... | schedule (default: schedule)
 * mode: ste|obq|posthoc  (default ste)
 *
 * Env:
 *   CNET_LADDER_FAMILY=schedule|down_proj|...
 *   CNET_LADDER_MODE=ste
 *   CNET_LADDER_CERT=0.56
 *   CNET_LADDER_STEPS=32          lean default — do not inflate lightly
 *   CNET_LADDER_MAX=512           full forest for schedule
 *   CNET_LADDER_EXPORT=path.ldtr
 *   CNET_LADDER_IMPORT=path.ldtr  resume: load prior certified trits
 *   CNET_LADDER_CAL_SEQS=3
 *   CNET_LADDER_N_CALIB=48
 *   CNET_LADDER_N_HOLDOUT=16
 *   CNET_GPU=1  CNET_GPU_BACKEND=opencl
 *   CNET_INFER_FP=1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../include/cce/cce_gguf.h"
#include "../include/cce/cce_clgemm.h"
#include "../include/cce/cce_hipgemm.h"
#include "../include/cce/cce_spec_ladder.h"

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static int count_packed_forest(cce_forest *f) {
    int i, n, c = 0;
    if (!f) return 0;
    n = cce_forest_branch_count(f);
    for (i = 0; i < n; ++i) {
        char name[160];
        cce_cascade *cas;
        if (cce_forest_branch_name(f, i, name, (int)sizeof name) != CCE_OK)
            continue;
        cas = cce_forest_get_resident(f, name);
        if (cas && cas->num_blocks > 0 && cas->blocks[0].w_trit) c++;
    }
    return c;
}

int main(int argc, char **argv) {
    const char *path, *family, *mode_s, *exp, *imp;
    cce_gguf_qwen2 *m = NULL;
    cce_ladder_cfg cfg;
    cce_ladder_report *reps = NULL;
    cce_ladder_bank bank;
    cce_clgemm *cl = NULL;
    cce_hipgemm *hip = NULL;
    char cl_name[160] = {0}, hip_name[160] = {0};
    int max_n = 512, n = 0, i, cert = 0, packed = 0, real_n = 0, skipped = 0;
    float sum_err = 0.f;
    const char *e;
    int use_sched = 0, cal_seqs = 3, cal_len = 16, want_gpu = 1;
    int *cal = NULL;
    double t0, t1;

    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s <model.gguf> [family|schedule] [ste|obq|posthoc]\n",
                argv[0]);
        return 2;
    }
    path = argv[1];
    family = argc > 2 ? argv[2] : getenv("CNET_LADDER_FAMILY");
    if (!family || !family[0]) family = "schedule";
    mode_s = argc > 3 ? argv[3] : getenv("CNET_LADDER_MODE");
    if (!mode_s || !mode_s[0]) mode_s = "ste";
    use_sched = (strcmp(family, "schedule") == 0 || strcmp(family, "all") == 0);

    cce_ladder_cfg_default(&cfg);
    /* Lean STE defaults for long campaigns */
    cfg.mode = cce_ladder_mode_parse(mode_s);
    cfg.ste_steps = 32;
    cfg.n_calib = 48;
    cfg.n_holdout = 16;
    cfg.cert_relerr = 0.56f;
    cfg.pack_trits = 1;

    e = getenv("CNET_LADDER_CERT");
    if (e && e[0]) cfg.cert_relerr = (float)atof(e);
    e = getenv("CNET_LADDER_STEPS");
    if (e && e[0]) cfg.ste_steps = atoi(e);
    e = getenv("CNET_LADDER_MAX");
    if (e && e[0]) max_n = atoi(e);
    e = getenv("CNET_LADDER_CAL_SEQS");
    if (e && e[0]) cal_seqs = atoi(e);
    e = getenv("CNET_LADDER_N_CALIB");
    if (e && e[0]) cfg.n_calib = atoi(e);
    e = getenv("CNET_LADDER_N_HOLDOUT");
    if (e && e[0]) cfg.n_holdout = atoi(e);
    if (max_n < 1) max_n = 1;
    if (max_n > 1024) max_n = 1024;
    if (cal_seqs < 1) cal_seqs = 1;
    if (cfg.n_calib < 8) cfg.n_calib = 8;
    if (cfg.n_holdout < 4) cfg.n_holdout = 4;
    if (cfg.ste_steps < 1) cfg.ste_steps = 1;

    setenv("CNET_FOREST_NO_PERSIST", "1", 0);
    unsetenv("CNET_ORACLE_INT8");
    if (!getenv("CNET_INFER_FP")) setenv("CNET_INFER_FP", "1", 0);
    unsetenv("CNET_SPARSE_KV");
    unsetenv("CNET_DSA");

    e = getenv("CNET_GPU");
    if (e && (e[0] == '0' || strcmp(e, "off") == 0 || strcmp(e, "cpu") == 0))
        want_gpu = 0;
    if (!getenv("CNET_GPU_BACKEND") && want_gpu)
        setenv("CNET_GPU_BACKEND", "opencl", 0);

    exp = getenv("CNET_LADDER_EXPORT");
    imp = getenv("CNET_LADDER_IMPORT");

    printf("== CNET specialist conversion ladder ==\n");
    printf("  model=%s family=%s mode=%s cert=%.3f steps=%d "
           "n_calib=%d holdout=%d max=%d gpu=%s host_threads=%d "
           "spec_parallel=%d\n",
           path, family, mode_s, cfg.cert_relerr, cfg.ste_steps, cfg.n_calib,
           cfg.n_holdout, max_n, want_gpu ? "auto" : "off",
           cce_ladder_nthreads(), cce_ladder_spec_parallel());
    if (imp && imp[0]) printf("  import=%s\n", imp);
    if (exp && exp[0]) printf("  export=%s (checkpoint each stage if schedule)\n", exp);

    t0 = wall_ms();
    if (cce_gguf_load_model(&m, path) != CCE_OK || !m) {
        if (cce_gguf_load_qwen2(&m, path) != CCE_OK || !m) {
            fprintf(stderr, "load failed\n");
            return 1;
        }
    }
    t1 = wall_ms();
    if (!m->forest) {
        fprintf(stderr, "no forest\n");
        cce_gguf_qwen2_free(m);
        return 1;
    }
    printf("  open_ms=%.0f layers=%d branches=%d\n", t1 - t0, m->n_layer,
           cce_forest_branch_count(m->forest));

    if (want_gpu) {
        hip = cce_hipgemm_open(hip_name, sizeof hip_name);
        if (hip) {
            cce_gguf_qwen2_set_hipgemm(m, hip);
            cce_ladder_set_hipgemm(hip);
            printf("  gpu=ON hip %s ndev=%zu\n",
                   hip_name[0] ? hip_name : "?", cce_hipgemm_device_count(hip));
        }
        cl = cce_clgemm_open(NULL, cl_name, sizeof cl_name);
        if (cl) {
            cce_gguf_qwen2_set_clgemm(m, cl);
            cce_ladder_set_clgemm(cl);
            printf("  gpu=ON OpenCL %s ndev=%zu (STE + capture)\n",
                   cl_name[0] ? cl_name : "?", cce_clgemm_device_count(cl));
        }
        if (!cl && !hip)
            printf("  gpu=OFF (no OpenCL/hip — CPU STE)\n");
    }

    if (imp && imp[0]) {
        int nr = cce_ladder_import_certified(m->forest, imp);
        printf("  import %s loaded=%d (resume packs; will skip on re-run)\n",
               imp, nr);
    }

    reps = (cce_ladder_report *)calloc((size_t)max_n, sizeof *reps);
    if (!reps) {
        cce_gguf_qwen2_free(m);
        return 1;
    }

    cce_ladder_bank_init(&bank, 512, 512);
    cal = (int *)malloc((size_t)cal_seqs * cal_len * sizeof(int));
    if (cal) {
        int V = m->vocab_size > 1 ? m->vocab_size : 2;
        uint64_t rng = 0xC0FFEEu;
        int s, t;
        for (s = 0; s < cal_seqs; ++s)
            for (t = 0; t < cal_len; ++t) {
                rng = rng * 6364136223846793005ULL + 1;
                cal[s * cal_len + t] = (int)((rng >> 33) % (uint64_t)V);
            }
        t0 = wall_ms();
        cce_ladder_capture_model(m, &bank, cal, cal_seqs, cal_len);
        t1 = wall_ms();
        printf("  capture_ms=%.0f specs=%d\n", t1 - t0, bank.n_specs);
        free(cal);
    }

    t0 = wall_ms();
    if (use_sched) {
        /* One process, all families; checkpoint export after each stage */
        n = cce_ladder_run_schedule(m, &bank, NULL, 0, &cfg, reps, max_n,
                                    exp);
    } else {
        n = cce_ladder_convert_forest_bank(m->forest, family, &bank, &cfg, reps,
                                           max_n);
        if (n == 0 && strcmp(family, "down_proj") == 0)
            n = cce_ladder_convert_forest_bank(m->forest, "ffn_down", &bank,
                                               &cfg, reps, max_n);
    }
    t1 = wall_ms();
    printf("  convert_ms=%.0f converted=%d\n", t1 - t0, n);

    for (i = 0; i < n; ++i) {
        int is_skip = (reps[i].packed && reps[i].relerr_final == 0.f &&
                       !reps[i].used_real_acts && reps[i].certified);
        printf("  [%d] %s in=%d out=%d cert=%d packed=%d real=%d "
               "relerr=%.4f (posthoc=%.4f) ste=%.4g->%.4g%s\n",
               i, reps[i].name, reps[i].in_dim, reps[i].out_dim,
               reps[i].certified, reps[i].packed, reps[i].used_real_acts,
               reps[i].relerr_final, reps[i].relerr_posthoc,
               (double)reps[i].ste_loss0, (double)reps[i].ste_loss1,
               is_skip ? " [skip]" : "");
        if (reps[i].certified) {
            cert++;
            if (reps[i].relerr_final > 0.f)
                sum_err += reps[i].relerr_final;
        }
        if (reps[i].packed) packed++;
        if (reps[i].used_real_acts) real_n++;
        if (is_skip) skipped++;
    }
    {
        int forest_packed = count_packed_forest(m->forest);
        int cert_n = 0;
        float mean = 0.f;
        for (i = 0; i < n; ++i)
            if (reps[i].certified && reps[i].relerr_final > 0.f) {
                mean += reps[i].relerr_final;
                cert_n++;
            }
        printf("LADDER_SUMMARY family=%s mode=%s attempted=%d certified=%d "
               "packed=%d skipped=%d real_act=%d forest_packed=%d "
               "mean_cert_relerr=%.4f\n",
               family, mode_s, n, cert, packed, skipped, real_n, forest_packed,
               cert_n ? mean / (float)cert_n : 0.f);
    }

    /* Final export (schedule already checkpointed; still write once more) */
    if (exp && exp[0]) {
        int nw = cce_ladder_export_certified(m->forest, exp);
        printf("  export %s written=%d\n", exp, nw);
    }

    {
        unsigned long gok = 0, gfail = 0;
        cce_ladder_gpu_stats(&gok, &gfail);
        printf("  gpu_matmul ok=%lu fail=%lu (fail>0 means CPU fallback)\n", gok,
               gfail);
    }
    printf("LADDER_%s\n",
           (n > 0 && cert > 0) ? "PASS" : (n > 0 ? "SOFT" : "FAIL"));
    cce_ladder_bank_free(&bank);
    free(reps);
    cce_ladder_set_clgemm(NULL);
    cce_ladder_set_hipgemm(NULL);
    if (m) {
        cce_gguf_qwen2_set_clgemm(m, NULL);
        cce_gguf_qwen2_set_hipgemm(m, NULL);
    }
    if (cl) cce_clgemm_close(cl);
    if (hip) cce_hipgemm_close(hip);
    cce_gguf_qwen2_free(m);
    return (n > 0) ? 0 : 1;
}
