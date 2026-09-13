# Millisecond answer budget — 2026-09-13

User-approved operating budget: **1 ms p95 target, 5 ms p99 ceiling**, warmed
local answer path. The old 25 us budget is superseded prospectively; historical
runs remain unchanged. This is a benchmark target, not a runtime timeout.

Budget configuration: `experiments/vsa_passage_repair/answer_budget.json`.
Decision: `plans/cnet_vsa_answer_budget_20260913.md`.

The existing CLI, 812 capsules and frozen 400 questions, two passes in one
process, gives:

| Scope | Mean | p50 | p95 | p99 | Max |
|---|---:|---:|---:|---:|---:|
| Warm route + passage rank, ms | .0423 | .0428 | .0544 | .0657 | .0868 |
| First pass, ms | .3651 | .3867 | .7913 | .8791 | 1.1587 |

The first pass includes lazy capsule loads as encountered; it is not a
controlled cold filesystem-cache benchmark. CLI setup, registry admission,
network and UI are outside the measured per-query fields. Existing answers
and production runtime are unchanged by this budget change.

## Bounded quality experiment

Implemented a CPU-only, one-thread evidence-feature prototype inspired by
[K-NRM](https://arxiv.org/abs/1706.06613): 64-dimensional fixed random projection
of frozen lexicon word vectors; query/passage word dot products; 11 Gaussian
similarity bins; IDF-weighted pooling, best-match and coverage statistics.
Twenty-five new features augment the existing twelve, with the same fixed
32 depth-two trees. This does not reproduce the paper's end-to-end training.
No transformer, external model service or GPU runs in the candidate query path.

Projection bank: 5,051,904 bytes of float vectors (plus keys and Python indexing
in this prototype). Passage preparation is offline/cached. No serving memory
claim is made for all 812 prepared capsules; that was not implemented or measured.

Feature computation over all passages of the correct capsule for 768 separate
training questions measured:

| Component metric | Milliseconds |
|---|---:|
| p50 wall | .569 |
| p95 wall | .867 |
| p99 wall | 1.126 |
| max wall | 1.662 |
| mean thread CPU | .591 |

This includes canonical query/passage similarities and feature computation,
but excludes routing, tree prediction, output assembly and cache preparation.
It is **not complete answer latency**. At 100 queries/s this component's measured
CPU cost alone corresponds to about 5.9% of one core on average; this says
nothing about burst saturation, other components, or deployment concurrency.

Quality on the existing capsule-disjoint validation split (163 capsules,
806 teacher-labeled selected passage pairs):

| Candidate | Correct top-1 accepts | Wrong top-1 accepts | Correct - 2 wrong |
|---|---:|---:|---:|
| Existing 12-feature learner | 40 | 11 | 18 |
| Previous 16-feature alignment learner | 42 | 11 | 20 |
| New 37-feature interaction learner | 40 | 11 | 18 |

The fixed validation floor was .708185 logit in all three cases; it is the
larger of logit(.67) and a strict 90% rejection quantile on wrong validation
pairs. These are selected candidates on a previously used validation split,
not an independent end-to-end benchmark. No new teacher calls or exposed-q400
quality scoring were needed. The candidate failed the predeclared validation
improvement requirement and remains an offline experiment. No seal floor was
changed; no production scorer was added. More latency budget has not yet
produced better answers.

## Verification and reproduction

- `python3 experiments/vsa_passage_repair/verify_budget.py`: baseline budget.
- `python3 experiments/vsa_passage_repair/test_interaction.py`: native pooling
  matches a NumPy reference; exact versus orthogonal evidence; empty refusal;
  more than 512 native tokens handled safely; embedded NUL refused. Three tests
  pass. Initial missing-module RED is saved.
- `python3 experiments/vsa_passage_repair/interaction_eval.py`: fixed-feature
  experiment, models and validation report; requires existing local labels,
  lexicon and production capsules. NumPy/BLAS explicitly uses one thread.
- Review fixed the shared experimental token wrapper's undersized buffers
  (512 versus the native tokenizer's 1024) and keyed projection caches to the
  lexicon SHA-256. No production source changes in this increment.

Raw artifacts are in `var/passage_quality_ms_20260913/`; source and artifact
hashes are pinned in the companion JSON. No commit.
