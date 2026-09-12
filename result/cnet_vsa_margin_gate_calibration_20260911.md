# VSA router margin gate and per-capsule radius calibration (2026-09-11)

Gate: `make cnet_vsa_calibration_bench` (marker CNET_VSA_CALIBRATION_BENCH_PASS, 5/5). Existing gates re-run green: cnet_vsa_router_bench (824 v1 capsules), cnet_vsa_gencap_bench, cnet_vsa_cli_bench (28/28).

## What changed

- Registry routing now requires two gates: the winner's per-capsule radius AND a margin over the registry null distribution, z >= sqrt(2 ln N) + 1.0 where N is the number of other certified capsules. Explained by `cnet_vsa_registry_route_ex` and `cnet_vsa_cli route`.
- Capsule format version 2 appends a calibration receipt (targets, measured rates, radius window) covered by the digest. Version 1 files load unchanged with the legacy 0.90 radius and calibrated=0.
- `gencap-create --negatives <dir> [--probes f] [--target-in] [--target-neg]` calibrates the radius from leave-one-out corpus sentences plus held-out probes against sentences sampled from other corpora, and refuses to seal when no radius meets both targets (NOT_SEPARABLE) or evidence is thin.
- Both crawlers write held-out probes and call the calibrated create; default targets 0.80 accept / 0.90 reject (env CNET_CALIB_TARGET_IN / CNET_CALIB_TARGET_NEG).

## Route evaluation on bin/ (824 version-1 capsules, all radius 0.90)

Queries: capsule name as an in-domain query (no probes exist for legacy capsules), 12 alien questions. Tool: `tools/cnet_vsa_route_eval.py`.

| set | queries | legacy accept | new accept | legacy correct | new correct | top-1 is self |
|---|---|---|---|---|---|---|
| in-domain | 824 | 0.944 | 0.761 | 0.409 | 0.386 | 0.411 |
| alien | 12 | 0.667 | 0.083 | - | - | - |

z-score: in-domain median 6.13, alien max 4.92, required 4.66


Legacy = radius only; new = radius AND margin. The 19 in-domain losses sit at z between 2.97 and 4.66 against a required 4.66; the single alien acceptance (coral reefs -> thermal_shock_resistance_engineering) scored z=4.92.

## Alien queries
| query | closest | dist | limit | z | legacy | new |
|---|---|---|---|---|---|---|
| How do I fold and shape a sourdough loaf for an open crumb? | computer_vision_and_shape_analysis | 0.860 | 0.900 | 3.96 | ACCEPT | refuse |
| How do quantum qubits maintain coherent superposition on a B | quantum_computing | 0.818 | 0.900 | 3.96 | ACCEPT | refuse |
| What stitch pattern gives a Fair Isle sweater its stranded c | tribunal_procedure | 0.910 | 0.900 | 3.16 | refuse | refuse |
| How are deep-sea coral reef ecosystems affected by ocean aci | thermal_shock_resistance_engineering | 0.764 | 0.900 | 4.92 | ACCEPT | ACCEPT |
| What are the astrological interpretations of planetary trans | damage_mechanics | 0.801 | 0.900 | 3.01 | ACCEPT | refuse |
| How does a haute couture atelier drape silk velvet for an ev | magnetorheological_actuator_design | 0.898 | 0.900 | 3.07 | ACCEPT | refuse |
| How do paleontologists date dinosaur fossils in sedimentary  | pm_director_pacing | 0.932 | 0.900 | 3.59 | refuse | refuse |
| What gives a wheated bourbon its sweetness during barrel agi | pm_perception_sensors | 0.873 | 0.900 | 3.29 | ACCEPT | refuse |
| Which chess openings lead to a closed center and a kingside  | ceramic_fiber_manufacturing | 0.920 | 0.900 | 3.91 | refuse | refuse |
| How do you tune a violin using harmonics and a fifth interva | pm_craft_production | 0.870 | 0.900 | 3.25 | ACCEPT | refuse |
| What is the best way to train a puppy to stop pulling on the | feudal_jurisprudence_and_estate_law | 0.947 | 0.900 | 3.35 | refuse | refuse |
| How does a sommelier detect cork taint in an aged Burgundy? | detwinning_and_variant_reorientation | 0.853 | 0.900 | 2.96 | ACCEPT | refuse |

## In-domain queries lost to the margin gate (19)

## Calibration sweep over all 1068 corpora under var/distill

Tool: `tools/cnet_vsa_calib_sweep.py` over `gencap-create --dry-run --calib-report` output. In-domain = leave-one-out corpus sentences (no probes exist for legacy corpora); negatives = 512 sentences sampled from other corpora.

in-domain leave-one-out distance: mean of means 0.792; negatives: mean of means 0.968

| accept >= | reject >= | separable | chosen radius median | radius p10 | radius p90 |
|---|---|---|---|---|---|
| 0.90 | 0.95 | 82/1068 (7.7%) | 0.885 | 0.858 | 0.902 |
| 0.90 | 0.90 | 195/1068 (18.3%) | 0.898 | 0.873 | 0.916 |
| 0.85 | 0.95 | 193/1068 (18.1%) | 0.877 | 0.846 | 0.897 |
| 0.85 | 0.90 | 366/1068 (34.3%) | 0.891 | 0.866 | 0.909 |
| 0.80 | 0.95 | 314/1068 (29.4%) | 0.871 | 0.839 | 0.891 |
| 0.80 | 0.90 | 567/1068 (53.1%) | 0.885 | 0.860 | 0.904 |
| 0.75 | 0.90 | 748/1068 (70.0%) | 0.880 | 0.852 | 0.900 |
| 0.70 | 0.90 | 880/1068 (82.4%) | 0.875 | 0.844 | 0.896 |

## Achievable in-domain accept at reject >= 0.95, per corpus
median achievable accept 0.72; p10 0.53; p90 0.89

Worst 15 corpora (least separable):
| corpus | achievable accept |
|---|---|
| pm_combat_guilds | 0.18 |
| materials_science | 0.22 |
| mechanism_synthesis | 0.24 |
| environmental_monitoring_systems | 0.28 |
| robotics | 0.30 |
| historical_architecture_and_spatial_organization | 0.32 |
| manipulator_design | 0.33 |
| pneumatology | 0.33 |
| welding_and_coating_process_engineering | 0.33 |

## Reading

- The bag-of-words topical encoder separates domain means well (in-domain mean distance 0.79 vs negatives 0.97) but its tails overlap: a single teacher sentence often shares few content words with the rest of its corpus, and 5 percent of negatives come from adjacent corpora (near-duplicate topics exist in the registry).
- A 0.90/0.95 policy is therefore the too-tight failure mode: 7.7 percent of corpora would seal. 0.80/0.90 seals 53 percent; 0.70/0.90 seals 82 percent. The receipt inside each capsule records the measured rates, so the policy is explicit rather than hidden in a constant.
- The margin gate is independent of the radius policy and already removes most random-maximum acceptances at registry scale.
- Next lever is the encoder, not the thresholds: document-frequency weighting across the registry, positional bigram binding, and character-trigram word vectors would move the whole sweep table. Re-run the sweep after any encoder change before touching targets.

WITHHELD: text quality of generated output is still unmeasured; calibration certifies routing scope only.
