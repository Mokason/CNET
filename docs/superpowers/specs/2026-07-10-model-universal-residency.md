# Model-Universal Residency and Governance Specification

**Date:** 2026-07-10  
**Status:** implementation contract

## Goal

CNET must govern dense transformers, state-space models, and large MoE models through one native catalog and lifecycle authority. It must retain the largest useful set of resident models that fits declared CPU/GPU budgets, lazily load cold models, reuse hot models, and safely evict inactive models without embedding architecture policy in the scheduler.

The lifecycle is:

`discover → inspect → catalog → admit/place → load once → lease/use → retain/reuse → evict/unload → recover`

## Authority boundary

CNET owns:

- model IDs, artifact identity, descriptors, capabilities, and lifecycle state;
- memory budgets, placement, pinning, admission, load deduplication, and eviction;
- request identity, deadlines, cancellation, evidence, certification, and policy.

Backends own only:

- architecture-specific inspection details not represented by generic GGUF metadata;
- model materialization and release;
- sessions, tokenizer/template behavior, KV state, kernels, and inference execution.

DS4, CCE, and llama.cpp are backend implementations. None may create a second catalog, scheduler, policy engine, or trust authority.

## Generic descriptor

Every catalog entry carries:

- stable model ID, artifact path/digest, backend name, format, architecture, quantization;
- class: dense transformer, MoE transformer, SSM, embedding, or other;
- capability flags such as text generation and embeddings;
- artifact bytes, conservative resident bytes per selected resource, workspace reserve, context limit, and KV bytes/token;
- allowed resource mask, preferred resource mask, and required resource count.

Dense models normally require one eligible resource and can occupy either R9700. Distributed MoE models may require a fixed `0b11` mask. CPU is represented by an explicit resource bit rather than mask zero.

## Residency manager

The native manager is backend-neutral and thread-safe. It provides:

- bounded model/backend tables;
- independent per-resource byte budgets;
- lazy load on first lease;
- one in-flight load per model with concurrent acquire deduplication;
- per-resource reservations while a load callback runs unlocked;
- conservative admission using an upper-bound resident estimate;
- LRU eviction of resident models only when lease and pin counts are zero;
- atomic leases carrying model identity, placement, handle, and generation;
- explicit cold/loading/resident/failed states;
- hit/load/eviction/failure telemetry and resource utilization;
- explicit retry reset after a failed load.

A backend returning resident usage above its declared upper bound is rejected and unloaded. This prevents optimistic admission from destabilizing another resident model.

## Placement policy

1. Honor a fixed required mask when supplied.
2. For a one-resource dense model, prefer a requested/preferred eligible device when it fits without eviction.
3. Otherwise choose the eligible device requiring the fewest evicted bytes; ties prefer more free capacity.
4. For a multi-resource model, all required resources must be admitted atomically.
5. Never evict a pinned or leased model. Return `BUSY` rather than violating ownership.
6. Models remain resident after the last lease so repeated requests are cache hits.

This policy maximizes useful residency without duplicating models or manufacturing work.

## llama.cpp parity boundary

Adopt now:

- header-first GGUF inspection;
- mmap-friendly artifact identity;
- quantized artifact size accounting;
- explicit CPU/GPU placement;
- persistent model residency and lazy load;
- model/context memory separation;
- LRU unload under pressure.

Backend-local follow-up gates remain:

- architecture/tokenizer/template coverage;
- partial GPU offload and split modes;
- paged/shared KV cache accounting;
- quant coverage and mmap/page-cache performance;
- same-model numerical and throughput parity.

## Acceptance gates

1. Dense and MoE descriptors coexist in one catalog.
2. Two dense models fitting different R9700 budgets remain resident concurrently.
3. A fixed two-GPU MoE admission evicts only inactive models and occupies both budgets atomically.
4. Leased and pinned models block unsafe eviction.
5. Concurrent cold acquires invoke one backend load and return the same generation/handle.
6. Load failure and over-budget actual usage leave no resident handle or accounting leak.
7. Real local GGUFs are inspected without loading tensor data and classified dense/MoE where structural evidence exists.
8. Existing async, CCE, DS4, CPU-only, and dual-R9700 gates remain green.
