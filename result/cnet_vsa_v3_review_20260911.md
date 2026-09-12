# VSA v3 review — 2026-09-11

Verdict: request changes before treating v3 as fully validated or migrating the legacy registry. The reported int8 routing tradeoff is promising, but three executable probes expose gaps that the passing calibration and router gates do not cover.

Scope: reviewed the v3 result report, plan, calibration/seal/load/registry implementation, wide quantization implementation, relevant tests, and held-out evaluator. This was a targeted review of the uncommitted working tree, not an exhaustive review of every changed file. No production source files or capsule registries were changed.

## Confirmed findings

1. **P1 — Additional ingest preserves a stale calibration receipt.** `cnet_vsa_gencap_ingest` refuses certified capsules, but a calibrated, unsealed capsule remains ingestible. It changes both centroids without invalidating either receipt. `seal_common` then quantizes the new centroid and seals it with the old measured acceptance/rejection rates and radius. A valid digest consequently authenticates a receipt that does not describe the sealed centroid. Reproduction: successful calibration, one additional sentence from another domain, then seal. Observed `ingest=12 seal=0 calibrated=1 receipt_unchanged=1 wide_calibrated=1`. Invalidate calibration on successful ingest and require recalibration before seal, or refuse ingest after calibration. References: `src/cnet_vsa_gen_capsule.c:117`, `src/cnet_vsa_gen_capsule.c:437`.

2. **P1 — Registry admission does not verify the capsule digest.** `cnet_vsa_registry_add_capsule` checks the header, exact file size, encoder, and topical layout, then copies routing metadata without validating its digest. Flipping one centroid byte produces `full_load=-5 registry_add=0 registry_count=1`. Thus a corrupt entry can influence the winner, runner-up, and registry-wide null statistics. Dispatch verifies the winning file later and refuses a corrupt winner; that limits the failure, but does not protect routing from corrupt losing entries or make registry admission fail closed. Validate integrity before publishing an entry to the registry, and test centroid/receipt/radius corruption through registry admission, not only full loading. Reference: `src/cnet_vsa_gen_capsule.c:742`.

3. **P2 — Calibration checks sentence count, not sentence identity.** The claimed protection against measuring a different list compares `n_wide_seen` with `wide_count`. A different list of the same length passes. A fresh same-domain corpus, never ingested into the capsule, returned `calibrate=0 calibrated=1` when supplied as its ingested-sentence list. The subtraction used for leave-one-out then removes vectors that were never accumulated. Track and verify the ingested evidence identity, including multiplicity, or make calibration consume retained build evidence directly. Reference: `src/cnet_vsa_gen_capsule.c:386`.

The reproduction source is `/tmp/cnet-v3-review.c`; its binary is `/tmp/cnet-v3-review`. It reuses the existing synthetic corpus generator and leaves no capsule file behind. These are review probes, not committed regression tests.

## What the measurements establish

The final report's 202-query table implies approximately 39 correct routes, 10 wrong routes, and 153 abstentions for wide k=2. Consequently:

- Coverage: 49/202 = 24.3%.
- Correct routes among all queries: 39/202 = 19.3%.
- Wrong routes among all queries: 10/202 = 5.0%.
- Wrong routes among accepted queries: 10/49 = 20.4%.
- Score: 39 - 2*10 = +19.

These are capsule-routing labels. Generated-answer correctness remains WITHHELD, as the report itself states. Describing this as “5% wrong answers” obscures both the denominator and the fact that generated answers were not evaluated.

The reported top-1 improvement, 41.6% to 48.0%, supports better candidate ranking on the 101-capsule evaluation. The k=2 operating point trades substantial coverage for fewer wrong routes. The closest tabulated gated float comparison is k=1, score +10, versus wide k=2, +19; the headline -6 to +19 comparison changes both representation and gate policy. A paired evaluation on a fresh frozen test set is needed to separate those effects. If these same held-out queries were used to choose k, they serve as validation data for that choice, not an untouched final test.

The held-out evaluator also passes the original, untrimmed probes directory as the negative source when resealing. `load_negatives` reads other capsules' corpus and probe files from that directory. Evaluation queries are therefore eligible for inclusion in competing capsules' calibration negatives. The exact sampled overlap was not measured in this review. Make the split global across positive and negative calibration inputs before relying on strict held-out claims. References: `tools/cnet_vsa_route_eval.py:153`, `tools/cnet_vsa_cli.c:882`.

“Int8 is lossless” is stronger than the evidence. The implementation rescales and rounds centroid coordinates; that is lossy quantization. Matching aggregate top-1/top-3 rates at the displayed precision supports “no observed aggregate routing loss in this experiment,” not exact vector preservation or identical per-query decisions.

The 2048-byte comparison is for centroid payloads. V3 appends a 2112-byte topical block while retaining the old float centroid, and v2 added a 64-byte receipt. V3 therefore adds 2176 persisted bytes relative to v1. The passing test reports a complete v3 capsule size of 5,600,640 bytes. It is not a same-size whole-file replacement.

The measured 9.2 versus 27.7 microseconds is about a 3x improvement for encode + scan + gates over the 101-capsule registry. It does not measure file admission, winning-capsule loading/digest verification, or text generation.

## Deployment and verification

A fresh header inventory found `bin/` contains exactly 824 v1 `.gencap` files. `cnet_vsa_registry_binary_space` requires every certified entry to have a topical block. Adding v3 capsules to a mixed registry keeps that registry in float space, with the float ambiguity default. New crawler output being v3 therefore does not by itself prove production wide routing. This review did not independently verify the live crawler process.

Freshly rerun:

- `make cnet_vsa_calibration_bench`: PASS, 6 gates.
- `make cnet_vsa_router_bench`: PASS, 4 gates, using the 824-capsule registry.
- Three targeted reproduction probes: all exposed the behavior described above.

Gate output is `/tmp/cnet-v3-review-gates.log`. The complete corpus sweep, 101-capsule A/B, and `verify-fast` were not rerun. `mk/verify_tiers.mk:101` shows that `verify-fast` does not include the VSA calibration/router/sweep targets, so its passing status would not address these findings.

Recommended order: fix the integrity and calibration lifecycle gaps with RED-first regression tests; freeze globally disjoint calibration/validation/test evidence and rerun paired routing comparisons; then rebuild legacy capsules into a separate registry and validate the full-size population before switching. Treat document-frequency weighting as a subsequent encoder/calibration change, with its table identity and compatibility rules explicit. It changes the geometry against which existing radii were certified.
