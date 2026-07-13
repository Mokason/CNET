# SamKV / SCOPE Adaptation for CNET Sparse KV Routing

## Sources checked

- SamKV: `arXiv:2508.11661`, Sparse Attention across Multiple-context KV Cache, submitted 2025-08-06.
- SCOPE: `arXiv:2412.13649`, Optimizing Key-Value Cache Compression in Long-context Generation, ACL 2025 version.
- SCBench: `arXiv:2412.10319`, KV-cache-centric long-context benchmark, ICLR 2025.

## Design Translation

SamKV motivates CNET's first sparse selector around three ideas:

1. Preserve initial and local/recent KV positions because they act as attention sinks and local context anchors.
2. Select important middle tokens by attention score instead of static truncation.
3. Keep the selector multi-context ready by treating the selector as per-specialist/per-context budget policy, not a monolithic global cache.

SCOPE adds one important guardrail: do not aggressively compress prefill. CNET therefore keeps `CCE_CONTEXT_ROUTING_FULL_KV` as the default and only applies sparse routing when explicitly enabled for autoregressive KV attention.

SCBench suggests dynamic sparsity is stronger than fixed sparse patterns, so the selector combines fixed anchors with per-step heavy hitters.

## Current Phase 2 Slice

Implemented `cce_specialist_select_kv_tokens()` as a C policy primitive:

- keep `initial_tokens` from the beginning of context.
- keep `recent_tokens` from the end of context.
- keep every `long_range_stride` token as a low-cost long-range dependency anchor.
- fill the remaining budget with highest attention-score middle tokens.
- return sorted unique token indices so causal attention order remains stable.

The first runtime hook is `cce_supra_generate_text()` -> `kv_step()`. Full KV is unchanged by default. Sparse routing is enabled through `cce_supra_set_context_routing()`.

## Deferred

- True SamKV-style multi-context personalized query vectors.
- Selective recomputation of sparse KV tokens after cross-context fusion.
- LongBench / InfiniteBench / SCBench measurement against a full-KV baseline.
