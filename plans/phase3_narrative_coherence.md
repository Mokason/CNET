# Phase 3: Narrative Coherence Contracts — Execution Log

**Status**: Implemented; bounded real-model preservation measured
**Started**: 2026-07-06
**Measured**: 2026-07-16

## Contract

### RPG Storytelling Framing

- **Place**: Heavily post-trained creative models under CNET compression.
- **Dilemma**: Structural compression can flatten narrative texture, moral ambiguity, and folklore depth.
- **Social consequence**: A compressed model can remain syntactically valid while losing its creative value.

### Random Variables

- **Systematic component**: voice consistency, moral language, delayed consequence, folklore texture, and narrative complexity.
- **Irreducible component**: style preference and some post-training flattening; the lexical rubric is not a human-preference oracle.

## Implemented

- `include/contract/narrative_coherence.h`
- `src/contract/narrative_coherence.c`
- creative-task routing preference in `src/cce/cce_router.c`
- native regression target `make narrative_coherence_test`
- C# scoring and testimony metadata integration
- bounded real-model authority in `tools/run_phase123_benchmarks.py`

## Gate Repair

Benchmark closure found that the committed native test expected a different API than the committed header/source and the Makefile omitted `-Iinclude`; the advertised target did not build. The test now exercises the actual public API (`cnet_narrative_evaluate` and `cnet_narrative_passes`) and passes.

## Real-Model Measurement

Two fixed creative prompts were generated deterministically on CPU by the admitted reference and recovered Q4_K_S candidate and scored by the native C rubric:

- reference mean: `0.8005000055`;
- candidate mean: `0.7645000219`;
- delta: `-0.0359999835`;
- preservation floor: `-0.05`;
- measured verdict: pass.

Evidence: `reports/phase123_benchmark_closure.json` and `docs/phase123_benchmark_closure.md`.

## Claim Boundary

The result measures bounded lexical-rubric preservation. Human judgment and independent LLM-as-judge evaluation were not run and remain labeled `not_run`; no human-preference claim is made.
