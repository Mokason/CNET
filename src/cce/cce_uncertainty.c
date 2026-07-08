#include "../../include/cce/cce_uncertainty.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CCE_UNCERTAINTY_EPS 1.0e-8f

static float cce_uncertainty_clamp01(float value) {
    if (value < 0.0f) return 0.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}

static cce_result entropy_from_logits(const float* values,
                                      int count,
                                      float* entropy_out,
                                      float* variance_out) {
    if (!values || count <= 0 || !entropy_out) return CCE_ERR_INVALID_ARG;
    if (count == 1) {
        *entropy_out = 0.0f;
        if (variance_out) *variance_out = 0.0f;
        return CCE_OK;
    }

    float max_v = values[0];
    double mean = 0.0;
    for (int i = 0; i < count; ++i) {
        if (values[i] > max_v) max_v = values[i];
        mean += values[i];
    }
    mean /= (double)count;

    double var = 0.0;
    double sum = 0.0;
    float* probs = (float*)calloc((size_t)count, sizeof(float));
    if (!probs) return CCE_ERR_OOM;

    for (int i = 0; i < count; ++i) {
        double dv = (double)values[i] - mean;
        var += dv * dv;
        probs[i] = expf(values[i] - max_v);
        sum += probs[i];
    }
    var /= (double)count;

    if (sum <= CCE_UNCERTAINTY_EPS) {
        free(probs);
        return CCE_ERR_INVALID_ARG;
    }

    double entropy = 0.0;
    for (int i = 0; i < count; ++i) {
        double p = (double)probs[i] / sum;
        if (p > 0.0) entropy -= p * log(p);
    }
    entropy /= log((double)count);

    *entropy_out = cce_uncertainty_clamp01((float)entropy);
    if (variance_out) {
        *variance_out = cce_uncertainty_clamp01((float)(var / (var + 1.0)));
    }
    free(probs);
    return CCE_OK;
}

static cce_result entropy_from_routes(const CounterfactualRoute* routes,
                                      int route_count,
                                      float* entropy_out,
                                      float* variance_out) {
    if (!entropy_out) return CCE_ERR_INVALID_ARG;
    if (!routes || route_count <= 1) {
        *entropy_out = 0.0f;
        if (variance_out) *variance_out = 0.0f;
        return CCE_OK;
    }

    double sum = 0.0;
    double mean = 0.0;
    for (int i = 0; i < route_count; ++i) {
        float score = routes[i].route_score;
        if (score > 0.0f) sum += score;
        mean += score;
    }
    mean /= (double)route_count;

    double var = 0.0;
    for (int i = 0; i < route_count; ++i) {
        double dv = (double)routes[i].route_score - mean;
        var += dv * dv;
    }
    var /= (double)route_count;

    if (sum <= CCE_UNCERTAINTY_EPS) {
        *entropy_out = 1.0f;
        if (variance_out) *variance_out = 0.0f;
        return CCE_OK;
    }

    double entropy = 0.0;
    for (int i = 0; i < route_count; ++i) {
        float score = routes[i].route_score;
        if (score <= 0.0f) continue;
        double p = (double)score / sum;
        entropy -= p * log(p);
    }
    entropy /= log((double)route_count);

    *entropy_out = cce_uncertainty_clamp01((float)entropy);
    if (variance_out) {
        *variance_out = cce_uncertainty_clamp01((float)(var / (var + 1.0)));
    }
    return CCE_OK;
}

cce_result cce_specialist_get_uncertainty(const float* activations,
                                          int activation_count,
                                          float* uncertainty) {
    cce_specialist_uncertainty_report report;
    cce_result rc = cce_specialist_get_uncertainty_ex(activations,
                                                      activation_count,
                                                      NULL,
                                                      0,
                                                      &report);
    if (rc != CCE_OK) return rc;
    if (uncertainty) *uncertainty = report.combined_uncertainty;
    return uncertainty ? CCE_OK : CCE_ERR_INVALID_ARG;
}

cce_result cce_specialist_get_uncertainty_ex(const float* activations,
                                             int activation_count,
                                             const CounterfactualRoute* routes,
                                             int route_count,
                                             cce_specialist_uncertainty_report* report) {
    if (!activations || activation_count <= 0 || !report || route_count < 0)
        return CCE_ERR_INVALID_ARG;

    memset(report, 0, sizeof(*report));
    cce_result rc = entropy_from_logits(activations,
                                        activation_count,
                                        &report->activation_entropy,
                                        &report->activation_variance);
    if (rc != CCE_OK) return rc;

    rc = entropy_from_routes(routes,
                             route_count,
                             &report->route_entropy,
                             &report->route_variance);
    if (rc != CCE_OK) return rc;

    if (routes && route_count > 1) {
        report->combined_uncertainty = cce_uncertainty_clamp01(
            0.65f * report->activation_entropy + 0.35f * report->route_entropy);
    } else {
        report->combined_uncertainty = report->activation_entropy;
    }
    return CCE_OK;
}
