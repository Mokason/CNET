# CNET Release Integrity Implementation Plan

> **For Hermes:** Execute in strict priority order with RED→GREEN tracer bullets and one final fail-fast closure check.

**Goal:** Move CNET’s latest Qwen3.5 campaign path from manually credible to mandatory-gated, provenance-enforced, documented, portable, and CI-verifiable.

**Architecture:** Keep one Makefile authority. Add model-free regressions around the existing CCE/flagship boundaries, enforce manifest identity before loading the model, then layer documentation and release mechanics without changing planner semantics.

**Tech stack:** C11, POSIX shell, GNU Make, GitHub Actions YAML, SHA-256 tooling available on Linux.

## Priority 1 — Oracle regression closure

**Place:** `priority_acceptance`, the release gate.
**Dilemma:** Current Qwen3.5 and flagship prefix fixes pass only as standalone evidence.
**Social consequence:** A future green umbrella could silently certify a reintroduced chimera oracle.
**Systematic component:** mandatory hermetic tests; **irreducible noise:** none—the fixtures are model-free.

1. Add a failing hermetic prefix-cache behavior test.
2. Implement the minimal shared cache state helper and wire `flagship_run.c` through it.
3. Add the focused target and Qwen3.5 e2e target to `priority_acceptance`.
4. Run focused GREEN gates.

## Priority 2 — Provenance closure

**Place:** manifest replay before any model-backed work.
**Dilemma:** recipe fields are recorded but decorative.
**Social consequence:** users cannot distinguish the same campaign from a same-named but different binary/model/window.
**Systematic component:** SHA-256 identity and mismatch refusal; **irreducible noise:** compiler/build-id variation, handled by executable content digest rather than prose.

1. Add failing tests for missing/mismatched executable, model, window and base digests.
2. Add SHA-256 manifest fields and fail-closed replay validation.
3. Track a digest sidecar for the ignored clean `.cnb` and link it from the manifest.
4. Verify valid replay metadata against local artifacts without running the model.

## Priority 3 — Documentation truth

**Place:** `docs/EXECUTION_TIERS.md`.
**Dilemma:** it says AICIMO is excluded while the Makefile and gate require it in core.
**Social consequence:** operators choose the wrong execution boundary.
**Systematic component:** source/gate cross-check; **irreducible noise:** none.

1. Correct the AICIMO tier description.
2. Run the existing alternate-path gate and a static doc/build consistency check.

## Priority 4 — Release mechanics and warning debt

**Place:** portable build/install/package/CI boundary.
**Dilemma:** the repository works locally but has no repeatable consumer path.
**Social consequence:** external users cannot build or trust releases.
**Systematic component:** portable flags, staged install, version metadata, manual-dispatch CI and a full-native `-Werror` gate; **irreducible noise:** platform/toolchain differences exposed by explicitly authorized CI runs.

1. Add portable/release flag modes and version metadata.
2. Add staged `install`, `uninstall`, `dist`, and pkg-config generation.
3. Add CPU/model-free, manual-dispatch-only GitHub Actions CI.
4. Triage and eliminate warnings across the complete shared-library source set; enforce the full-native warning gate.

## Closure

Run one fail-fast command covering focused tests, provenance tests, packaging smoke, YAML validation, diff hygiene, and `priority_acceptance`; inspect full logs for sanitizer/fatal signals. Do not push externally without confirmation.
