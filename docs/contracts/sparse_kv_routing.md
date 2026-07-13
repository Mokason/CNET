# Sparse KV Routing Contract

## API

`cce_specialist_select_kv_tokens(attention_scores, token_count, budget, out_indices, out_cap, out_count)` selects a sorted, unique subset of KV token positions for one specialist.

`cce_supra_set_context_routing(model, mode, budget)` switches autoregressive Supra generation between full KV and sparse routing.

## Default Safety

The default mode is `CCE_CONTEXT_ROUTING_FULL_KV`. Sparse routing must be explicitly enabled. This follows the SCOPE constraint that prefill/context comprehension should not be degraded by default compression.

## Sparse Policy

When sparse routing is enabled, the selector keeps:

- initial tokens for attention-sink behavior.
- recent tokens for local generation continuity.
- long-range stride anchors to reduce lost-in-the-middle failures.
- heavy-hitter middle tokens ranked by attention score.

The selector must never return duplicates and must return indices in ascending causal order.
