# Registry rollout: bin/ resealed under the v3 block and the shipped lexicon (2026-09-12)

Until today every capsule in bin/ was a v1 file (bag encoder, float-512 gate, fixed 0.90 radius) and nothing that
loaded a registry knew about a lexicon. The measured lexicon existed only inside the benchmark. This record is the
rollout: the exact measured table now ships with the registry, every consumer finds it automatically, and all
capsules are resealed under it with per-capsule calibration.

## What ships

- `bin/registry.lex`: the frozen v3.2 trained table, digest 0xaf5569e5bde45edf, the same digest `frozen_model.json`
  pins and `make vsa_routing_arena` rebuilds (41.5 MB; bin/ is untracked, the gate reproduces it from the fixture).
- Auto-activation (`cnet_vsa_lexicon_activate_default`): `cnet_vsa_registry_load_dir` activates `<dir>/registry.lex`
  (else `CNET_VSA_LEXICON`) unless a lexicon is already active, so the CLI, the Unity bridge and every tool that
  routes through `route`/`route-batch` get it without configuration. `gencap-gen` on a LEX capsule does the same from
  the capsule's directory and refuses, rather than gating in float space, when the tag does not match.
- Both crawlers seal new capsules with `--encoder lex` under the registry's table when `registry.lex` exists
  (`_encoder_for`, `_lexicon_env`); the registry refuses any capsule sealed under another table (-8).
- Lexicon epochs: retraining the table changes its digest and every LEX capsule then refuses under it. A new epoch
  means resealing the whole registry; keep epochs coarse and reseal with this procedure.

## Reseal procedure (var/claude_scratch/reseal_one.sh, 8 workers, 2.3 s per capsule, 4 minutes for 824)

`gencap-create <name> <domain> var/distill/<name>_corpus.txt ... --encoder lex --probes <5 training questions>
--negatives var/distill --target-in 0.80 --target-neg 0.90` with `CNET_VSA_LEXICON=bin/registry.lex`. Calibration
probes are the teacher TRAINING questions only; the frozen evaluation questions were never probes and are the check
below. Negatives: 512 sentences sampled from the other 1,067 corpora. Legacy files were backed up to
var/bin_legacy_v1 before the swap.

Outcome: 812 sealed, 12 refused as NOT_SEPARABLE at 0.80/0.90 in the wide space and removed from bin/
(computational_thermal_analysis, controlled_cooling_and_thermal_cycling, crystallographic_structure_determination,
experimental_mechanics, force_distribution_monitoring, hide_trimming_and_leveling, hydraulic_modeling,
maintenance_and_reliability_engineering, quantitative_metallography, surface_reconstruction_techniques,
thermal_barrier_coatings, wood_technology): each sits on a sibling the registry cannot tell it from at the policy; the
old v1 files had been sealed at a fixed radius with no such test. Calibrated wide-space radius: median 0.869, range
0.788 to 0.925.

## Verification (full production router: calibrated radius, margin gate, ambiguity gate k=2, wide space)

Admission is fail-closed: 812 capsules load with the shipped table, 0 with a different lexicon, 0 with none.

Routing the 3,994 frozen teacher questions of the resealed capsules (never calibration probes) plus the 12 aliens:

| registry | accept | correct | wrong accept | abstain | top-1 self | score (correct - 2 wrong) | aliens accepted |
|---|---|---|---|---|---|---|---|
| legacy bin/ (v1 bag, float, fixed 0.90) | 58.9% | 23.8% | **35.1%** | 41.1% | 29.7% | **-1851** | 1/12 |
| resealed (v3 block + lexicon, calibrated) | 34.6% | 32.9% | **1.7%** | 65.4% | 67.7% | **+1179** | 1/12 |

The legacy registry was accepting wrong capsules on a third of in-domain questions; the resealed one accepts fewer
questions, is right on 95% of those it accepts, and routes the right capsule top-1 on two thirds of all questions.
The remaining alien accepted is the same quantum-qubit query as before. `gencap-gen` on the 100 prompts measured in
the generation record: 2 refused (was 29), 96 generated.

## Term-dependence gate (added the same afternoon, after the router bench caught a false accept)

The router bench's alien "haute couture silk velvet dress tailoring and draping" was ACCEPTED by the resealed registry
as stone_cutting_and_dressing at distance 0.71 (limit 0.85). Word ablation: "dress" alone routes there at 0.45; the
stemmer maps dress and dressing to one key, the trained vector for that key points at the capsule, and one word inside
the radius carried the whole prompt. The legacy float registry had refused it. So wide-space prompt routing now has a
fourth gate (`term_gate_check`, `CNET_VSA_TERM_GATE=0` disables): an accept must survive the removal of any one
content word (each leave-one-word-out query re-encoded and checked against the winner's radius), and a prompt with
fewer than two content words is never accepted. It runs only on accepts: an abstention costs nothing, an accept costs
one encode per content word (about 5 us each).

| resealed registry, frozen questions | accept | correct | wrong | abstain | score | aliens accepted (fixture) | bench alien |
|---|---|---|---|---|---|---|---|
| term gate off | 34.6% | 32.9% | 1.7% | 65.4% | +1179 | 1/12 | accepted |
| **term gate on (default)** | 30.6% | 29.4% | 1.2% | 69.4% | +1077 | **0/12** | refused |

The price is 3.5 points of correct accepts (routes that hinged on one key term), for 0.5 fewer wrong accepts, no alien
accepted, and the single-word hole closed. Under the arena's score rule alone the gate loses 102 points on this
question set; it is on by default because a route that a single colliding word can flip is not a certified route, and
the knob records the trade-off. The router bench passes with it (`make cnet_vsa_router_bench`); its crypto keyword
list now abstains between cryptography_foundations and cryptographic_encryption_protocols by the ambiguity gate, which
the bench accepts as a sibling abstention; the fixed 0.900 OOD distance assertion became "refused by some gate", since
calibrated wide-space radii replace the fixed float radius. CLI, lexicon, calibration and capsule gates pass.

## Not done here

- The Isekai/RPG corpora (var/distill_isekai) have no capsules in bin/ and no lexicon epoch of their own.
- The abstain rate on single questions is high by design (fail-closed on a one-line query); the router sees whole
  prompts in use. Whether 65% abstain is the right operating point is a policy question for the ambiguity gate
  (CNET_VSA_AMBIGUITY_K) and the calibration targets, not the encoder, and was not tuned here.
