#include "../../include/cce/cce_mtk_host.h"
#include "../../include/cce/cce_clgemm.h"
#include "../../include/cce/cce_hipgemm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int argmax_f(const float *v, int n) {
    int i, b = 0;
    if (n < 1) return 0;
    for (i = 1; i < n; ++i)
        if (v[i] > v[b]) b = i;
    return b;
}

static double wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

/* CNET_GPU=0 → CPU only. unset/auto/1 → try GPU. */
static int want_gpu(void) {
    const char *e = getenv("CNET_GPU");
    if (!e || !e[0] || strcmp(e, "auto") == 0 || strcmp(e, "AUTO") == 0)
        return 1;
    if (e[0] == '0' && e[1] == 0) return 0;
    if (strcmp(e, "off") == 0 || strcmp(e, "cpu") == 0 ||
        strcmp(e, "CPU") == 0)
        return 0;
    return 1;
}

static void attach_gpu(cce_mtk_host *h) {
    const char *be;
    int want_cl = 1, want_hip = 1;
    char cn[128] = {0};

    if (!h || !h->model || !want_gpu()) return;

    be = getenv("CNET_GPU_BACKEND");
    if (be && (strcmp(be, "opencl") == 0 || strcmp(be, "cl") == 0))
        want_hip = 0;
    else if (be && strcmp(be, "hip") == 0)
        want_cl = 0;
    else if (be && strcmp(be, "cpu") == 0)
        return;

    /* OpenCL path prefers int8 specialists (set before load in open()). */
    if (want_hip) {
        h->hip = cce_hipgemm_open(h->gpu_name, sizeof h->gpu_name);
        if (h->hip) {
            cce_gguf_qwen2_set_hipgemm(h->model, h->hip);
            h->gpu_active = 1;
            fprintf(stderr, "[mtk_host] hip GPU: %s ndev=%zu\n", h->gpu_name,
                    cce_hipgemm_device_count(h->hip));
        }
    }
    if (want_cl) {
        h->cl = cce_clgemm_open(NULL, cn, sizeof cn);
        if (h->cl) {
            cce_gguf_qwen2_set_clgemm(h->model, h->cl);
            h->gpu_active = 1;
            if (!h->gpu_name[0])
                snprintf(h->gpu_name, sizeof h->gpu_name, "%s", cn);
            else {
                size_t n = strlen(h->gpu_name);
                snprintf(h->gpu_name + n, sizeof h->gpu_name - n, " + %s", cn);
            }
            fprintf(stderr, "[mtk_host] OpenCL GPU: %s ndev=%zu\n", cn,
                    cce_clgemm_device_count(h->cl));
        }
    }
    if (!h->gpu_active)
        fprintf(stderr, "[mtk_host] GPU requested but none available — CPU\n");
}

static void detach_gpu(cce_mtk_host *h) {
    if (!h) return;
    if (h->model) {
        if (h->cl) cce_gguf_qwen2_set_clgemm(h->model, NULL);
        if (h->hip) cce_gguf_qwen2_set_hipgemm(h->model, NULL);
    }
    if (h->cl) {
        cce_clgemm_close(h->cl);
        h->cl = NULL;
    }
    if (h->hip) {
        cce_hipgemm_close(h->hip);
        h->hip = NULL;
    }
    h->gpu_active = 0;
}

cce_result cce_mtk_host_open(cce_mtk_host **out, const char *gguf_path,
                             const char *routes_path) {
    cce_mtk_host *h;
    cce_result rc;
    const char *rp;
    if (!out || !gguf_path || !gguf_path[0]) return CCE_ERR_INVALID_ARG;
    h = (cce_mtk_host *)calloc(1, sizeof *h);
    if (!h) return CCE_ERR_OOM;

    /* Resource orchestrator first (sets CNET_* for loaders). */
    if (cnet_gov_orchestrate_boot(&h->gov, getenv("CNET_GOV_PROFILE")) !=
        CNET_GOV_OK) {
        CnetGovPolicy p;
        cnet_gov_policy_deploy_defaults(&p);
        (void)cnet_gov_open(&h->gov, &p);
        (void)cnet_gov_apply_compute_env(&h->gov);
    }

    cnet_setenv("CNET_FOREST_NO_PERSIST", "1", 0);

    /* GPU fast path: int8 specialists at load (unless FP forced). */
    if (want_gpu() && !getenv("CNET_INFER_FP") && !getenv("CNET_ORACLE_INT8"))
        cnet_setenv("CNET_ORACLE_INT8", "1", 0);

    rc = cce_gguf_load_model(&h->model, gguf_path);
    if (rc != CCE_OK || !h->model) {
        rc = cce_gguf_load_qwen2(&h->model, gguf_path);
    }
    /* Hybrid runners (qwen35) refuse CNET_SPARSE_KV — clear and retry once. */
    if ((rc != CCE_OK || !h->model) && getenv("CNET_SPARSE_KV")) {
        cnet_unsetenv("CNET_SPARSE_KV");
        cnet_unsetenv("CNET_DSA");
        h->model = NULL;
        rc = cce_gguf_load_model(&h->model, gguf_path);
        if (rc != CCE_OK || !h->model)
            rc = cce_gguf_load_qwen2(&h->model, gguf_path);
    }
    if (rc != CCE_OK || !h->model) {
        cce_mtk_host_close(h);
        return rc != CCE_OK ? rc : CCE_ERR_IO;
    }

    attach_gpu(h);

    if (cce_mtk_open(&h->mtk) != CCE_OK) {
        cce_mtk_host_close(h);
        return CCE_ERR_OOM;
    }
    (void)cce_mtk_bind_gguf(h->mtk, h->model);
    cce_mtk_set_kv_flush(h->mtk, cce_mtk_gguf_kv_flush, h->model);

    rp = routes_path;
    if (!rp || !rp[0]) rp = getenv("CNET_MTK_ROUTES");
    if (rp && rp[0]) {
        snprintf(h->routes_path, sizeof h->routes_path, "%s", rp);
        (void)cce_mtk_router_load(h->mtk, rp);
    }

    if (getenv("CNET_MTK_HOOK") && getenv("CNET_MTK_HOOK")[0] == '1')
        cce_mtk_install_block_hook();

    h->loaded = 1;
    *out = h;
    return CCE_OK;
}

void cce_mtk_host_close(cce_mtk_host *h) {
    if (!h) return;
    if (h->mtk) {
        cce_mtk_close(h->mtk);
        h->mtk = NULL;
    }
    detach_gpu(h);
    if (h->model) {
        cce_gguf_qwen2_free(h->model);
        h->model = NULL;
    }
    cnet_gov_close(&h->gov);
    cce_mtk_uninstall_block_hook();
    free(h);
}

cce_result cce_mtk_host_route_prompt(cce_mtk_host *h, const char *prompt) {
    cce_result rc;
    if (!h || !h->loaded || !h->mtk) return CCE_ERR_INVALID_ARG;
    if (!prompt) return CCE_ERR_INVALID_ARG;
    rc = cce_mtk_router_apply(h->mtk, prompt, 1.f);
    if (rc == CCE_OK)
        snprintf(h->last_skill, sizeof h->last_skill, "%s",
                 h->mtk->skill_name);
    return rc;
}

cce_result cce_mtk_host_apply_skill(cce_mtk_host *h, const char *skill_path,
                                    float scale) {
    cce_result rc;
    if (!h || !h->loaded || !h->mtk || !skill_path) return CCE_ERR_INVALID_ARG;
    rc = cce_mtk_apply_file(h->mtk, skill_path, scale);
    if (rc == CCE_OK)
        snprintf(h->last_skill, sizeof h->last_skill, "%s", skill_path);
    return rc;
}

cce_result cce_mtk_host_revert(cce_mtk_host *h) {
    if (!h || !h->mtk) return CCE_ERR_INVALID_ARG;
    h->last_skill[0] = 0;
    return cce_mtk_revert(h->mtk);
}

cce_result cce_mtk_host_generate(cce_mtk_host *h, const int *prompt,
                                 int n_prompt, int n_new, int *out_tokens,
                                 int *out_n) {
    float *logits = NULL;
    int V, i, t, pos;
    cce_result rc;
    double t0, t1;
    if (!h || !h->loaded || !h->model || !prompt || n_prompt < 1 || !out_tokens)
        return CCE_ERR_INVALID_ARG;
    if (n_new < 0) n_new = 0;
    V = h->model->vocab_size > 0 ? h->model->vocab_size : 1;
    logits = (float *)malloc((size_t)V * sizeof(float));
    if (!logits) return CCE_ERR_OOM;

    h->last_prefill_ms = 0;
    h->last_decode_ms = 0;

    (void)cnet_gov_begin_generate(&h->gov);
    h->model->cur_pos = 0;
    t0 = wall_ms();
    rc = cce_gguf_qwen2_forward(h->model, prompt, n_prompt, logits, V);
    t1 = wall_ms();
    h->last_prefill_ms = t1 - t0;
    if (rc != CCE_OK) {
        free(logits);
        (void)cnet_gov_end_generate(&h->gov);
        return rc;
    }
    for (i = 0; i < n_prompt; ++i) out_tokens[i] = prompt[i];
    pos = n_prompt;
    t = argmax_f(logits, V);
    t0 = wall_ms();
    for (i = 0; i < n_new; ++i) {
        out_tokens[pos++] = t;
        h->gen_tokens++;
        (void)cnet_gov_between_token(&h->gov);
        rc = cce_gguf_qwen2_forward(h->model, &t, 1, logits, V);
        if (rc != CCE_OK) break;
        t = argmax_f(logits, V);
    }
    t1 = wall_ms();
    h->last_decode_ms = t1 - t0;
    if (out_n) *out_n = pos;
    free(logits);
    (void)cnet_gov_end_generate(&h->gov);
    return rc;
}

cce_gguf_qwen2 *cce_mtk_host_model(cce_mtk_host *h) {
    return h ? h->model : NULL;
}
cce_mtk *cce_mtk_host_mtk(cce_mtk_host *h) { return h ? h->mtk : NULL; }

int cce_mtk_host_gpu_active(const cce_mtk_host *h) {
    return h ? h->gpu_active : 0;
}
const char *cce_mtk_host_gpu_name(const cce_mtk_host *h) {
    return h && h->gpu_name[0] ? h->gpu_name : "(none)";
}
double cce_mtk_host_last_prefill_ms(const cce_mtk_host *h) {
    return h ? h->last_prefill_ms : 0;
}
double cce_mtk_host_last_decode_ms(const cce_mtk_host *h) {
    return h ? h->last_decode_ms : 0;
}
