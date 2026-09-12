# Compact capsule retrieval experiment

An opt-in CPU retriever combining frozen v3.2 centroids, a compact inverted index
of corpus/teacher-question evidence, offline semantic expansion, and a small
within-passage matching feature. It returns candidate capsule IDs only. It does
not execute, certify, or activate capsules, and does not change the production
router or its gates.

The smaller preset uses about 56.14 MiB of model/index/tokenizer data. The larger
preset uses about 60.98 MiB. The Python CLI has additional interpreter/native
library heap overhead: these are data sizes, **not total process RSS**. Detailed
results and failed alternatives are in
[`result/cnet_vsa_retrieval_20260912.md`](../../result/cnet_vsa_retrieval_20260912.md).

## Query the exported cache

From the repository root:

```bash
python3 experiments/vsa_retrieval/query.py 'your question'
python3 experiments/vsa_retrieval/query.py --index var/arena_cache/retrieval_20260912/runtime1024 'your question'
```

Without a positional query, the command reads one question per stdin line and
keeps the index loaded. JSON output is explicitly `certified: false`. Scores
are ranking features, not calibrated probabilities. Empty/contentless inputs
return no candidates; NUL bytes and inputs exceeding 4096 UTF-8 bytes are refused.
Index corruption and a changed frozen lexicon fail loudly. No model downloads,
GPU work, Torch, or Transformers imports occur in the query process.

## Reproduce

Existing CNET sources and the uncommitted v3.2 fixture/cache are prerequisites.
The adapter compiles the same C source set as the arena with GCC, `-O3`, and
`-march=native`, into `var/arena_cache/retrieval_20260912/`. NumPy and `tokenizers`
are already available in the session's Python environment. The compiled index
uses ordinary experimental NumPy caches; this is not a new capsule format.

```bash
OPENBLAS_NUM_THREADS=1 python3 -m unittest discover -s experiments/vsa_retrieval -v
python3 experiments/vsa_retrieval/run.py --stage all --out var/arena_cache/retrieval_20260912/all.json
python3 experiments/vsa_retrieval/qwen_control.py
.venv-unlimited-ocr/bin/python experiments/vsa_retrieval/compile_semantic.py
python3 experiments/vsa_retrieval/semantic_eval.py
python3 experiments/vsa_retrieval/export_runtime.py
python3 experiments/vsa_retrieval/export_runtime.py --terms 1024
python3 experiments/vsa_retrieval/verify_runtime.py
python3 experiments/vsa_retrieval/verify_runtime.py --terms 1024
python3 experiments/vsa_retrieval/verify_runtime.py --extra
python3 experiments/vsa_retrieval/verify_runtime.py --terms 1024 --extra
python3 tools/cnet_vsa_arena_check.py benchmarks/vsa_routing_arena_20260911
```

If `var/arena_cache/cnet/arena_trained.lex` is absent, the existing
`make vsa_routing_arena` rebuilds it. It must match the existing frozen digest;
do not repin a different model to run this experiment. Qwen controls require
the existing raw `docs/questions/tests.f16.npy` caches. Extra-question caches
contain centroid similarities only, so extra-set multi-vector Qwen performance
is withheld; query embeddings are never inferred from similarity matrices.

Only `compile_semantic.py` needs a ROCm PyTorch/Transformers environment and
downloads a pretrained document model. It refuses a non-ROCm GPU backend. The
environment path above is the one already present on this machine; another
ROCm environment with those packages can run the same script. The offline model
is [OpenSearch document-only v3 distill](https://huggingface.co/opensearch-project/opensearch-neural-sparse-encoding-doc-v3-distill),
pinned to revision `babf71f3c48695e2e53a978208e8aba48335e3c0` (Apache-2.0).
Its published score example is checked before compilation. Source chunks
contain **only corpus train rows**. The fixed query IDF is folded into stored
postings; query processing needs only the Rust WordPiece tokenizer.

## What was measured

- Baseline v3.2 reproduction on all eight currently gated ranking blocks.
- Fixed 4/8-prototype, BM25, teacher-expanded BM25, RRF, passage, semantic
  capsule, and semantic chunk presets. No ranking weights fitted on eval labels.
- All exported score vectors and top-20 lists versus the experiment, across
  11,508 queries for each preset. Corruption, empty input, and bounds refusals.
- Encoding/tokenization plus ranking p50/p95 on one CPU thread, with Python
  overhead included. Loading, certification, and execution are separate.
- An additional previously exposed 773-question control, not a new blind set.

Selection is exploratory on previously exposed synthetic sets. Strict top-1 and
top-3 improved over v3.2, but the Mistral/Gemma held-out-content gaps to the
original Qwen remain. The original Qwen JSON/top-1 dumps are the authoritative
reference: reranking its float16 similarity cache can change ties. The separate
Qwen prototype control is labeled as a reconstruction from raw float16 caches.

The old gate passing proves its old floors remain intact. It does **not** certify
these new scores. Human-query performance, accepted-route error rates, capsule
execution correctness, and production portability remain WITHHELD.
