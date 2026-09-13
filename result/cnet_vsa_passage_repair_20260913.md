# Passage repair attempt — 2026-09-13

**Outcome: incomplete.** Exact arithmetic speed improvements are implemented;
no answer-quality replacement qualified. The complete query path still exceeds
the user's maximum **25 microseconds**. No production capsule was resealed, no
certification floor lowered, and no new passage format introduced. No commit.

## Retained implementation

`src/cnet_vsa_text.c` uses an exact VNNI signed int8 dot product when the compile
target supports AVX512BW/VNNI. Unsigned bias is corrected in integer arithmetic;
other targets retain the scalar implementation. The term guard now subtracts
one word from the accumulated query, removes adjacent learned phrases, adds the
bridging phrase, then uses the original quantization and distance calculation.
It stops at the same first refusal. Queries crossing encoder token caps use the
original re-encoding path. This adds no model or persistent table.

The CLI grew from 381,616 to 390,096 bytes (+8,480 bytes). Lexicon unchanged.

Warm single-query timings on Ryzen 9 9950X, native GCC build, 812 production
capsules, second pass over the same 400 questions in one CLI process:

| Measurement (microseconds) | Before | After |
|---|---:|---:|
| Route p50 | 47.55 | 33.90 |
| Route p95 | 75.35 | 43.20 |
| Route + passage ranking p50 | 51.75 | 38.85 |
| Route + passage ranking p95 | 82.93 | 50.31 |
| Largest observed route + rank | 208.9 | 81.7 |

A prior pair measured total p95 84.42 → 57.23 us. These are measured warm
latencies, not worst-case guarantees; the 25 us requirement FAILS even at p50.
Cold lazy passage loads are excluded and can take hundreds of microseconds.
The historical 4–7 us figure measured encoding alone, a different scope.

## Answer-quality experiments — none promoted

Same exposed 400 questions, frozen local Mistral answer judgments. Net is
correct minus twice wrong. The normal accepted-route branch is the only branch
changed by the experimental statistics. The sibling path retains the complete
v4 cosine/z/zgap/term policy; absolute scores cannot reuse a z-gap threshold.

| Candidate | Answered | Correct | Wrong | Net |
|---|---:|---:|---:|---:|
| Production v4 control | 164 | 126 | 38 | 50 |
| Cosine + .2 lexical overlap, relative z | 142 | 112 | 30 | 52 |
| Absolute cosine + .2 overlap, 90% negative rejection | 236 | 158 | 78 | 2 |
| Same absolute score, 99% rejection | 228 | 156 | 72 | 12 |
| Two-passage composition | 190 | 137 | 53 | 31 |
| 32 small trees, 768 teacher-labeled questions | 151 | 125 | 26 | 73 |
| Same learner + token semantic alignment | 152 | 126 | 26 | 74 |
| Keep baseline + conservative learned rescue | 179 | 136 | 43 | 50 |

The final token-alignment candidate rescued 15 correct and 6 wrong answers, but
lost enough previously correct answers that correct coverage stayed at 126.
The conservative union preserved every baseline answer and permitted zero
additional accepts on each original negative list; it added 10 correct and
5 wrong, exactly break-even. No tested candidate increased BOTH correct
coverage and net utility. This is evidence about these candidates, not proof
that improving coverage is impossible.

The first 256-label learner showed 128 correct / 29 wrong on the exposed set,
but its separate validation accepted only 20 pairs, 12 correct and 8 wrong.
It was rejected. With 768 questions, capsule-hash split gives 605 training and
163 validation capsules, 2,949 training pairs and 806 validation pairs. The
12-feature model accepted 51 validation pairs (40 correct); the 16-feature
alignment model accepted 53 (42 correct). These are selected passage pairs,
not an independent end-to-end answer benchmark.

Training used existing external-teacher training questions, excluding exact
frozen evaluation question strings, and fresh local Mistral passage labels.
No CNET answers were training targets. The alignment graph uses 32 neighbors
per entry in the existing lexicon; the Python implementation is an offline
experiment, with no measured native serving latency. All model results are
exploratory on the already exposed 400, not new certification or evidence of
human performance. Transformer victory and human-quality claims WITHHELD.

## Calibration and checks

The C probe uses the production tokenizer, lexicon IDF and phrase encoder.
For 201 normally routed capsules, pinned 512-line negative lists reproduce
the actual v4 floor to 2e-4. Source ordering and hashes are saved because the
seal sampler depends on directory enumeration order. New experimental floors
strictly reject at least their stated fraction, including tied scores.
Cross-capsule rejection alone does not measure whether an in-capsule passage
answers a question: the absolute-score failures demonstrate this gap.

Validation completed:

- 5,276 full CLI outputs match after removing timing fields: 5,258 frozen
  questions, 12 aliens and six token-boundary prompts. Decisions, selected
  passages and printed guard statistics are unchanged.
- Exact deletion distances match legacy re-encoding on 5,261 queries and
  50,508 distances; this corpus harness excludes long queries.
- Permanent hermetic tests add full-range signed-dot parity, phrase deletion
  and bridging, repeats, identity OOV, composed-subword OOV, zero residual,
  early refusal, capacity refusal and 124/125/126/249/250/251 token cases.
- q8, answer, router and calibration gates pass. Non-VNNI build also passes.
- Prior user edits outside the four implementation/test files match the
  saved pre-task hashes; production lexicon is unchanged.

Fresh review found the token-boundary/OOV test gaps; they are addressed. No
calibration replacement or format mutation is part of the retained code.

## Reproduction and retained evidence

See `experiments/vsa_passage_repair/README.md`. Raw runs, model labels, models,
ordered negatives, judgments and pre-task source/binary snapshots are in the
ignored `var/passage_repair_20260913/` directory. Their hashes and compact
results are pinned in `result/cnet_vsa_passage_repair_20260913.json`. The
snapshot baseline is necessary to reproduce a comparison against the user's
initial dirty tree; rebuilding HEAD would be a different baseline.
