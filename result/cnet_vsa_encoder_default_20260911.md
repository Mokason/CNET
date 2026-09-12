# VSA topical encoder selection by registry-wide sweep (2026-09-11, afternoon)

Gate: `make cnet_vsa_encoder_sweep_bench` (marker CNET_VSA_ENCODER_SWEEP_BENCH_PASS;
SKIP when var/distill is absent). The gate runs the seal path's own calibration
(`cnet_vsa_gencap_calibrate_ex`) over every corpus and every encoder, and refuses a
default that is worse than BAG at the 0.80/0.90 policy or more than 4x slower to encode.

Re-run green after the change: calibration, gencap, router (now routes by prompt so
version-1 bag capsules and new stem capsules share one registry), arena, text, CLI (28/28).

## Why this exists

The HD encoder (char-trigram words + 0.6 bound bigrams) was made the default on the
strength of a hermetic 6-sentence pair (gap 0.200 -> 0.236). On the registry it broke
the CLI bench (an uncalibrated capsule refused its own in-domain prompt: dist 0.931 >
0.900) and, measured on 1068 corpora, separated 5.5% of them where BAG separated 51%.
Cause: char-trigram vectors share a common component (cosine 0.246 with a bundle of 20
unrelated words; hashed words 0.0009), and bound bigrams contribute nothing to a
paraphrased query. The fix was to make the sweep the arbiter, and to add cheaper
candidates.

## Sweep over var/distill (1068 corpora, leave-one-out, 512 negatives each, no probes)

| encoder | 0.90/0.95 | 0.80/0.90 | 0.70/0.90 | median achievable accept @reject 0.90 | encode us/query |
|---|---|---|---|---|---|
| bag     |  77 ( 7.2%) | 546 (51.1%) | 880 (82.4%) | 0.806 |  6.94 |
| hd      |   3 ( 0.3%) |  59 ( 5.5%) | 170 (15.9%) | 0.529 | 74.53 |
| stem    | 117 (11.0%) | 660 (61.8%) | 922 (86.3%) | 0.831 |  7.24 |
| stem_bi | 111 (10.4%) | 634 (59.4%) | 912 (85.4%) | 0.828 | 14.26 |
| cgram_c |  34 ( 3.2%) | 281 (26.3%) | 580 (54.3%) | 0.714 | 40.89 |

## Sweep over var/distill_isekai (288 corpora, 200 with held-out teacher probes; negatives are sibling Isekai topics)

| encoder | 0.90/0.95 | 0.80/0.90 | 0.70/0.90 | median achievable accept @reject 0.90 | encode us/query |
|---|---|---|---|---|---|
| bag     | 3 (1.0%) | 39 (13.5%) |  99 (34.4%) | 0.620 |  7.20 |
| hd      | 0 (0.0%) |  8 ( 2.8%) |  21 ( 7.3%) | 0.449 | 75.62 |
| stem    | 5 (1.7%) | 60 (20.8%) | 114 (39.6%) | 0.651 |  7.36 |
| stem_bi | 4 (1.4%) | 62 (21.5%) | 118 (41.0%) | 0.659 | 14.47 |
| cgram_c | 3 (1.0%) | 40 (13.9%) |  83 (28.8%) | 0.589 | 40.12 |

## Held-out routing on a 120-capsule slice (reseal from corpus minus last 2 sentences; those 2 are the queries)

Tool: `tools/cnet_vsa_route_eval.py --dir bin --limit 120 --reseal-heldout DIR --encoder <e>`.
Score = correct - 2 * wrong; abstain = 0. Capsules refused by calibration at 0.80/0.90 are not in the registry.

| encoder | sealed | in-domain n | correct | wrong | abstain | top-1 self | score | alien accepted |
|---|---|---|---|---|---|---|---|---|
| bag  | 64/120 | 128 | 0.328 | 0.070 | 0.602 | 0.531 | +24 | 0/11 |
| stem | 71/120 | 142 | 0.373 | 0.092 | 0.535 | 0.563 | +27 | 1/11 |

The one alien accepted under stem: | How do I fold and shape a sourdough loaf for an open crumb? | computer_vision_and_shape_analysis | 0.822 | 4.56 | ACCEPT | ACCEPT |
The two registries differ in membership (stem seals 7 more capsules), so the rows are
the operating points of each encoder, not a paired comparison on identical capsules.

## Decision

`CNET_VSA_ENCODER_DEFAULT = CNET_VSA_ENCODER_STEM`: conservative suffix stripping
(plural, -ing, -ed, -ly, y->i) before hashing. +10.7 points over BAG on the general
registry, +7.3 on the Isekai registry, +4% encode cost, no heap traffic, no tables.
STEM_BI is within noise of STEM at twice the cost; HD and CGRAM_C stay available by
name for experiments only. Capsules record their encoder in the digest-covered
receipt, and routing by prompt scores each capsule with its own encoder, so the
existing 824 bag capsules keep working unchanged.

The Isekai registry separates poorly under every encoder because its negatives are its
own siblings: the frontier has no semantic dedup, so many capsules are near-duplicates.
That is a crawler topology problem, not an encoder problem; do not answer it by
lowering targets.

## Fixed in the same pass

- `gencap-gen` encoded the prompt with the bag encoder regardless of the capsule's
  encoder; it now uses `cnet_vsa_gencap_encoder_id(cap)`. Without this the crawler's
  in-domain gate would have failed on every non-bag capsule.
- Encoder ids are validated through one registry (`cnet_vsa_encoder_valid/name/parse`)
  instead of two hard-coded values; the CLI accepts `--encoder default|bag|hd|stem|stem_bi|cgram_c`.
- `tools/cnet_vsa_route_eval.py --reseal-heldout DIR` reseals from corpora minus the
  last two sentences so in-domain queries are genuinely held out; the plain mode now
  says its corpus queries are optimistic.

WITHHELD: generated-text quality; transformer baseline; open-ended language.

## Adversarial review of this change set (75-agent workflow: 6 lenses, 3 refuters per finding)

23 raw findings, 10 confirmed, 13 refuted. All 10 fixed and gates re-run green
(stem, sweep, calibration, gencap, router, arena, text, CLI 28/28, verify-fast).

| # | severity | fix |
|---|---|---|
| 1 | high | `cnet_vsa_gencap_load` now refuses out-of-range `ngram.dim`, vocab/transition/frame counts and unknown encoder ids before the digest walk (a crafted or corrupt file could read gigabytes past the struct, and dispatch sized 512-float stack buffers from the file's dim). `cnet_vsa_text_encode_topical` rejects dim > 512. |
| 2 | low | sweep bench freed only the truncated corpus range under CNET_VSA_SWEEP_MAX |
| 3 | medium | CLI negatives loader excluded every sibling whose name started with the capsule name (35 of 1068 lost their hardest negatives); now exact-name exclusion |
| 4 | medium | `--reseal-heldout` passed the two probe lines it later queried into calibration; now trimmed |
| 5 | low | 0.80f * n rounded up at every multiple of 5; targets snapped to 1e-4 in double in library and bench |
| 6 | low | ingest added a sentence to the centroid only if the n-gram engine took it, but calibrate subtracted every sentence leave-one-out; centroid now takes every encodable sentence |
| 7-10 | medium/low | stemmer over-stemmed -ly (apply -> app), had no -eed guard, undoubled 3-letter stems (add/added), and never dropped silent e (encode vs encod). Rewritten as Porter steps 1a/1b/1c/2-adverbial/5 with `make cnet_vsa_stem_bench` as a hermetic gate |

Sweep after the fixes: stem 661/1068 (61.9%) vs bag 556 (52.1%) at 0.80/0.90; stem 7.18 us vs bag 6.99 us per query. Default unchanged.

Refuted (kept as notes, not defects): routing cost is ~200 us for 824 capsules with a scalar double dot loop (see result/cnet_vsa_quasi_orthogonal_study_20260911.md for the binary-centroid path that fixes this); route_query's ~12 KB stack; lazy static init of the trigram stoplist without a lock (single-threaded runtime).
