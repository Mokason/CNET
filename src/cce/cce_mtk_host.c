#include "../../include/cce/cce_mtk_host.h"
#include "../../include/cnet_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int argmax_f(const float *v, int n) {
    int i, b = 0;
    if (n < 1) return 0;
    for (i = 1; i < n; ++i)
        if (v[i] > v[b]) b = i;
    return b;
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
        /* Soft: open with defaults if orchestrator fails */
        CnetGovPolicy p;
        cnet_gov_policy_deploy_defaults(&p);
        (void)cnet_gov_open(&h->gov, &p);
        (void)cnet_gov_apply_compute_env(&h->gov);
    }

    cnet_setenv("CNET_FOREST_NO_PERSIST", "1", 0);

    rc = cce_gguf_load_model(&h->model, gguf_path);
    if (rc != CCE_OK || !h->model) {
        /* try qwen2 path */
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
    if (!h || !h->loaded || !h->model || !prompt || n_prompt < 1 || !out_tokens)
        return CCE_ERR_INVALID_ARG;
    if (n_new < 0) n_new = 0;
    V = h->model->vocab_size > 0 ? h->model->vocab_size : 1;
    logits = (float *)malloc((size_t)V * sizeof(float));
    if (!logits) return CCE_ERR_OOM;

    (void)cnet_gov_begin_generate(&h->gov);
    h->model->cur_pos = 0;
    rc = cce_gguf_qwen2_forward(h->model, prompt, n_prompt, logits, V);
    if (rc != CCE_OK) {
        free(logits);
        (void)cnet_gov_end_generate(&h->gov);
        return rc;
    }
    for (i = 0; i < n_prompt; ++i) out_tokens[i] = prompt[i];
    pos = n_prompt;
    t = argmax_f(logits, V);
    for (i = 0; i < n_new; ++i) {
        out_tokens[pos++] = t;
        h->gen_tokens++;
        (void)cnet_gov_between_token(&h->gov);
        rc = cce_gguf_qwen2_forward(h->model, &t, 1, logits, V);
        if (rc != CCE_OK) break;
        t = argmax_f(logits, V);
    }
    if (out_n) *out_n = pos;
    free(logits);
    (void)cnet_gov_end_generate(&h->gov);
    return rc;
}

cce_gguf_qwen2 *cce_mtk_host_model(cce_mtk_host *h) {
    return h ? h->model : NULL;
}
cce_mtk *cce_mtk_host_mtk(cce_mtk_host *h) { return h ? h->mtk : NULL; }
