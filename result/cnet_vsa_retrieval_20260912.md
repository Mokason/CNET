# Compact retrieval experiment — 2026-09-12

Implemented an opt-in CPU candidate retriever. Both exported presets improve strict top-1 and top-3 over frozen v3.2 on all eight evaluated blocks. Both fit the provisional 64 MiB runtime-data and 1 ms p95 budgets. **The transformer has not been beaten across the task:** Mistral/Gemma everyday questions and the additional 773-question control still favor the original Qwen comparator; a stronger Qwen multi-vector control also leads on held-out sentences. Production stays v3.2. Nothing is committed.

## Selected results

Strict top-1, percent. These are previously exposed synthetic sets, not a fresh blind qualification. All presets use the same 1,068 capsules. Tiny point-estimate leads do not establish robust superiority.

| Block | n | v3.2 | Small (256 terms) | Larger (1024 terms) | Published Qwen3-4B |
|---|---:|---:|---:|---:|---:|
| Held-out sentences | 2136 | 33.71 | 38.90 | 38.72 | 37.64 |
| Frozen questions | 5258 | 64.24 | 75.77 | 76.68 | 56.05 |
| Regression 88 | 88 | 55.68 | 67.05 | 69.32 | 47.73 |
| Colloquial | 1877 | 57.06 | 59.83 | 59.78 | 49.44 |
| Contrast | 549 | 33.70 | 39.71 | 40.07 | 32.06 |
| Mistral everyday | 600 | 17.83 | 21.17 | 21.83 | 24.17 |
| Gemma everyday | 600 | 24.17 | 27.67 | 28.67 | 30.00 |
| Mistral in-corpus control | 400 | 24.75 | 38.25 | 38.00 | 37.50 |

The additional, previously exposed and partly overlapping 773-question Mistral control scores 22.12% for v3.2, **26.52% small / 26.39% larger**, and 27.55% Qwen. Small wins 73 and loses 39 queries against v3.2; larger wins 70 and loses 37. This is a control, not independent fresh evidence.

## Footprint and speed

| Export | Runtime data | p50 | p95 | CLI smoke peak RSS |
|---|---:|---:|---:|---:|
| 256 terms/capsule | 56.14 MiB | 332.8 μs | 428.4 μs | 109.69 MiB |
| 1024 terms/capsule | 60.98 MiB | 342.8 μs | 434.1 μs | 113.38 MiB |

Each exported runtime replayed 11,508 queries. Timing covers CPU encoding, both tokenizers, retrieval, passage features and top-20 sorting, with Python overhead included. It excludes loading, assertions, certification and execution. These are single-thread local measurements, not concurrency throughput guarantees. CLI RSS is a separate smoke measurement; verification RSS (167–171 MiB) includes full evaluation matrices. The 64 MiB cap is **data**, not total process memory.

Frozen v3.2 centroid encoding plus ranking measured about 105–115 μs p95 on the 320-query diagnostic sample. The new retriever therefore adds CPU work; the original 5–7 μs encoder-only number is not a comparable end-to-end baseline. No new Qwen latency benchmark was run.

Runtime data includes the unchanged 41.46 MiB lexicon, centroid/index arrays, passage arrays, tokenizer assets and capsule names. Offline model weights, source/evaluation caches, compiler artifacts and Python libraries are separate. The larger export costs about 4.84 MiB more than the default. Native deployment RSS remains unmeasured.

## What changed

Queries combine three complementary rankings: the frozen v3.2 centroid, BM25 over corpus plus existing external-teacher training questions, and a precomputed semantic term index. Equal reciprocal-rank fusion uses constant 60 and top 20 per branch. Candidate passage scoring adds 20% weight to 75% content-term coverage plus 25% adjacent ordered content-term pairs found within one source passage. This is a matching feature; negation interpretation and certified answer verification are not provided.

The semantic index comes from [OpenSearch document-only v3 distill](https://huggingface.co/opensearch-project/opensearch-neural-sparse-encoding-doc-v3-distill/tree/babf71f3c48695e2e53a978208e8aba48335e3c0), pinned to revision `babf71f3c48695e2e53a978208e8aba48335e3c0` (Apache-2.0). Its transformer runs **offline only**. This moves semantic work into corpus compilation; it does not establish independence from transformer knowledge. The query path uses tokenization, sparse sums and existing CNET C primitives, with no Torch/Transformers import.

The publisher reference score reproduced at 11.11048 versus 11.1105. The build processed 61,112 train sentences in 5,757 chunks of at most 254 tokens on AMD/ROCm. The measured forward/aggregation phase was 8.69 seconds, excluding model download/load and source tokenization. No new question generation or lexicon training occurred. Capsule max pooling and term pruning are adaptations of the model, not a reproduction of OpenSearch benchmark scores.

## Failed alternatives and controls

- Four/eight VSA prototypes regressed; their extra memory and ranking work did not earn retention.
- Expanded BM25 and passage matching helped without a new semantic model, but left larger content-transfer deficits.
- Preserving separate semantic chunks did not close the remaining gap. The 256-term chunk/passage preset used 64.86 MiB before export overhead and failed the 64 MiB cap. The floor was not changed.
- Qwen also benefits from representation changes: eight prototypes mixed equally with its centroid reached **42.37% held-out sentence top-1** and **66.98% frozen-question top-1**, versus 38.90% and 75.77% for the small CNET preset. These controls reconstruct from raw float16 caches and are labeled separately from published Qwen results. Extra-set query vectors are absent, so their multi-vector Qwen comparison is WITHHELD.

All attempted presets follow below. Column order is held-out sentences / frozen questions / regression88 / colloquial / contrast / Mistral everyday / Gemma everyday / Mistral in-corpus control. Numbers are strict top-1 percentages. The companion JSON retains top-3, recall@20, paired wins/losses against v3.2, resource measurements and hashes for every preset.

| Preset | Strict top-1 across the eight blocks |
|---|---|
| centroid | 33.71 / 64.24 / 55.68 / 57.06 / 33.70 / 17.83 / 24.17 / 24.75 |
| prototype4 | 27.72 / 50.42 / 39.77 / 40.86 / 23.68 / 13.17 / 19.17 / 23.25 |
| prototype4_mix | 33.80 / 60.67 / 47.73 / 50.77 / 29.51 / 17.17 / 23.33 / 26.25 |
| prototype8 | 26.03 / 46.63 / 32.95 / 33.83 / 20.04 / 12.33 / 17.33 / 22.00 |
| prototype8_mix | 32.91 / 59.19 / 48.86 / 48.53 / 28.60 / 15.67 / 22.00 / 27.75 |
| bm25_corpus | 34.64 / 60.16 / 34.09 / 35.43 / 23.50 / 13.33 / 19.67 / 29.75 |
| fusion_corpus | 35.49 / 66.49 / 52.27 / 48.32 / 30.42 / 18.17 / 23.83 / 29.25 |
| bm25_expanded | 34.74 / 62.31 / 56.82 / 51.04 / 35.34 / 18.33 / 27.83 / 30.25 |
| fusion_expanded | 35.25 / 66.74 / 60.23 / 58.87 / 37.89 / 18.67 / 26.17 / 28.75 |
| passage_corpus | 36.84 / 74.10 / 55.68 / 50.08 / 33.33 / 18.83 / 23.67 / 34.00 |
| passage_expanded | 36.89 / 75.24 / 60.23 / 60.68 / 40.07 / 19.00 / 26.50 / 36.00 |
| semantic256 | 31.79 / 59.57 / 56.82 / 45.50 / 29.51 / 18.67 / 26.33 / 30.75 |
| semantic256_fusion | 36.52 / 66.66 / 59.09 / 55.25 / 34.97 / 20.83 / 27.67 / 32.00 |
| semantic256_three | 37.50 / 69.84 / 62.50 / 59.88 / 37.70 / 20.17 / 28.67 / 32.75 |
| semantic256_passage | 38.90 / 75.77 / 67.05 / 59.83 / 39.71 / 21.17 / 27.67 / 38.25 |
| semantic1024 | 32.16 / 62.70 / 53.41 / 46.72 / 32.06 / 18.33 / 25.67 / 32.25 |
| semantic1024_fusion | 36.28 / 68.47 / 57.95 / 55.67 / 34.43 / 19.67 / 27.83 / 33.50 |
| semantic1024_three | 37.73 / 70.84 / 61.36 / 59.30 / 38.25 / 21.17 / 29.00 / 32.00 |
| semantic1024_passage | 38.72 / 76.68 / 69.32 / 59.78 / 40.07 / 21.83 / 28.67 / 38.00 |
| chunks128 | 31.32 / 65.71 / 59.09 / 49.92 / 34.79 / 19.67 / 26.17 / 32.00 |
| chunks128_fusion | 35.96 / 69.53 / 60.23 / 57.96 / 34.61 / 20.00 / 28.17 / 33.50 |
| chunks128_three | 37.50 / 71.87 / 64.77 / 60.84 / 39.16 / 20.00 / 28.83 / 33.00 |
| chunks128_passage | 38.06 / 77.18 / 68.18 / 61.05 / 40.07 / 21.00 / 28.00 / 37.75 |
| chunks256 | 31.13 / 67.04 / 60.23 / 50.99 / 34.97 / 19.83 / 25.50 / 32.00 |
| chunks256_fusion | 35.86 / 70.20 / 61.36 / 58.82 / 34.79 / 20.00 / 27.67 / 33.50 |
| chunks256_three | 37.13 / 72.19 / 64.77 / 61.48 / 39.34 / 20.83 / 28.67 / 33.75 |
| chunks256_passage | 37.55 / 77.56 / 68.18 / 61.11 / 40.62 / 21.33 / 28.50 / 38.75 |

## Top-3 comparison

| Block | v3.2 | Small | Larger | Published Qwen |
|---|---:|---:|---:|---:|
| Held-out sentences | 49.91 | 55.15 | 55.10 | 56.84 |
| Frozen questions | 82.71 | 89.05 | 89.69 | 78.17 |
| Regression 88 | 78.41 | 87.50 | 87.50 | 75.00 |
| Colloquial | 75.87 | 79.38 | 79.97 | 72.46 |
| Contrast | 52.82 | 61.02 | 60.47 | 67.94 |
| Mistral everyday | 28.83 | 37.67 | 37.67 | 40.33 |
| Gemma everyday | 38.50 | 47.33 | 46.33 | 49.00 |
| Mistral in-corpus control | 41.50 | 54.00 | 54.75 | 57.50 |

Qwen retains top-3 leads on held-out sentences, contrast and all three main writer-control blocks. Candidate recall and top-1 choice remain distinct problems.

## Validation and limits

- 12 focused tests passed, including train/test source separation, prototype aggregation, sparse reference sums, passage locality/order, malformed cache arrays and query bounds. RED failures preceded the corresponding implementation.
- Both exports reproduce 11,508 full score vectors within float tolerance and all 11,508 top-20 lists. Empty/contentless input returns no candidates; corruption fails loudly; NUL and >4096-byte queries are refused.
- The unchanged arena check passes 48 frozen values and 28 claims. All 49 protected baseline files retain their original hashes. This preserves the old gate; it does not certify the experimental retriever.
- Compile checks and focused code review completed. The adapter reuses CNET primitives; arrays are validated before crossing the Python/C boundary. Only one retriever/active lexicon is supported per process; run presets in separate processes. Receipt hashes detect accidental corruption, not malicious replacement of both receipt and payload.
- Evaluation labels never enter indexes or fit ranking weights. Presets were introduced in documented checkpoints, but their selection used exposed evaluation distributions. Strict metrics are retained; diagnostic lenient metrics are omitted because alternate handling differed.
- Authoritative Qwen comparisons use its published JSON. Preliminary reranking of float16 similarity caches changes ties, so those Qwen counts and paired comparisons are excluded from the final record.

Human-query transfer, accepted-route errors, capsule execution correctness and production portability remain **WITHHELD**. If no human is available, fresh external-writer, source-disjoint and counterfactual tests can strengthen evidence, but cannot establish human-query performance. Promotion requires a new qualification set, verifier/abstention calibration and compatibility checks through the existing capsule manifest/export mechanism.

## Files and reproduction

Run instructions and the opt-in CLI are in [experiments/vsa_retrieval/README.md](../experiments/vsa_retrieval/README.md). The [plan](../plans/cnet_vsa_retrieval_20260912.md) records fixed settings and checkpoints. [Machine-readable results](cnet_vsa_retrieval_20260912.json) retain all metrics, original/source/artifact hashes, model provenance and exported receipts. Large rebuildable artifacts remain in ignored `var/arena_cache/retrieval_20260912/`. The existing uncommitted baseline is a prerequisite; this experiment does not claim clean-checkout reproducibility without it.

No production source, certification floor, capsule format, model service or existing benchmark fixture was changed by this experiment. The small export is the experimental default. No commit or production promotion was made.
