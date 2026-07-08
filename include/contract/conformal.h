#ifndef CONTRACT_CONFORMAL_H
#define CONTRACT_CONFORMAL_H

#include <stddef.h>

#include "../nn.h"
#include "contract.h"

/* ===========================================================================
 * Conformal reject-option: a distribution-free abstention guarantee.
 *
 * The coverage layer (contract_coverage.h) proves a primitive on ENUMERABLE
 * domains and gives only a heuristic Wilson floor on the SAMPLED regime. This
 * module replaces that heuristic with split (inductive) conformal prediction
 * (after arXiv:2506.21802 "Classification with Reject Option: Distribution-free
 * Error Guarantees via Conformal Prediction"), which yields a finite-sample,
 * distribution-free guarantee with no assumption on the data distribution --
 * only exchangeability of the calibration + test points.
 *
 * Procedure (single ONEHOT output port = a classifier over field_width classes):
 *   score      s(x,y) = 1 - p_hat(y|x)            (p_hat = softmax of raw output)
 *   quantile   q = the ceil((n+1)(1-alpha))-th smallest TRUE-class calib score
 *   set        C(x) = { y : p_hat(y|x) >= 1 - q }
 *   reject     accept (classify) iff |C(x)| == 1, else ABSTAIN
 *   guarantee  P( Y in C(X) ) >= 1 - alpha          (marginal, distribution-free)
 * ===========================================================================
 */

/* ---- score-level core (classifier-agnostic, fully testable) ------------- */

/* The split-conformal threshold of `n` calibration nonconformity scores at
   target miscoverage alpha in (0,1): the k-th smallest score with
   k = ceil((n+1)(1-alpha)). When k > n the threshold is >= 1.0 (the set then
   contains every class -- never a singleton on coverage grounds). Returns the
   quantile, or a negative value on bad args. Does not modify `scores`. */
double conformal_quantile(const double *scores, size_t n, double alpha);

/* Prediction set for a probability vector at threshold q: class y is included
   iff (1 - probs[y]) <= q. Returns the set size; if set_out != NULL writes the
   0/1 membership over `classes`; if single != NULL writes the sole class when
   the size is exactly 1, else -1. */
size_t conformal_set_from_probs(const double *probs, size_t classes, double q,
                                int *set_out, int *single);

/* Numerically-stable softmax of `n` logits into probs (sums to 1). */
void conformal_softmax(const double *logits, size_t n, double *probs);

/* ---- BTN / contract wrapper --------------------------------------------- */

typedef struct {
    double q;          /* conformal threshold */
    double alpha;      /* target miscoverage (risk) */
    size_t classes;    /* output ONEHOT field_width */
    size_t n_calib;    /* calibration set size */
    int    valid;      /* 1 once calibrated */
} ConformalCalibrator;

/* Calibrate against a BTN whose SINGLE output port is ONEHOT field_count==1
   (a classifier). For each calibration sample i: softmax the raw output, take
   the true class's nonconformity score 1 - p_hat(true_class[i]); the threshold
   is the conformal quantile at alpha. inputs is n x (sum of input port totals);
   true_class[i] in [0, classes). Returns 0, or -1 on bad args / non-classifier
   output port. */
int conformal_calibrate_btn(ConformalCalibrator *cal, BinaryTransformNetwork *btn,
                            const Contract *c, const double *inputs,
                            const size_t *true_class, size_t n, double alpha);

/* Reject-option decision for one input: returns the predicted class (>= 0)
   when the conformal set is a singleton, or -1 to ABSTAIN (empty or >= 2
   classes / not calibrated). */
int conformal_classify_or_abstain(const ConformalCalibrator *cal,
                                  BinaryTransformNetwork *btn, const Contract *c,
                                  const double *input);

/* The certified marginal coverage level (1 - alpha). This is the rigorous,
   distribution-free replacement for coverage_accuracy_lower_bound on the
   SAMPLED regime. Returns 0.0 if uncalibrated. */
double conformal_coverage_level(const ConformalCalibrator *cal);

#endif /* CONTRACT_CONFORMAL_H */





