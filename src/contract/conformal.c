/* contract_conformal.c -- split-conformal reject-option for BTN classifiers.
 * The distribution-free upgrade to contract_coverage's heuristic abstention.
 * See include/contract_conformal.h. Built only on btn_forward + the port
 * geometry; no core machinery is modified.
 */

#include "../../include/contract/conformal.h"
#include "../../include/nn.h"
#include "../../include/contract/contract.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- score-level core --------------------------------------------------- */

void conformal_softmax(const double *logits, size_t n, double *probs) {
    size_t i;
    double mx, sum = 0.0;
    if (n == 0) return;
    mx = logits[0];
    for (i = 1; i < n; ++i) if (logits[i] > mx) mx = logits[i];
    for (i = 0; i < n; ++i) { probs[i] = exp(logits[i] - mx); sum += probs[i]; }
    if (sum <= 0.0) { for (i = 0; i < n; ++i) probs[i] = 1.0 / (double)n; return; }
    for (i = 0; i < n; ++i) probs[i] /= sum;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da < db) ? -1 : (da > db) ? 1 : 0;
}

double conformal_quantile(const double *scores, size_t n, double alpha) {
    double *sorted;
    double q;
    size_t k;
    if (scores == NULL || n == 0 || alpha <= 0.0 || alpha >= 1.0) return -1.0;

    /* k = ceil((n+1)(1-alpha)); the conformal rank. */
    {
        double kf = ceil((double)(n + 1) * (1.0 - alpha));
        if (kf < 1.0) kf = 1.0;
        k = (size_t)kf;
    }
    if (k > n) return 1.0 + 1e-9;   /* threshold admits every class */

    sorted = (double *)malloc(n * sizeof(double));
    if (sorted == NULL) return -1.0;
    memcpy(sorted, scores, n * sizeof(double));
    qsort(sorted, n, sizeof(double), cmp_double);
    q = sorted[k - 1];              /* k-th smallest (1-indexed) */
    free(sorted);
    return q;
}

size_t conformal_set_from_probs(const double *probs, size_t classes, double q,
                                int *set_out, int *single) {
    size_t y, size = 0;
    int last = -1;
    if (single) *single = -1;
    if (probs == NULL || classes == 0) return 0;
    for (y = 0; y < classes; ++y) {
        int in = ((1.0 - probs[y]) <= q + 1e-12) ? 1 : 0;  /* s_y <= q */
        if (set_out) set_out[y] = in;
        if (in) { ++size; last = (int)y; }
    }
    if (single && size == 1) *single = last;
    return size;
}

/* ---- BTN / contract wrapper --------------------------------------------- */

static int classifier_classes(const Contract *c, size_t *classes) {
    if (c == NULL || c->output_port_count != 1) return -1;
    if (c->output_ports[0].family != PORT_ONEHOT ||
        c->output_ports[0].field_count != 1) return -1;
    *classes = c->output_ports[0].field_width;
    return (*classes >= 2) ? 0 : -1;
}

static size_t input_total(const Contract *c) {
    size_t s = 0, i;
    for (i = 0; i < c->input_port_count; ++i)
        s += c->input_ports[i].field_width * c->input_ports[i].field_count;
    return s;
}

int conformal_calibrate_btn(ConformalCalibrator *cal, BinaryTransformNetwork *btn,
                            const Contract *c, const double *inputs,
                            const size_t *true_class, size_t n, double alpha) {
    size_t classes, in_total, i;
    double *scores, *probs;

    if (cal == NULL) return -1;
    memset(cal, 0, sizeof *cal);
    if (btn == NULL || c == NULL || inputs == NULL || true_class == NULL || n == 0)
        return -1;
    if (classifier_classes(c, &classes) != 0) return -1;
    in_total = input_total(c);
    if (in_total != btn->input_count || classes != btn->output_count) return -1;

    scores = (double *)malloc(n * sizeof(double));
    probs  = (double *)malloc(classes * sizeof(double));
    if (scores == NULL || probs == NULL) { free(scores); free(probs); return -1; }

    for (i = 0; i < n; ++i) {
        const double *raw = btn_forward(btn, inputs + i * in_total);
        size_t t = true_class[i];
        if (raw == NULL || t >= classes) { free(scores); free(probs); return -1; }
        conformal_softmax(raw, classes, probs);
        scores[i] = 1.0 - probs[t];          /* nonconformity of the true class */
    }

    cal->q = conformal_quantile(scores, n, alpha);
    cal->alpha = alpha;
    cal->classes = classes;
    cal->n_calib = n;
    cal->valid = (cal->q >= 0.0) ? 1 : 0;

    free(scores);
    free(probs);
    return cal->valid ? 0 : -1;
}

int conformal_classify_or_abstain(const ConformalCalibrator *cal,
                                  BinaryTransformNetwork *btn, const Contract *c,
                                  const double *input) {
    size_t classes;
    const double *raw;
    double *probs;
    int single = -1;
    size_t size;

    if (cal == NULL || !cal->valid || btn == NULL || c == NULL || input == NULL)
        return -1;
    if (classifier_classes(c, &classes) != 0 || classes != cal->classes) return -1;

    raw = btn_forward(btn, input);
    if (raw == NULL) return -1;
    probs = (double *)malloc(classes * sizeof(double));
    if (probs == NULL) return -1;
    conformal_softmax(raw, classes, probs);
    size = conformal_set_from_probs(probs, classes, cal->q, NULL, &single);
    free(probs);

    return (size == 1) ? single : -1;   /* singleton -> classify, else abstain */
}

double conformal_coverage_level(const ConformalCalibrator *cal) {
    if (cal == NULL || !cal->valid) return 0.0;
    return 1.0 - cal->alpha;
}



