# Capsule format v3: wide int8 topical block (2026-09-11, evening)

Gates: `make cnet_vsa_calibration_bench` (6/6, section [6/6] covers v3),
`make cnet_vsa_encoder_sweep_bench` (dual-space gate), plus stem, text, gencap, router,
arena, CLI (28/28) re-run green after each step below.

## What was built, in order, and what the measurements did to it

1. Sign-bit block, 8192 bits (1 KB), popcount routing. Separability over 1068 corpora
   at 0.80/0.90 rose from 61.9% (float-512) to 82.7%. Then the held-out routing run on
   99 resealed capsules scored -54 (correct 38%, WRONG 33%) against +27 for float:
   the wide space accepted 71% of in-domain queries instead of 47%, and most of the
   extra acceptances went to a sibling capsule. Nearest-corpus accuracy across all
   1068 corpora (2 held-out sentences each, 1068-way) explains it:

   | centroid | width | bytes | top-1 | top-3 | mean margin |
   |---|---|---|---|---|---|
   | float | 512 | 2048 | 19.7% | 33.0% | 0.0380 |
   | float | 2048 | 8192 | 23.2% | 37.5% | 0.0400 |
   | int8 | 2048 | 2048 | 23.2% | 37.5% | 0.0400 |
   | binary | 2048 | 256 | 20.0% | 33.7% | 0.0282 |
   | int8 | 4096 | 4096 | 23.3% | 38.2% | 0.0403 |
   | binary | 8192 | 1024 | 21.5% | 36.5% | 0.0289 |
   | float | 8192 | 32768 | 23.7% | 38.6% | 0.0406 |

   Sign bits discard the centroid magnitudes that separate siblings. int8 quantisation
   (rescale to max-abs, round) is lossy on the vector, but showed no aggregate top-1/top-3
   routing loss against float at any width in this experiment. The sign-bit layout was retired; any file carrying it
   refuses to load (width/kind check before the digest) and the 12 crawler capsules that
   had it were resealed from their corpora (10 resealed, 2 now refused as not separable).

2. Final block: 2048-d int8 centroid (a 2 KB payload, the same bytes as the float-512
   centroid payload; the persisted file grows by 2176 bytes over v1 because the v2 receipt
   and the v3 block are appended, the float centroid is retained, total 5,600,640 bytes),
   quantised from the sum of L2-normalised wide sentence vectors (variant A; raw word
   sums and two-prototype centroids were measured and lost: 83.5% / 82.5% / 78.1%
   separable at 8192 bits). Query = signed word counts saturated to int8, cosine via
   int32 dot with precomputed norms.

   Sweep over 1068 corpora, stem encoder (`make cnet_vsa_encoder_sweep_bench`):

   | space | 0.90/0.95 | 0.80/0.90 | 0.70/0.90 | median achievable accept @0.90 | encode us/query |
   |---|---|---|---|---|---|
   | float-512 | 10.8% | 61.9% | 87.0% | 0.831 | 7.4 |
   | wide int8-2048 | 21.8% | 81.6% | 95.2% | 0.887 | 5.4 |

3. Ambiguity gate. Held-out A/B on the SAME 101 resealed capsules (202 held-out
   sentences, 11 aliens), score = correct - 2 * wrong:

   | space | k | accept | correct | wrong | abstain | top-1 self | score | alien accepted |
   |---|---|---|---|---|---|---|---|---|
   | wide int8 | 0 | 0.609 | 0.347 | 0.262 | 0.391 | 0.480 | -36 | 1/11 |
   | wide int8 | 1 | 0.406 | 0.267 | 0.139 | 0.594 | 0.480 | -2 | 1/11 |
   | wide int8 | 2 | 0.243 | 0.193 | 0.050 | 0.757 | 0.480 | +19 | 1/11 |
   | float-512 | 0 | 0.356 | 0.228 | 0.129 | 0.644 | 0.416 | -6 | 0/11 |
   | float-512 | 1 | 0.257 | 0.188 | 0.069 | 0.743 | 0.416 | +10 | 0/11 |

   The wide space picks the right capsule more often (48.0% vs 41.6% top-1) and, gated
   at k = 2, has the fewest wrong ROUTES of any configuration: 10 of 202 queries (5.0%),
   which is 10 of the 49 accepted (20.4%). These are routing labels; generated-answer
   correctness was not evaluated. k was chosen on this same 101-capsule slice, so this
   table is validation data for that choice; a disjoint test slice is reported below. Defaults: wide k = 2.0,
   float k = 0 (on the legacy 824-capsule registry k >= 1 refuses keyword queries whose
   runner-up merely shares a word: "wavefront lds ... hip" vs `wavefront_sensing`).
   `CNET_VSA_AMBIGUITY_K` overrides both; `CNET_VSA_FORCE_FLOAT=1` forces the float space.

4. Latency on the 101-capsule v3 registry (route_query = encode + scan + gates):

   | path | us/query | us/capsule |
   |---|---|---|
   | wide int8 (v3) | 9.2 | 0.091 |
   | float-512 (same capsules, forced) | 27.7 | 0.274 |
   | wide int8 encode alone | 4.4 | |
   | float-512 encode alone | 6.0 | |

   On the legacy 824-capsule float registry route_query costs 186 us.

## What is still weak

- One alien ("fold and shape a sourdough loaf for an open crumb") routes to
  `computer_vision_and_shape_analysis` in every wide configuration: generic words
  ("shape", "open", "fold") dominate a bag-of-words query. Document-frequency weighting
  across the registry is the next lever; it needs a shared table and a versioning story.
- Accept rates at k = 2 are low (24% of held-out sentences). That is the score rule's
  choice, not a floor; k = 1 doubles acceptance at three times the wrong rate.
- Near-duplicate capsules (frontier without semantic dedup) are still the dominant cause
  of wrong routes; the gate hides them, it does not fix them.
- Legacy bin/ (824 v1 capsules) still routes in float-512 until resealed from var/distill.

WITHHELD: generated-text quality; transformer baseline; open-ended language.

## Adversarial review of the v3 change set (77-agent workflow: 5 lenses, 3 refuters per finding)

24 raw findings, 7 confirmed, 17 refuted. All 7 fixed and every gate re-run green
(calibration 6/6, sweep, gencap, router, arena, text, stem, CLI 28/28, verify-fast).

| # | severity | fix |
|---|---|---|
| 1, 3 | medium | `cnet_vsa_gencap_seal` refuses an already certified (loaded) capsule and any v3 seal with no wide evidence: a load -> seal -> save upgrade would have published an all-zero centroid under an intact calibrated receipt. A build-time `wide_count` (never persisted) is kept next to the accumulator, and calibration refuses when the sentence list it measures is not the list that was ingested. |
| 2 | medium | `cnet_vsa_registry_add_capsule` now checks the file length against the version's persisted size and refuses a v2+ file whose receipt or topical block cannot be read or is invalid, so one truncated file can no longer demote a whole registry to the float space. |
| 4 | medium | `--reseal-heldout` empties its target directory first and the report records the routing space observed, with a warning if any query fell back to float. |
| 5 | medium | The wide-space routing test now runs over 12 v3 capsules so the margin gate is actually computed in that space (asserted), not only the radius. |
| 6 | low | The sweep gate requires >= 95% of corpora evaluated for bag and default, so an encoder regression cannot pass by removing rows. |
| 7 | low | The sweep gate bounds the wide encode at 4x the float encode + 1 us (measured 5.4 vs 7.4). |

Refuted findings worth keeping as notes: the Unity bridge reports distances whose
scale depends on the space (float vs wide) without saying which; the two lazily
initialised lookup tables are unsynchronised plain ints (single-threaded runtime);
route_query's stack frame is ~80 KB with the per-encoder query buffers.

## Corrections after the second review (result/cnet_vsa_v3_review_20260911.md)

The review found three defects and several overclaims. All three defects are fixed with
RED-first tests in `make cnet_vsa_calibration_bench` (now 6 gates, 3 new checks):

| finding | fix |
|---|---|
| P1: ingest after calibration kept a stale receipt | `cnet_vsa_gencap_ingest` refuses once calibration has been attempted (-1) |
| P1: registry admission never verified the digest | `cnet_vsa_registry_add_capsule` / `load_dir` perform the full verified load (bounds + digest) before an entry exists; a flipped centroid byte is refused (-5). Cost: one full file read per capsule; 824 capsules load in about 4 s warm. `route-batch` was added to the CLI so tools load a registry once per run |
| P2: calibration checked the sentence count, not identity | build-time order-free multiset hash of ingested sentences; a same-length foreign list is refused (-1) |

Overclaims withdrawn: "lossless" (see above), "same footprint" (payload, not file), "5% wrong
answers" (5% of queries are wrong routes; 20% of accepted; answers unmeasured). `verify-fast`
does not include the VSA targets (mk/verify_tiers.mk), so its PASS says nothing about them;
the VSA gates listed at the top are the evidence.

Held-out leak closed: `--reseal-heldout` now calibrates against a globally trimmed copy of
every corpus and probe file, so no evaluation query can be sampled as a negative for any
capsule. The measurement below reruns both the validation slice and a disjoint test slice
under that protocol.

## Leak-free rerun: validation slice and a disjoint test slice (score = correct - 2 * wrong)

Protocol: reseal from corpora minus the last 2 sentences; negatives drawn only from a
globally trimmed copy of every corpus and probe file; each eval loads the registry once
(`route-batch`). Slice A = first 120 capsules of bin/ (the slice k was chosen on, so
validation). Slice B = capsules 120-239 (never used to choose anything, so test).

| slice | space | k | sealed | queries | accept | correct | wrong | top-1 self | score | alien accepted |
|---|---|---|---|---|---|---|---|---|---|---|
| A (validation) | wide int8 | 2 | 99 | 198 | 0.258 | 0.207 | 0.051 | 0.475 | +21 | 1/11 |
| A (validation) | float-512 | 1 | 99 | 198 | 0.263 | 0.197 | 0.066 | 0.414 | +13 | 0/11 |
| A (validation) | float-512 | 0 | 99 | 198 | 0.374 | 0.242 | 0.131 | 0.414 | -4 | 0/11 |
| B (test) | wide int8 | 2 | 107 | 214 | 0.350 | 0.266 | 0.084 | 0.463 | +21 | 0/11 |
| B (test) | wide int8 | 1 | 107 | 214 | 0.533 | 0.341 | 0.192 | 0.463 | -9 | 1/11 |
| B (test) | wide int8 | 0 | 107 | 214 | 0.757 | 0.407 | 0.350 | 0.463 | -63 | 1/11 |
| B (test) | float-512 | 1 | 107 | 214 | 0.364 | 0.257 | 0.107 | 0.458 | +9 | 0/11 |
| B (test) | float-512 | 0 | 107 | 214 | 0.481 | 0.304 | 0.178 | 0.458 | -11 | 0/11 |

On the test slice the ordering chosen on the validation slice holds: wide k=2 (+21) beats
float at k=1 (+9) and float ungated (-11); the wide space's top-1 advantage is smaller
there (46.3% vs 45.8%) than on slice A (47.5% vs 41.4%), so most of the test-slice gain
comes from the gate, not the representation. Wrong routes at wide k=2 on the test slice:
18 of 214 queries (8.4%), 18 of 75 accepted (24%). Generated answers remain unmeasured.
