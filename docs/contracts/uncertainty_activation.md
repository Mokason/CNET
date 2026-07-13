# Uncertainty-Aware Activation Contract

## Purpose

Phase 4 adds a cheap uncertainty layer for CNET specialists. It is designed for compression and routing gates where a full Bayesian posterior is not available.

## Native API

Header: `include/cce/cce_uncertainty.h`
Implementation: `src/cce/cce_uncertainty.c`

```c
cce_result cce_specialist_get_uncertainty(const float* activations,
                                          int activation_count,
                                          float* uncertainty);

cce_result cce_specialist_get_uncertainty_ex(const float* activations,
                                             int activation_count,
                                             const CounterfactualRoute* routes,
                                             int route_count,
                                             cce_specialist_uncertainty_report* report);
```

## Scoring

- Activation uncertainty is normalized entropy over a specialist activation/logit vector.
- Route uncertainty is normalized entropy over primary + counterfactual route scores.
- Combined uncertainty is `0.65 * activation_entropy + 0.35 * route_entropy` when route evidence exists.

The score is in `[0,1]`: peaked activations and decisive routes are low uncertainty; flat activations and route ties are high uncertainty.

## Compression Gradients

Header: `include/cce/cce_compression.h`
Implementation: `src/cce/cce_compression.c`

`cce_compression_grads` provides a `Grads[]` buffer that accumulates identity and residual backward paths. `cce_supra_decomposed` now owns an optional `compression_grads` buffer for compression-aware recovery passes.
