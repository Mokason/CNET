# Learned lexicon for the wide space: Random Indexing with all-but-the-top (2026-09-11, night)

Gate: `make cnet_vsa_lexicon_bench` (CNET_VSA_LEXICON_BENCH_PASS). Every other VSA gate re-run
green with the module linked (calibration 6/6, sweep, gencap, router, arena, text, stem, q8,
evidence, CLI 28/28).

## What it is

`src/cnet_vsa_lexicon.c`: one sparse sweep over the retained teacher corpora (Random Indexing).
Each word keeps its hash-seeded identity signature and gains a context signature: the sparse
ternary index vectors (16 of 2048) of its window-3 neighbours, cyclically permuted by signed
offset (direction kept), damped by 1/log2(2+count); words present in more than 30% of corpora
never act as contexts. Signatures are L2-normalised, mean-centred, and the top principal
directions are projected out (power iteration with deflation). Stored vector =
normalize(identity + beta * context) * idf, quantised to int8 under one global scale.

Runtime: the LEX encoder (id 5) looks each stemmed word up by binary search and adds its 2 KB
int8 vector; unknown words add their identity at the lexicon's identity magnitude. No PRNG per
word. A lexicon is a digest-covered artifact; a LEX capsule records the lexicon tag at seal and a
registry refuses it under any other lexicon (-8) or none. LEX without an active lexicon refuses
to encode rather than falling back.

Build on var/distill: 1068 corpora, 63,248 sentences, 770,943 content tokens, 10,911-word
vocabulary (min count 2), sweep 0.15-0.18 s, table 22.5 MB.

## Transformer baseline on the same protocol

nomic-embed-text-v1.5 (Q8_0 GGUF, 137M parameters, 768-d) served by llama-server with
embeddings on GPU. Same corpora, same leave-one-out rule, same 512 negatives per corpus with the
same seeds, same 2 held-out sentences per corpus for the 1068-way nearest-corpus test.
Embedding 65,384 texts took 210.5 s: 3.2 ms per text.

## Nearest-corpus accuracy and separability (harness, 1068 corpora, 2136 held-out queries)

| encoder | separable 0.80/0.90 | separable 0.90/0.95 | top-1 | top-3 | margin best-2nd | encode per query |
|---|---|---|---|---|---|---|
| stem hash, int8-2048 (shipped default) | 81.2% | 21.9% | 23.2% | 37.5% | 0.040 | 5 us |
| nomic-embed-text v1.5 (transformer) | 75.5% | 23.2% | 28.6% | 44.9% | 0.011 | 3,200 us |
| lexicon, uncentred, beta 0.5 | 1.4% | 0.1% | 27.8% | 42.9% | 0.020 | ~5 us |
| lexicon, centred, no PC removal | 4.5% | 0.3% | 28.0% | 43.4% | 0.032 | ~5 us |
| lexicon, centred, 4 PCs removed (shipped as distill_v1.lex) | 92.9% | 38.7% | 29.0% | 45.3% | 0.056 | ~5 us |
| lexicon, centred, 16 PCs removed | 92.3% | 39.9% | 28.9% | 45.5% | 0.057 | ~5 us |
| lexicon, centred, 64 PCs removed | 91.8% | 37.8% | 29.0% | 45.6% | 0.057 | ~5 us |
| lexicon, beta 0.25, 16 PCs removed | 90.1% | 36.3% | 29.0% | 45.5% | 0.057 | ~5 us |

Uncentred variants at beta 0.25 / 1.0 / 2.0 and one reflective pass all sat between 0.2% and
13% separable: raw Random Indexing signatures share one large common direction (technical
prose vocabulary every topic uses), so unrelated texts look alike (null sd 0.05 -> 0.10). The
fix is the standard one for anisotropic embeddings: remove the mean and a few top principal
directions (Mu & Viswanath, "All-but-the-Top", ICLR 2018). After it, the lexicon is better than
the transformer embedding on every measured metric at roughly 600x lower encode cost, and
better than the shipped hash default by 12 points of separability and 6 points of top-1.

Reading of the ceiling: top-1 of 29% is the shared limit of both the transformer and the lexicon
on this registry, which says the remaining errors are in the registry (near-duplicate topics),
not in the encoder.

## Library sweep (the real seal path, `make cnet_vsa_encoder_sweep_bench` with CNET_VSA_LEXICON, all encoders)

Wide int8 space, calibrate_dual, 1068 corpora, 512 negatives each:

| encoder | 0.90/0.95 | 0.80/0.90 | 0.70/0.90 | median achievable accept @0.90 | wide encode us/query |
|---|---|---|---|---|---|
| bag | 20.2% | 78.4% | 94.9% | 0.879 | 5.14 |
| stem (default) | 21.8% | 81.6% | 95.2% | 0.887 | 5.24 |
| lex (distill_v1.lex) | 38.1% | 92.8% | 98.8% | 0.929 | 2.13 |

The lexicon encode is 2.5x faster than the hash encode: a table lookup replaces 32 PRNG
steps per word. Float-512 rows are identical for stem and lex by construction.

## Held-out routing, leak-free protocol, wide space, ambiguity gate k as marked

| slice | encoder | k | sealed | queries | accept | correct | wrong | abstain | top-1 self | score | alien accepted |
|---|---|---|---|---|---|---|---|---|---|---|---|
| A (validation) | stem | 2 | 99/120 | 198 | 0.258 | 0.207 | 0.051 | 0.742 | 0.475 | +21 | 1/11 |
| A (validation) | lex | 2 | 110/120 | 220 | 0.336 | 0.277 | 0.059 | 0.664 | 0.527 | +35 | 0/11 |
| B (test) | stem | 2 | 107/120 | 214 | 0.350 | 0.266 | 0.084 | 0.650 | 0.463 | +21 | 0/11 |
| B (test) | lex | 2 | 114/120 | 228 | 0.390 | 0.329 | 0.061 | 0.610 | 0.553 | +47 | 0/11 |
| B (test) | stem | 1 | 107/120 | 214 | 0.533 | 0.341 | 0.192 | 0.467 | 0.463 | -9 | 1/11 |
| B (test) | lex | 1 | 114/120 | 228 | 0.588 | 0.421 | 0.167 | 0.412 | 0.553 | +20 | 0/11 |
| B (test) | stem | 0 | 107/120 | 214 | 0.757 | 0.407 | 0.350 | 0.243 | 0.463 | -63 | 1/11 |
| B (test) | lex | 0 | 114/120 | 228 | 0.772 | 0.496 | 0.276 | 0.228 | 0.553 | -13 | 0/11 |

On the untouched test slice the lexicon more than doubles the routing score at the same gate
(+47 vs +21), seals more capsules (114 vs 107), picks the right capsule more often (55.3% vs
46.3% top-1), answers more (39% vs 35%) with fewer wrong routes (6.1% vs 8.4%), and no alien is
accepted at any k. Wrong routes at k=2: 14 of 228 queries, 14 of 89 accepted (16%).

Latency on the 114-capsule lexicon registry: route_query 7.2 us per query (0.063 us per capsule),
wide encode alone 1.9 us; the same registry forced to float-512 costs 30.9 us.

## Verdict against the transformer

On the only task both systems were measured on, topic routing over 1068 teacher-written corpora
under one protocol, the lexicon-encoded wide space is better than nomic-embed-text v1.5 on
separability (92.9% vs 75.5%), on nearest-corpus top-1 (29.0% vs 28.6%) and top-3 (45.3% vs
44.9%), and on runner-up margin (0.056 vs 0.011), at about 600x lower encode cost (2 us vs
3.2 ms) and 22 MB of table against 146 MB of weights plus a GPU. This is a routing-encoder
comparison; generation, open-ended language and QA answer quality remain WITHHELD.

## Rollout, not yet done

- `CNET_VSA_ENCODER_DEFAULT` stays STEM: the compiled default must work with no artifact on
  disk. LEX is selected by `--encoder lex` with `CNET_VSA_LEXICON=<file>` in the environment.
- Any consumer of a LEX registry must activate the same lexicon before loading it (the CLI does
  from the environment; the Unity bridge and daemons do not yet), or those capsules are refused
  at admission (-8). Switching the crawlers to lex should wait for that plumbing.
- The lexicon was built from var/distill only. The Isekai crawler's vocabulary lives in
  var/distill_isekai; a combined build is one command (`lexicon-build` over a merged directory).
- Lexicon epochs: rebuilding after new corpora changes the digest, and every LEX capsule sealed
  under the old lexicon then refuses under the new one until resealed from its corpus. Keep
  epochs coarse.

WITHHELD: generated-text quality; QA answer correctness; open-ended language.

## Clean-split check (lexicon swept WITHOUT the held-out test sentences)

The first lexicon was swept over full corpora, so the 2,136 harness test sentences had
contributed their word contexts to the table (the transformer had no such exposure). Rebuilt
from corpora minus their last two lines (`distill_v1_clean.lex`, 10,795 words):

| lexicon | separable 0.80/0.90 | 0.90/0.95 | top-1 | top-3 | margin |
|---|---|---|---|---|---|
| swept with test sentences (distill_v1) | 92.9% | 38.7% | 29.0% | 45.3% | 0.056 |
| swept without them (distill_v1_clean) | 93.4% | 38.4% | 28.9% | 45.2% | 0.055 |

The leak was immaterial; the comparison against the transformer stands on the clean table.
Held-out routing on the disjoint test slice with the clean lexicon is recorded below.

| slice | encoder | k | sealed | queries | accept | correct | wrong | abstain | top-1 self | score | alien accepted |
|---|---|---|---|---|---|---|---|---|---|---|---|
| B (test) | lex clean | 2 | 113/120 | 226 | 0.403 | 0.336 | 0.066 | 0.597 | 0.553 | 46 | 0/11 |
