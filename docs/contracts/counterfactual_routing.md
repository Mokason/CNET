# Counterfactual Routing Contract

## Purpose

Counterfactual routing reduces hallucination risk by comparing the selected expert route with a small set of plausible alternative routes before a claim is treated as stable.

## Native Interface

- `CounterfactualRoute` records branch index, branch name, route score, raw logit, contrast margin, and a normalized consistency score.
- `cce_router_sample_counterfactuals(...)` returns the top 2-3 non-primary expert routes ranked by all-branch route probability.
- `cce_router_consistency_score(...)` conservatively returns the minimum primary-vs-counterfactual consistency pressure.
- `cce_router_verify_claim(route, claim, counterfactual_routes, count, consistency, uncertainty)` validates that a claim has route-grounded evidence and reports uncertainty as `1 - consistency`.

## Consistency Rule

The primary route and each counterfactual route are scored from the same forest logits. For each pair:

```text
consistency = clamp01(0.5 + 0.5 * ((primary_score - counterfactual_score) /
              (abs(primary_score) + abs(counterfactual_score) + eps)))
```

The claim-level score is the minimum pair score. This is intentionally conservative: a near-tie alternative route should flag the claim for review even if weaker alternatives agree.

## MCP Contract

`cnet_verify_claim` accepts optional:

- `counterfactualRoutes`: human-readable route labels or evidence summaries.
- `counterfactualConsistency`: normalized score from the native router or an equivalent evaluator.

When consistency is below `0.55`, the MCP verdict is `NEEDS_REVIEW`; otherwise it remains `PARTIALLY_SUPPORTED`.

## Non-Goals

This contract does not prove factual truth from text alone. It verifies route stability and exposes uncertainty so downstream retrieval, activation checks, or human review can decide whether to trust the claim.
