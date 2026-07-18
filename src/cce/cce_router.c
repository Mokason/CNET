#include "../../include/cce/cce_router.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#if defined(__SSE__) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
#include <xmmintrin.h>
#include <pmmintrin.h>
#define CCE_HAVE_MXCSR 1
#endif

#define CCE_ROUTER_EPS 1.0e-6f
#define CCE_ROUTER_NARRATIVE_BONUS 1.0f
/* DSA-style relative margin: keep only logits within this gap of the max
 * (in temperature-scaled space). exp(-margin) is the relative mass floor
 * before renorm — values farther from the peak are exact zeros. */
#define CCE_SSMAX_DEFAULT_MARGIN 4.0f
#define CCE_SSMAX_POST_MIN_MASS  CCE_SLEEP_DEFAULT_EPS

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

/* Stack scratch for hot SSMax paths (branch counts stay well under this). */
#define CCE_SSMAX_STACK_N 256

/* Partial selection sort: put top-k indices first in idx[] (descending score).
 * Tie-break: lower original index wins (strict >), matching MoE / llama.cpp. */
static void cce_ssmax_select_topk(const float* scaled, int n, int top_k, int* idx) {
    int i, j;
    for (i = 0; i < n; ++i) idx[i] = i;
    if (top_k > n) top_k = n;
    for (i = 0; i < top_k; ++i) {
        int best = i;
        for (j = i + 1; j < n; ++j) {
            if (scaled[idx[j]] > scaled[idx[best]]) best = j;
        }
        if (best != i) {
            int tmp = idx[i];
            idx[i] = idx[best];
            idx[best] = tmp;
        }
    }
}

/* K-pass argmax with taken[] — O(k*n), no full index buffer. Same ties as
 * select_topk (lowest index wins). Writes descending-score order into sel[]. */
static int cce_ssmax_pick_topk(const float* scores, int n, int top_k,
                               int* sel) {
    int k, o, taken_cap;
    unsigned char* taken;
    unsigned char taken_stk[CCE_SSMAX_STACK_N];

    if (!scores || !sel || n <= 0 || top_k <= 0) return 0;
    if (top_k > n) top_k = n;

    if (n <= CCE_SSMAX_STACK_N) {
        taken = taken_stk;
        memset(taken, 0, (size_t)n);
        taken_cap = 0; /* stack — don't free */
    } else {
        taken = (unsigned char*)calloc((size_t)n, 1);
        if (!taken) return 0;
        taken_cap = 1;
    }

    for (k = 0; k < top_k; ++k) {
        int best = -1;
        for (o = 0; o < n; ++o) {
            if (taken[o]) continue;
            if (best < 0 || scores[o] > scores[best]) best = o;
        }
        if (best < 0) {
            top_k = k;
            break;
        }
        taken[best] = 1;
        sel[k] = best;
    }
    if (taken_cap) free(taken);
    return top_k;
}

/*
 * Sparse Softmax (SSMax) — DeepSeek DSA-inspired selection for routing.
 *
 * Classic dense softmax puts nonzero mass on EVERY candidate ("under the
 * curve"), so weak / near-zero scores still dilute the decision.
 *
 * DeepSeek Sparse Attention (DSA, DeepSeek-V3.2-Exp) fixes the same pathology
 * in sequence attention by (1) scoring candidates, (2) selecting a sparse
 * support (top-k heavy hitters), (3) attending / normalizing only on that
 * support. We apply the same pattern to CNET branch routing:
 *
 *   logits → scale by temperature
 *         → keep only (top-k) ∩ (within `margin` of the max)
 *         → stable softmax renorm over the support only
 *         → exact zeros elsewhere
 *         → optional post-min-mass hard prune + renorm
 *
 * Not a full DSA attention kernel — same sparsity principle for selection.
 */
void cce_ssmax_ex(const float* logits, int n, float temperature, float* probs,
                  int top_k, float margin) {
    int i, support_n = 0;
    float maxv, sum, inv_t;
    cce_fp_enable_ftz_daz();
    float* scaled = NULL;
    int* idx = NULL;
    int* keep = NULL;
    float scaled_stk[CCE_SSMAX_STACK_N];
    int idx_stk[CCE_SSMAX_STACK_N];
    int keep_stk[CCE_SSMAX_STACK_N];
    int heap = 0;

    if (!logits || !probs || n <= 0) return;
    if (temperature <= 0.0f) temperature = 1.0f;
    inv_t = 1.0f / temperature;
    if (top_k <= 0) top_k = 1;
    if (top_k > n) top_k = n;
    if (margin < 0.0f) margin = CCE_SSMAX_DEFAULT_MARGIN;

    for (i = 0; i < n; ++i) probs[i] = 0.0f;

    if (n <= CCE_SSMAX_STACK_N) {
        scaled = scaled_stk;
        idx = idx_stk;
        keep = keep_stk;
        memset(keep, 0, (size_t)n * sizeof(int));
    } else {
        heap = 1;
        scaled = (float*)malloc((size_t)n * sizeof(float));
        idx = (int*)malloc((size_t)n * sizeof(int));
        keep = (int*)calloc((size_t)n, sizeof(int));
        if (!scaled || !idx || !keep) {
            free(scaled);
            free(idx);
            free(keep);
            /* fail-soft: argmax one-hot */
            {
                int best = 0;
                for (i = 1; i < n; ++i)
                    if (logits[i] > logits[best]) best = i;
                probs[best] = 1.0f;
            }
            return;
        }
    }

    maxv = logits[0] * inv_t;
    for (i = 0; i < n; ++i) {
        scaled[i] = logits[i] * inv_t;
        if (scaled[i] > maxv) maxv = scaled[i];
    }

    /* Stage 1: top-k candidates by scaled logit */
    cce_ssmax_select_topk(scaled, n, top_k, idx);

    /* Stage 2: DSA-style relative margin — drop top-k members that are still
     * far under the peak (the "under the curve" tail inside top-k). */
    for (i = 0; i < top_k; ++i) {
        int j = idx[i];
        if (scaled[j] >= maxv - margin) {
            keep[j] = 1;
            support_n++;
        }
    }

    /* Always keep the true argmax even if top_k/margin edge cases */
    if (support_n == 0) {
        int best = 0;
        for (i = 1; i < n; ++i)
            if (scaled[i] > scaled[best]) best = i;
        keep[best] = 1;
        support_n = 1;
    }

    /* Stage 3: stable softmax only over support */
    sum = 0.0f;
    for (i = 0; i < n; ++i) {
        if (!keep[i]) {
            probs[i] = 0.0f;
            continue;
        }
        /* clamp extreme negatives after max-subtract for overflow safety */
        float z = scaled[i] - maxv;
        if (z < -60.0f) {
            probs[i] = 0.0f;
            keep[i] = 0;
            continue;
        }
        probs[i] = expf(z);
        sum += probs[i];
    }

    if (sum <= 0.0f) {
        int best = 0;
        for (i = 1; i < n; ++i)
            if (logits[i] > logits[best]) best = i;
        for (i = 0; i < n; ++i) probs[i] = 0.0f;
        probs[best] = 1.0f;
        if (heap) {
            free(scaled);
            free(idx);
            free(keep);
        }
        return;
    }

    for (i = 0; i < n; ++i) {
        if (keep[i]) probs[i] /= sum;
        else probs[i] = 0.0f;
    }

    /* Stage 4: physics-style sleep — tiny mass → exact 0, renorm survivors.
     * Exact zeros can be skipped next cycle (no branch / no expert matmul). */
    {
        int best = 0;
        for (i = 1; i < n; ++i)
            if (logits[i] > logits[best]) best = i;
        cce_sleep_renorm(probs, n, CCE_SSMAX_POST_MIN_MASS, best);
    }

    if (heap) {
        free(scaled);
        free(idx);
        free(keep);
    }
}

void cce_ssmax(const float* logits, int n, float temperature, float* probs, int top_k) {
    cce_ssmax_ex(logits, n, temperature, probs, top_k, CCE_SSMAX_DEFAULT_MARGIN);
}

/* Count nonzero support size (for tests / telemetry). */
int cce_ssmax_support_size(const float* probs, int n) {
    int c = 0, i;
    if (!probs || n <= 0) return 0;
    for (i = 0; i < n; ++i)
        if (probs[i] > 0.0f) c++;
    return c;
}

/*
 * Physics-style sleep (dead zone): residual mass below eps becomes exact zero
 * so the next cycle can skip that expert / branch / KV row. Survivors renorm
 * to unit sum. All-asleep → one-hot fallback (never leave an empty mix).
 */
int cce_sleep_renorm(float* w, int n, float eps, int fallback_idx) {
    int i, kept = 0;
    float sum = 0.0f;

    if (!w || n <= 0) return 0;
    if (eps < 0.0f) eps = CCE_SLEEP_DEFAULT_EPS;
    if (fallback_idx < 0 || fallback_idx >= n) fallback_idx = 0;

    for (i = 0; i < n; ++i) {
        if (w[i] > 0.0f && w[i] < eps) w[i] = 0.0f;
        if (!(w[i] > 0.0f) || !isfinite(w[i])) {
            w[i] = 0.0f;
            continue;
        }
        sum += w[i];
        kept++;
    }

    if (kept == 0 || !(sum > 0.0f) || !isfinite(sum)) {
        for (i = 0; i < n; ++i) w[i] = 0.0f;
        w[fallback_idx] = 1.0f;
        return 1;
    }
    if (fabsf(sum - 1.0f) > 1e-6f) {
        for (i = 0; i < n; ++i)
            if (w[i] > 0.0f) w[i] /= sum;
    }
    return kept;
}

void cce_fp_enable_ftz_daz(void) {
#ifdef CCE_HAVE_MXCSR
    static int once = 0;
    if (once) return;
    once = 1;
    /* FTZ: flush denormal results to zero. DAZ: treat denormal inputs as zero.
     * Stops exp-tail / multiply thrash (Skyrim-style micro-noise cycle burn). */
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#endif
}

/*
 * MoE / layer helper: top-k by raw logits, then stable softmax renorm ONLY on
 * the selected support. Mathematically identical to the classic path
 *   dense_softmax(all) → pick top-k by prob → renorm selected
 * because softmax is monotone in the logits, and renorming a softmax subset
 * equals softmax over that subset. Avoids O(E) exp for large expert counts.
 *
 * Does not sleep here — callers may cce_sleep_renorm then skip w==0 work.
 */
int cce_ssmax_topk_weights(const float* logits, int n, int top_k,
                           int* sel, float* w) {
    int k, i;
    float mx, sum;

    if (!logits || !sel || !w || n <= 0 || top_k <= 0) return 0;
    cce_fp_enable_ftz_daz();
    k = cce_ssmax_pick_topk(logits, n, top_k, sel);
    if (k <= 0) return 0;

    mx = logits[sel[0]];
    for (i = 1; i < k; ++i)
        if (logits[sel[i]] > mx) mx = logits[sel[i]];

    sum = 0.0f;
    for (i = 0; i < k; ++i) {
        float z = logits[sel[i]] - mx;
        if (z < -60.0f) {
            w[i] = 0.0f;
        } else {
            w[i] = expf(z);
            sum += w[i];
        }
    }
    if (sum <= 0.0f) {
        /* fail-soft: one-hot on first (argmax among selected) */
        for (i = 0; i < k; ++i) w[i] = 0.0f;
        w[0] = 1.0f;
        return k;
    }
    for (i = 0; i < k; ++i) w[i] /= sum;
    return k;
}

cce_result cce_router_init(cce_router* r, float temp, int top_k) {
    if (!r) return CCE_ERR_INVALID_ARG;
    r->temperature = (temp > 0.0f) ? temp : 1.0f;
    r->novelty_threshold = 0.3f;
    r->top_k = (top_k > 0) ? top_k : 1;
    r->ssmax_margin = CCE_SSMAX_DEFAULT_MARGIN;
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

    float* probs = (float*)calloc((size_t)forest->num_branches, sizeof(float));
    if (!probs) {
        free(logits);
        return CCE_ERR_OOM;
    }
    cce_ssmax_ex(logits, forest->num_branches, r->temperature, probs, r->top_k,
                 r->ssmax_margin);

    int best = 0;
    float best_p = -1.0f;
    for (int i = 0; i < forest->num_branches; ++i) {
        if (probs[i] > best_p) {
            best_p = probs[i];
            best = i;
        }
    }

    *top_branch = best;
    if (score) *score = best_p > 0.0f ? best_p : 0.0f;

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
    cce_ssmax_ex(logits, forest->num_branches, r->temperature, probs, r->top_k,
                 r->ssmax_margin);

    int best = 0;
    float best_p = -1.0f;
    for (int i = 0; i < forest->num_branches; ++i) {
        if (probs[i] > best_p) {
            best_p = probs[i];
            best = i;
        }
    }

    *top_branch = best;
    if (score) *score = best_p > 0.0f ? best_p : 0.0f;

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

    /* Sparse: at most primary + counterfactual cap, not full dense softmax */
    {
        int k = r->top_k;
        if (k < CCE_ROUTER_MAX_COUNTERFACTUALS + 1)
            k = CCE_ROUTER_MAX_COUNTERFACTUALS + 1;
        if (k > forest->num_branches) k = forest->num_branches;
        cce_ssmax_ex(logits, forest->num_branches, r->temperature, probs, k,
                     r->ssmax_margin);
    }

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
            /* Only consider sparse support; if support is too thin, fall back
             * to raw logits among non-primary. */
            float score_i = (probs[i] > 0.0f) ? probs[i] : (logits[i] * 1e-6f);
            if (score_i > best_p) {
                best_p = score_i;
                best = i;
            }
        }

        if (best < 0) break;
        cce_router_fill_route(forest, best, logits[best],
                              probs[best] > 0.0f ? probs[best] : 0.0f,
                              &out_routes[count]);
        out_routes[count].contrast_margin = primary_score - out_routes[count].route_score;
        out_routes[count].consistency_score =
            cce_router_pair_consistency(primary_score, out_routes[count].route_score);
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
