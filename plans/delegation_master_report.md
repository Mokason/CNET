# CNET Contract Optimization — Master Report

Goal: improve contracts in quality, speed, and correctness without changing unrelated planner/runtime behavior.

| Slice | Artifact | RED evidence | GREEN acceptance |
|---|---|---|---|
| Correctness | `plans/contract_correctness.md` | oversized frozen counts, overlong names, ambiguous borrowed tables, non-finite evidence, soft EVIDENCE save/load failure, and certification hashing a forged shape before signature refusal | descriptor/table shape validated before atomic publish; malformed inputs refused; finite RAW/EVIDENCE text round-trips bit-identically; certification rejects mismatched shapes before digesting borrowed tables |
| Quality | `plans/contract_quality_speed.md` | a 0.99 certified candidate lost to a 0.76 incumbent because canonicalized MSE was identically zero | higher `CertifyReport.min_margin` wins |
| Speed | `plans/contract_quality_speed.md` | candidate comparison performed 4 forwards (certify + redundant score replay) | exactly 2 forwards, one certification replay per model: 50% fewer |
| Closure | `make contract_optimized` | security target previously swallowed non-zero test exits | focused tracer + security + sealed-unit + full historical native suite; strict positive markers |

## Hypotheses

- H0: validation and margin reuse do not improve contract safety, promotion quality, or forward work.
- H1: malformed contracts are atomically refused, finite normalized EVIDENCE text persists without precision/seal loss, the stronger certified implementation is promoted, and comparison halves forward calls.

Focused RED/green logs reject H0. Final umbrella acceptance is the single closure check.

## Evidence

- `logs/contract_optimized_red.log`: 7 expected failures; 4 forwards for one comparison.
- `logs/contract_optimized_red2.log`: ambiguous borrowed tables accepted before the fix.
- `logs/contract_optimized_green2.log`: original optimized assertions pass; `iterations=2000 forwards=4000`.
- `logs/contract_evidence_red.log`: EVIDENCE load/seal and non-finite refusal fail before the async-audit fix.
- `logs/contract_evidence_green.log`: lossless sealed EVIDENCE round-trip and non-finite refusal pass.
- `logs/contract_evidence_fullasan_run.log`: ASan found forged-shape digest OOB before signature rejection; gate ordering fixed.
- `logs/contract_evidence_legacy_fixed2.log`: historical single executable passes after preserving finite RAW contract semantics.
- `logs/contract_evidence_final_gate.log`: `ALL TESTS PASSED (single exe)` and `CONTRACT_OPTIMIZATION_GATE_PASS`.
