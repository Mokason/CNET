# Compact retrieval experiment — 2026-09-12

User authorized the researched retrieval experiments, emphasizing speed, size,
and efficiency. This work is isolated under `experiments/vsa_retrieval/` because
the v3.2 source, fixtures, and several unrelated changes are uncommitted.
Do not stage or overwrite that existing work. Existing `tasks/` documents refer
to other active work; this plan also carries this experiment's task checklist.

## Acceptance criteria

- Frozen v3.2 digest and centroid scores reproduce before comparing candidates.
- Only corpus `train` rows and pinned external-teacher training questions enter
  indexes. Evaluation questions, test sentences, alternates, and gold labels do
  not select prototypes, expansion terms, or ranking weights.
- CPU query path reusing existing C primitives and the installed NumPy harness.
  The semantic branch also uses the installed Rust `tokenizers` package for
  WordPiece; no neural query model or PyTorch dependency in the query command.
  Provisional budgets: 64 MiB runtime model/index data and 1 ms p95 per query
  for encoding plus ranking on one CPU thread. Python experiment overhead and
  native timings must be identified separately; no GPU speed claims.
- Report every preset, original strict top-1/top-3, recall@20, memory, latency,
  and paired wins/losses. Preserve old gates and floors. No automatic promotion.
- A new retrieval index is an experimental cache, not a new capsule format.
  Production integration requires existing verification/abstention calibration.

## Ordered tasks

1. [x] RED tests for source separation, capsule aggregation, deterministic
   compact prototypes, sparse evidence scoring, and malformed inputs.
2. [x] Minimal native adapter reusing the frozen encoder, q8 quantizer, dot
   product, and stem tokenizer; reproduce v3.2 on all reported slices.
3. [x] Compare 1/4/8 prototypes per capsule, with fixed construction settings.
   Compare Qwen using available raw caches; label missing query caches explicitly.
4. [x] Compare BM25 corpus evidence, teacher-question expansion, and their union
   with v3.2 retrieval. Reuse existing teacher data; no generation required.
5. [x] Test bounded candidate passage scoring with fixed proximity/co-occurrence
   features. Do not fit to evaluation labels. Stop adding components if they
   do not earn their measured cost.
6. [x] Review, run focused tests and unchanged arena checks, retain reproducible
   results and input/source hashes, and record whether any candidate qualifies.

Checkpoints after tasks 2, 3, and 5. The user has approved this sequence; no
additional permission is needed for the local experiment.

## Initial presets (before scores)

- Spherical clustering: k=4 and k=8, deterministic farthest-first initialization,
  eight iterations. Score max prototype cosine; also report 50/50 mixing with
  original centroid scores to test specificity versus broad capsule coverage.
- BM25: k1=1.2, b=0.75; corpus-only and corpus plus training questions.
- Fusion: equal reciprocal-rank fusion, rank constant 60, top 20 from each
  branch. Candidate scoring uses normalized term coverage and ordered adjacent
  query-term matches within individual source passages.
- Use all eight previously reported evaluation blocks, plus any available
  independent writer set. These are exposed regression distributions, not a
  fresh blind test. Broader transfer and human-query performance: WITHHELD.

## Risks

Prototype max scoring can increase false matches. Expansion can amplify generic
words or unsupported teacher associations. Candidate interactions cannot recover
missing candidates. Qwen extra-query caches contain only centroid similarities,
so reconstructing arbitrary query vectors from them would be invalid. Any
comparison unavailable without re-embedding must be reported as unavailable.

No claim about certified execution, answer correctness, or production wrong
accepts follows from an uncalibrated retrieval experiment.

## Checkpoint: first retrieval experiments

Baseline reproduced on all eight blocks. Multiple prototypes regress. Corpus
plus teacher expansion, equal RRF, and a 20% within-passage feature improve the
old distributions, but the three content-transfer blocks still trail Qwen.
The passage feature is fixed at 75% term coverage plus 25% ordered pair coverage;
it is not a negation interpreter. Its result is combined with normalized RRF at
20%/80%, chosen before the passage run.

Next offline semantic-index control uses the public Apache-2.0 OpenSearch
document-only v3 distill model, revision
`babf71f3c48695e2e53a978208e8aba48335e3c0`. It processes only train sources on
AMD/ROCm. Query inference is tokenization plus precomputed postings; the 67M
parameter model remains an offline build dependency. Presets fixed before
scoring: max-pool document chunks within each capsule; retain 256 or 1024 terms
per capsule, with the model's fixed query IDF folded into postings. Compare
standalone, equal RRF with v3.2, and equal three-way RRF with expanded BM25.
These are model adaptations, not claims of reproducing OpenSearch benchmarks.

## Checkpoint: semantic capsule pooling

The publisher's reference example reproduced (11.11048 versus 11.1105).
Compilation of 5,757 train-only chunks took 8.4 seconds on ROCm, excluding model
download/load and source tokenization. Capsule-level semantic max pooling plus
three-way fusion and passage features improves all eight top-1 blocks over v3.2;
two held-out writer blocks still trail the frozen Qwen centroid.

One final structural control retains semantic chunks separately, rather than
taking each term's maximum across unrelated chunks. Retain 128/256 terms per
chunk, sum matching term weights within a chunk, then max-score its owning
capsule. Same pretrained model, inputs, IDF, RRF, and 20% passage feature; no
fitting to evaluation labels. Both memory outcomes will be reported, including
any budget failures. This tests evidence coherence, not a new writer or model.

## Final selection and integration boundary

Two opt-in experimental caches are exported: 256 terms/capsule (56.14 MiB data)
and 1024 terms/capsule (60.98 MiB data), both including the existing lexicon,
centroids, expanded lexical index, source-passage features, and tokenizer assets.
The smaller preset is the CLI default. Each reproduced all 11,508 evaluated
query score vectors and top-20 lists after export. Both improve all eight
v3.2 top-1 and top-3 blocks. Neither beats the original Qwen on every remaining
set, and Qwen's own multi-vector control raises its held-out sentence baseline.

Do not promote into the certified router. A fresh qualification set, real
verifier/abstention calibration, portability checks through the existing capsule
manifest/export path, and native deployment RSS remain follow-up requirements.
The 64 MiB budget applies to runtime data, not the Python interpreter or test
process. CLI process RSS and verification-process peak RSS are reported
separately. Nothing about a ranking score confers certification.

Final record: `result/cnet_vsa_retrieval_20260912.md` and matching JSON.
Twelve focused tests passed; both exports replayed 11,508 score vectors and
top-20 lists. The unchanged arena check passed 48 frozen values and 28 claims.
All 49 protected baseline inputs retained their original SHA-256 hashes.
The additional 773-question control improved to 26.52% / 26.39% strict top-1
from 22.12%, still below Qwen at 27.55%. No production promotion or commit.
