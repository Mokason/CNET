# TruthfulQA (v1)

- Source: https://github.com/sylinrl/TruthfulQA (raw
  `TruthfulQA.csv`, fetched 2026-07-16)
- License: Apache-2.0 (upstream repository license)
- sha256: b8d8ef1e12f98b4f2a9f47abc9765da0640b182b6c5d9b92f0c1a1f2f1e02e5c
- 789 questions + header row; adversarial + non-adversarial types.

Purpose: removes the "no real factuality dataset present" blocker named
in plans/phase1_counterfactual_routing.md and
docs/phase123_benchmark_closure.md. Presence of this file does NOT claim
any measured factuality number — a bounded, CPU-budgeted A/B (admitted
model with vs without counterfactual route evidence) must run and write
its own report before any Phase 1 external claim changes from
`withheld`.
