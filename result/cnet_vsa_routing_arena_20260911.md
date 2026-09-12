# Frozen routing arena: CNET encoders vs transformer embeddings (2026-09-11)

Gate: `make vsa_routing_arena` (marker CNET_VSA_ROUTING_ARENA_PASS). Fixture: `benchmarks/vsa_routing_arena_20260911/`
(1068 corpora, 2136 held-out test sentences, 5258 teacher-written questions over 1052 corpora, 12 alien queries), sha256 in manifest.json.
The CNET side is `tools/cnet_vsa_arena.c` (production int8 representation, lexicon rebuilt from train rows only);
the transformer side is `tools/cnet_vsa_arena_transformer.py` with small caches (centroids, query embeddings,
per-corpus distance lists) committed beside the fixture so the numbers re-score without any model.
Every number below is reproduced by the gate within tolerance 0.01 against expected.json.

## Protocol

- Test sentences: the last two sentences of every corpus, never used for centroids, lexicon, or calibration.
- Questions: five per corpus written by the teacher (Mistral-Small-3.2-24B, served as 'held') from train sentences only.
- Nearest corpus: a query is scored against all corpus centroids (mean of train sentence vectors); top-1 / top-3 exact.
- Separability: per corpus, leave-one-out distance of each train sentence vs 512 seeded negatives from other corpora; the same rule the seal path uses.
- Aliens: 12 off-registry questions; z = (best cosine - mean) / sd over the centroid population, what a margin gate sees (lower is better).
- Transformers embed with their documented prefixes (nomic: search_document/search_query; Qwen3: instruction on queries; gemma: none).

## Encoder-level results

| system | test top-1 | test top-3 | questions top-1 | questions top-3 | separable 0.80/0.90 | separable 0.90/0.95 | alien z | encode per text | footprint |
|---|---|---|---|---|---|---|---|---|---|
| CNET stem hash, int8-2048 (shipped default) | 23.2% | 37.5% | 37.5% | 56.6% | 81.0% | 21.8% | 4.71 | 7.2 us CPU | 2 KB/capsule + 0 |
| CNET lexicon (Random Indexing + all-but-the-top), int8-2048 | 28.9% | 45.2% | 45.5% | 67.1% | 92.9% | 39.3% | 3.56 | 3.7 us CPU | 2 KB/capsule + 22 MB table |
| CNET lexicon + distilled context (Qwen3-4B word vectors blended offline, alpha 0.5, beta 1.0, 16 PCs), int8-2048 | 29.3% | 45.6% | 46.8% | 69.7% | 94.8% | 48.8% | 3.77 | 4.2 us CPU | 2 KB/capsule + 22 MB table |
| CNET lexicon v3 full: + 8192 learned phrases + learned subword backoff | 30.1% | 46.4% | 48.2% | 70.8% | 96.7% | 56.6% | 4.06 | 6.9 us CPU | 2 KB/capsule + 40 MB table |
| CNET lexicon v3 trained: + supervised pass on 16 corpus sentences/corpus and 5,326 disjoint teacher questions (superseded) | 33.5% | 49.6% | 60.0% | 78.9% | 97.8% (partly on trained sentences) | 60.8% | 3.87 | 4.9 us CPU | 2 KB/capsule + 40 MB table |
| CNET lexicon v3.1 trained: mixed-register training families + contrast pairs with explicit negatives, 16 sentences/corpus (superseded) | 34.5% | 49.6% | 63.0% | 81.4% | 97.8% (partly on trained sentences) | 60.8% | 3.9 | 4.9 us CPU | 2 KB/capsule + 40 MB table |
| CNET lexicon v3.2 trained (frozen row `lex_trained`): v3.1 + the training-register questions in the lexicon build corpus (everyday vocabulary coverage) | 33.7% | 49.4% | 64.2% | 82.7% | 97.9% (partly on trained sentences) | 60.9% | 3.9 | 5 us CPU | 2 KB/capsule + 41.5 MB table |
| gemma4 26B MoE Q4, mean-pooled hidden states, 3840-d (decoder, not embedding-trained) | 9.6% | 16.8% | 8.9% | 15.7% | 2.6% | 0.1% | 2.72 | cached | 7.4 GB weights + GPU |
| nomic-embed-text v1.5, 137M params, 768-d (transformer, embedding-trained) | 28.7% | 44.9% | 46.9% | 67.6% | 75.8% | 24.1% | 3.86 | cached | 146 MB weights + GPU |
| Qwen3-Embedding-4B, 4.0B params, 2560-d (transformer, embedding-trained) | 37.6% | 56.8% | 56.0% | 78.2% | 89.9% | 47.9% | 4.96 | cached | 4.3 GB weights + GPU |

## Router-level results on the disjoint test slice (capsules 120-239 of bin/, teacher questions as queries)

Full production router: calibrated radius, margin gate, ambiguity gate k=2, wide int8 space; capsules resealed from
corpora minus the two test sentences; two questions per capsule are queries, the remaining questions are calibration
probes; negatives from a globally trimmed copy of every corpus. Score = correct - 2 * wrong; abstain = 0.

| encoder | sealed | queries | accept | correct | wrong | abstain | top-1 self | score | alien accepted |
|---|---|---|---|---|---|---|---|---|---|
| stem | 104/120 | 208 | 0.524 | 0.471 | 0.053 | 0.476 | 0.678 | 76 | 0/11 |
| lex | 113/120 | 226 | 0.553 | 0.518 | 0.035 | 0.447 | 0.761 | 101 | 0/11 |
| lex + distilled | 117/120 | 234 | 0.466 | 0.453 | 0.013 | 0.534 | 0.756 | 100 (98 net, one alien accepted at -2) | 1/11 |
| lex v3 full (phrases + subwords) | 119/120 | 238 | 0.513 | 0.487 | 0.025 | 0.487 | 0.756 | 104 (102 net, one alien accepted at -2) | 1/11 |
| lex v3 trained (16 sentences/corpus + disjoint questions) | 118/120 | 236 | 0.534 | 0.521 | 0.013 | 0.466 | 0.809 | 117 | 0/11 |
| lex v3.1 trained (mixed registers + contrast pairs) | 119/120 | 238 | 0.559 | 0.550 | 0.008 | 0.441 | 0.840 | 127 | 0/11 |

Post-freeze check (2026-09-12, `frozen_model.json` pins every rebuilt table's digest; 88 questions written afterwards
in a colloquial, non-teacher register by the assistant, not a human): with v3 training, Qwen3-4B 47.7% / 75.0%
top-1 / top-3 against the trained lexicon's 35.2% / 64.8%: the lead was bound to the teacher's question style.
v3.1 (same night) retrained on mixed-register paraphrase families with contrast pairs, and v3.2 added the
training-register questions to the lexicon build corpus: the 88 became a regression set (55.7% vs Qwen 47.7%,
one-sided floor in expected.json), and two untouched sets from held-out paraphrase families were added:
colloquial evaluation (1,877; trained 57.1% vs Qwen 49.4%) and contrast evaluation (549; 33.7% vs 32.1% top-1
within noise, 52.8% vs 67.9% top-3, recorded loss). Lenient labels from `alternates.tsv` apply to every
system. An independent set written after the freeze by Mistral-Small-3.2-24B in everyday register (773 from held-out
content, 400 from unused in-corpus content) reverses the verdict there: Qwen3-4B 27.6 / 37.5 vs the trained lexicon
22.1 / 24.8 strict top-1. A cross-writer experiment (training families from Mistral and gemma, each evaluated on the other) then
showed the lead is bound to the CONTENT of the training pairs, not their register: a writer's own register
moves its held-out-content set by one point. Recorded losses on all three unseen-writer sets. Evidence:
result/cnet_vsa_lexicon_register_training_20260912.md. Human-written set: `questions_human.tsv`.

Transformers have no calibrated radius, margin, or ambiguity gate, so no router-level row exists for them:
the encoder-level table is the like-for-like comparison, the router-level table is what CNET ships.

## Verdict

The gate asserts exactly these orderings (expected.json "claims"), each re-checked on every run:

1. Separability at the shipped 0.80/0.90 policy: lexicon 92.9% >= Qwen3-Embedding-4B 89.9% >= nomic 75.8% >= gemma mean-pooled 2.6%.
2. Ranking parity with the small embedding transformer: lexicon 45.2% vs nomic 44.9% (held-out sentences, top-3); 67.1% vs 67.6% (questions, top-3), within tolerance.
3. Lexicon over the shipped hash default on questions: 45.5% vs 37.5% top-1.
4. A recorded loss: Qwen3-Embedding-4B leads the lexicon on held-out sentence top-1, 37.6% vs 28.9% (and on question top-1, 0.90/0.95 separability). This claim is kept in the gate so that the day it stops holding is a measured event, not an announcement.
7. Lexicon v3.1 (2026-09-12, three more claims and three regression floors): the retrained table >= Qwen3-4B on the 88-question regression set (52.3 vs 47.7) and on the untouched colloquial evaluation set (56.4 vs 49.4); recorded loss on the contrast evaluation set's top-3 (51.9 vs 67.9). Floors: lex_trained's top-1 on the three post-freeze sets may rise but not fall. The lex_trained row's frozen question values moved to 63.0 / 81.4 (refrozen, digest re-pinned).
6. Lexicon v3 (2026-09-12, nine more claims): phrases + subword backoff >= the distilled lexicon on question top-1 (48.2 vs 46.8) and separability (96.7 vs 94.8), and >= Qwen3-4B on both separability policies (96.7/56.6 vs 89.9/47.9); the trained table >= Qwen3-4B on evaluation-question top-1 (60.0 vs 56.0) and top-3 (78.9 vs 78.2), the questions never having been trained on; two recorded losses to Qwen3-4B on held-out sentence top-1 (33.5 vs 37.6) and top-3 (49.6 vs 56.8). The trained row's separability is not claimed (partly measured on training sentences). Evidence: result/cnet_vsa_lexicon_phrases_subwords_training_20260912.md.
5. Distillation (added later the same night, six more claims): the distilled lexicon >= Random Indexing on question top-1 (46.8 vs 45.5) and separability (94.8 vs 92.9); >= nomic on question top-3 (69.7 vs 67.6); >= Qwen3-4B on 0.80/0.90 separability (94.8 vs 89.9); and two recorded losses to Qwen3-4B on held-out sentence top-1 (29.3 vs 37.6) and question top-1 (46.8 vs 56.0). Its 0.90/0.95 separability against Qwen3-4B (48.8 vs 47.9) is inside the tolerance and not claimed.

So the official sentence is: on in-domain topic routing over this registry, a 22 MB learned lexicon over
2 KB int8 centroids matches a 137M-parameter embedding transformer on ranking, beats a 4B-parameter
one on the separability the router actually gates on, costs 2 to 4 microseconds per query on CPU
against milliseconds on a GPU, and loses to the 4B model on ranking accuracy by about nine points.
Raw decoder hidden states (gemma 26B, mean-pooled) are not a usable embedding at all: 9.6% top-1.

Not claimed: any transformer comparison on generation, question answering, or open-ended language.

## Closing the ranking gap without a runtime transformer

The 4B model's nine-point lead is meaning the co-occurrence sweep cannot learn from 770k tokens:
it knows that "quench" and "cooldown" share contexts here, not that they are synonyms in general.
The remedy that keeps the runtime identical is to let a transformer write the lexicon's context
component once, offline: embed each vocabulary word (and frequent bigrams) with the 4B model,
reduce with PCA, project into the 2048-d space with a fixed random matrix, and mix with the hash
identity exactly as the Random Indexing signature is mixed today. Model2Vec showed this shape of
distillation, PCA plus Zipf weighting of static per-token vectors averaged at inference, keeps most
of a sentence transformer's quality at 500x the speed and 15x smaller ([Model2Vec](https://huggingface.co/blog/Pringled/model2vec),
[Minish](https://minish.ai/packages/model2vec/introduction/)). For this registry the cost is 10,911
words through the 4B model once (about 100 s on the GPU), a table that stays 22 MB, and no change
to the router. The anti-collapse rule holds: the supervision is an external model, applied at
learning time, recorded by digest. Whether it closes the gap is a question for this same gate.

Measured (same night, `lex_distilled` row above, reproduced offline by the gate from a tracked
PCA cache): it does not close the gap. Separability +1.9 / +9.5, question top-3 +2.6, question
top-1 +1.3, held-out top-1 +0.4; the 4B model still leads ranking by 8 to 9 points. The failure
analysis and the remaining levers (OOV backoff, distilled phrase entries, frontier dedup,
contrastive training of the static table) are in result/cnet_vsa_lexicon_distillation_20260911.md.
