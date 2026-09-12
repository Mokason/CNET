# Bounded structural HDC experiment — 2026-09-11

User objective: test structural HDC and factorization as a path beyond topical centroid retrieval. The user has no transformer endpoint for this experiment. Direct transformer superiority is WITHHELD; no transformer calls are part of the scored benchmark.

Protocol fixed before evaluating results:

- Relational suite: independently generated directed, signed facts and queries with 1, 2, or 4 relation steps. Include unique answers, reversed roles, distractors, missing paths, multiple answers, and contradictions. Use fixed development/test seeds and shuffled fact order; gold is exact graph traversal, independent of HDC.
- Baselines: current int8 topical fact retrieval using production routing gates; structural bipolar HDC forward unbinding; experimental coupled HDC constraint updates; exact symbolic traversal. Report proposed answers separately from exact verification, which is charged to latency and storage. The latter two HDC lanes are research prototypes, not certified production capabilities.
- Factorization suite: existing production three-factor resonator with native and paired bipolar keys at its supported 512 dimensions, and a separately labeled bipolar resonator prototype at 512/2048 dimensions. Sweep codebook count, vector seed, noise, and iteration cap. Measure exact factor IDs, wrong acceptance, timeout, reconstruction, elapsed time, and storage. Do not equate convergence with correctness. No threshold changes based on test results.
- Report input encoding/build time separately from warm query time. Measure memory explicitly. Constant iteration count does not imply constant total work.
- Natural-language role test: compare reversed subject/object encodings, and a controlled-grammar parser separately from typed solver input. Unrestricted language, open-ended synthesis, and broad transformer replacement remain WITHHELD.

Tasks:
1. Build seeded fixtures and independent reference checks, including deliberately failing role-collision evidence.
2. Implement isolated experiment engines and sanity tests. Preserve production code and all certification floors.
3. Run disjoint held-out suites, resource/latency sweeps, and artifact integrity checks.
4. Save raw results and a report including limitations and failed hypotheses.

Completed 2026-09-11. Harness, native engines, finite grammar checks, sanitizer
smoke test, frozen fixtures, raw results, and report are recorded under
`experiments/hdc_structure/` and `result/cnet_hdc_structure_20260911.*`.
360 relational fixtures and 1,728 factorization conditions were evaluated.
The exact solver wins the measured relational contract. The bipolar factor
prototype improves small-codebook recovery but does not sustain it at 64 entries
per factor. No production rollout, certification change, or transformer win is
claimed. Full limitations and resource accounting are in the report.
