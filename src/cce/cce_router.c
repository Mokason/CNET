#include "../../include/cce/cce_router.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define CCE_ROUTER_EPS 1.0e-6f
#define CCE_ROUTER_NARRATIVE_BONUS 1.0f

static float cce_router_clamp01(float x) {
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static float cce_router_branch_logit(cce_forest* forest,
                                     const float* input,
                                     int dim,
                                     int branch_idx) {
    cce_branch* br = &forest->branches[branch_idx];
    float sim = 0.0f;
    int cdim = (br->centroid_dim < dim) ? br->centroid_dim : dim;
    for (int d = 0; d < cdim; ++d) {
        float diff = input[d] - br->centroid[d];
        sim -= diff * diff;
    }
    float goodness = (br->cascade && br->cascade->goodness > 0.0f)
        ? br->cascade->goodness
        : 0.5f;
    return sim * 0.7f + goodness * 0.3f;
}

static cce_result cce_router_compute_logits(cce_forest* forest,
                                            const float* input,
                                            int dim,
                                            float** out_logits) {
    if (!forest || !input || !out_logits || forest->num_branches <= 0 || dim <= 0)
        return CCE_ERR_INVALID_ARG;

    float* logits = (float*)malloc((size_t)forest->num_branches * sizeof(float));
    if (!logits) return CCE_ERR_OOM;

    for (int i = 0; i < forest->num_branches; ++i) {
        logits[i] = cce_router_branch_logit(forest, input, dim, i);
    }

    *out_logits = logits;
    return CCE_OK;
}

static float cce_router_pair_consistency(float primary_score,
                                         float counterfactual_score) {
    float denom = fabsf(primary_score) + fabsf(counterfactual_score) + CCE_ROUTER_EPS;
    float normalized_margin = (primary_score - counterfactual_score) / denom;
    return cce_router_clamp01(0.5f + 0.5f * normalized_margin);
}

static int cce_router_contains_ci(const char* haystack, const char* needle) {
    size_t nlen;
    if (!haystack || !needle || needle[0] == '\0') return 0;
    nlen = strlen(needle);
    for (const char* p = haystack; *p; ++p) {
        size_t i = 0;
        while (i < nlen && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            ++i;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

int cce_router_is_creative_task(const char* task_text) {
    static const char* const terms[] = {
        "story", "fiction", "narrative", "testimony", "dilemma",
        "character", "voice", "folklore", "myth", "scene",
        "creative", "moral", "consequence"
    };
    if (!task_text || task_text[0] == '\0') return 0;
    for (int i = 0; i < (int)(sizeof(terms) / sizeof(terms[0])); ++i) {
        if (cce_router_contains_ci(task_text, terms[i])) return 1;
    }
    return 0;
}

static int cce_router_branch_is_narrative(const cce_branch* branch) {
    if (!branch) return 0;
    if (branch->specialist_type == CCE_SPECIALIST_NARRATIVE) return 1;
    return cce_router_contains_ci(branch->name, "narrative") ||
           cce_router_contains_ci(branch->name, "story") ||
           cce_router_contains_ci(branch->name, "fiction") ||
           cce_router_contains_ci(branch->name, "testimony") ||
           cce_router_contains_ci(branch->name, "memory-witness");
}

static void cce_router_apply_task_preference(cce_forest* forest,
                                             const char* task_text,
                                             float* logits) {
    if (!forest || !logits || !cce_router_is_creative_task(task_text)) return;
    for (int i = 0; i < forest->num_branches; ++i) {
        if (cce_router_branch_is_narrative(&forest->branches[i])) {
            logits[i] += CCE_ROUTER_NARRATIVE_BONUS;
        }
    }
}

static void cce_router_fill_route(const cce_forest* forest,
                                  int branch_idx,
                                  float logit,
                                  float route_score,
                                  CounterfactualRoute* out) {
    memset(out, 0, sizeof(*out));
    out->branch_index = branch_idx;
    out->route_score = route_score;
    out->logit = logit;
    out->consistency_score = 1.0f;
    out->contrast_margin = 0.0f;
    if (forest->branches[branch_idx].name[0] != '\0') {
        snprintf(out->branch_name, sizeof(out->branch_name), "%s",
                 forest->branches[branch_idx].name);
    } else {
        snprintf(out->branch_name, sizeof(out->branch_name), "branch_%d", branch_idx);
    }
}

void cce_ssmax(const float* logits, int n, float temperature, float* probs, int top_k) {
    if (!logits || !probs || n <= 0) return;
    if (temperature <= 0) temperature = 1.0f;
    if (top_k <= 0 || top_k > n) top_k = n;

    /* Scale logits */
    float* scaled = (float*)malloc(n * sizeof(float));
    if (!scaled) return;
    for (int i = 0; i < n; ++i) {
        scaled[i] = logits[i] / temperature;
    }

    /* Find top-k (simple partial sort for small n) */
    int* idx = (int*)malloc(n * sizeof(int));
    if (!idx) {
        free(scaled);
        return;
    }
    for (int i = 0; i < n; ++i) idx[i] = i;

    /* Bubble sort top-k (demo, fine for <1000 branches) */
    for (int i = 0; i < top_k && i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (scaled[idx[j]] > scaled[idx[i]]) {
                int tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
            }
        }
    }

    /* Zero out everything beyond top-k */
    float sum = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (i < top_k) {
            probs[idx[i]] = expf(scaled[idx[i]]);
            sum += probs[idx[i]];
        } else {
            probs[idx[i]] = 0.0f;
        }
    }

    /* Normalize */
    if (sum > 0) {
        for (int i = 0; i < top_k && i < n; ++i) {
            probs[idx[i]] /= sum;
        }
    } else {
        /* fallback uniform on top-k */
        float uni = 1.0f / top_k;
        for (int i = 0; i < top_k && i < n; ++i) {
            probs[idx[i]] = uni;
        }
    }

    free(scaled);
    free(idx);
}

cce_result cce_router_init(cce_router* r, float temp, int top_k) {
    if (!r) return CCE_ERR_INVALID_ARG;
    r->temperature = (temp > 0.0f) ? temp : 1.0f;
    r->novelty_threshold = 0.3f;
    r->top_k = (top_k > 0) ? top_k : 1;
    return CCE_OK;
}

cce_result cce_router_route(cce_router* r, cce_forest* forest,
                            const float* input, int dim,
                            int* top_branch, float* score) {
    if (!r || !forest || !input || !top_branch || forest->num_branches <= 0 || dim <= 0)
        return CCE_ERR_INVALID_ARG;

    float* logits = NULL;
    cce_result rc = cce_router_compute_logits(forest, input, dim, &logits);
    if (rc != CCE_OK) return rc;

    /* Apply SSMax */
    float* probs = (float*)calloc((size_t)forest->num_branches, sizeof(float));
    if (!probs) {
        free(logits);
        return CCE_ERR_OOM;
    }
    cce_ssmax(logits, forest->num_branches, r->temperature, probs, r->top_k);

    /* Pick top branch (highest prob) */
    int best = 0;
    float best_p = 0.0f;
    for (int i = 0; i < forest->num_branches; ++i) {
        if (probs[i] > best_p) {
            best_p = probs[i];
            best = i;
        }
    }

    *top_branch = best;
    if (score) *score = best_p;

    free(logits);
    free(probs);
    return CCE_OK;
}

cce_result cce_router_route_for_task(cce_router* r, cce_forest* forest,
                                     const float* input, int dim,
                                     const char* task_text,
                                     int* top_branch, float* score) {
    if (!r || !forest || !input || !top_branch || forest->num_branches <= 0 || dim <= 0)
        return CCE_ERR_INVALID_ARG;

    float* logits = NULL;
    cce_result rc = cce_router_compute_logits(forest, input, dim, &logits);
    if (rc != CCE_OK) return rc;

    cce_router_apply_task_preference(forest, task_text, logits);

    float* probs = (float*)calloc((size_t)forest->num_branches, sizeof(float));
    if (!probs) {
        free(logits);
        return CCE_ERR_OOM;
    }
    cce_ssmax(logits, forest->num_branches, r->temperature, probs, r->top_k);

    int best = 0;
    float best_p = 0.0f;
    for (int i = 0; i < forest->num_branches; ++i) {
        if (probs[i] > best_p) {
            best_p = probs[i];
            best = i;
        }
    }

    *top_branch = best;
    if (score) *score = best_p;

    free(logits);
    free(probs);
    return CCE_OK;
}

cce_result cce_router_sample_counterfactuals(cce_router* r, cce_forest* forest,
                                             const float* input, int dim,
                                             int primary_branch,
                                             CounterfactualRoute* out_routes,
                                             int max_routes,
                                             int* out_count) {
    if (!r || !forest || !input || !out_routes || !out_count ||
        forest->num_branches <= 0 || dim <= 0 || max_routes <= 0 ||
        primary_branch < 0 || primary_branch >= forest->num_branches)
        return CCE_ERR_INVALID_ARG;

    *out_count = 0;
    if (forest->num_branches <= 1) return CCE_OK;

    float* logits = NULL;
    cce_result rc = cce_router_compute_logits(forest, input, dim, &logits);
    if (rc != CCE_OK) return rc;

    float* probs = (float*)calloc((size_t)forest->num_branches, sizeof(float));
    if (!probs) {
        free(logits);
        return CCE_ERR_OOM;
    }

    cce_ssmax(logits, forest->num_branches, r->temperature, probs,
              forest->num_branches);

    int cap = max_routes;
    if (cap > CCE_ROUTER_MAX_COUNTERFACTUALS) cap = CCE_ROUTER_MAX_COUNTERFACTUALS;
    if (cap > forest->num_branches - 1) cap = forest->num_branches - 1;

    float primary_score = probs[primary_branch];
    int count = 0;
    while (count < cap) {
        int best = -1;
        float best_p = -1.0f;

        for (int i = 0; i < forest->num_branches; ++i) {
            int already_selected = 0;
            if (i == primary_branch) continue;
            for (int j = 0; j < count; ++j) {
                if (out_routes[j].branch_index == i) {
                    already_selected = 1;
                    break;
                }
            }
            if (already_selected) continue;
            if (probs[i] > best_p) {
                best_p = probs[i];
                best = i;
            }
        }

        if (best < 0) break;
        cce_router_fill_route(forest, best, logits[best], probs[best],
                              &out_routes[count]);
        out_routes[count].contrast_margin = primary_score - probs[best];
        out_routes[count].consistency_score =
            cce_router_pair_consistency(primary_score, probs[best]);
        ++count;
    }

    *out_count = count;
    free(logits);
    free(probs);
    return CCE_OK;
}

float cce_router_consistency_score(const CounterfactualRoute* route,
                                   const CounterfactualRoute* counterfactual_routes,
                                   int counterfactual_count) {
    if (!route) return 0.0f;
    if (!counterfactual_routes || counterfactual_count <= 0) return 1.0f;

    float min_score = 1.0f;
    for (int i = 0; i < counterfactual_count; ++i) {
        float score = counterfactual_routes[i].consistency_score;
        if (route->route_score > CCE_ROUTER_EPS ||
            counterfactual_routes[i].route_score > CCE_ROUTER_EPS) {
            score = cce_router_pair_consistency(route->route_score,
                                                counterfactual_routes[i].route_score);
        }
        score = cce_router_clamp01(score);
        if (score < min_score) min_score = score;
    }
    return min_score;
}

cce_result cce_router_verify_claim(const CounterfactualRoute* route,
                                   const char* claim,
                                   const CounterfactualRoute* counterfactual_routes,
                                   int counterfactual_count,
                                   float* consistency_score,
                                   float* uncertainty) {
    if (!route || !claim || claim[0] == '\0' || counterfactual_count < 0)
        return CCE_ERR_INVALID_ARG;

    float consistency = cce_router_consistency_score(route,
                                                     counterfactual_routes,
                                                     counterfactual_count);
    if (consistency_score) *consistency_score = consistency;
    if (uncertainty) *uncertainty = 1.0f - consistency;
    return CCE_OK;
}
