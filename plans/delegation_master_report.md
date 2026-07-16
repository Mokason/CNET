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

## 2026-07-16 Release Integrity Priorities

| Priority | Artifact | RED evidence | GREEN acceptance |
|---|---|---|---|
| P1 Oracle regression closure | `plans/release_integrity_implementation.md`, `tests/test_flagship_prefix_cache.c` | `/tmp/cnet-p1-red.log`: missing prefix-cache state helper | `FLAGSHIP_PREFIX_CACHE_PASS` (5), Qwen3.5 core 53/53, runner 36/36, current flagship source builds; all three targets now execute inside `priority_acceptance` |
| P2 Provenance closure | `src/cce/cce_campaign_provenance.c`, `qwythos_english_v1.cnb.sha256` | `/tmp/cnet-p2-red.log`: no fail-closed provenance API | SHA-256 known vector plus 10 mismatch/refusal checks; historical binary/model/window/golden/base closure passes; current binary refuses stale revision before model load; mandatory gate added |
| P3 Documentation truth | `docs/EXECUTION_TIERS.md`, `tests/test_execution_tiers_doc.sh` | `/tmp/cnet-p3-red.log`: canonical AICIMO falsely classified as quarantined | static source/build/doc consistency gate, executable alternate-path gate, and canonical AICIMO smoke all pass; doc gate is mandatory |
| P4 Release mechanics | `VERSION`, `.github/workflows/ci.yml`, `tests/test_release_package.sh`, `tests/test_ci_workflow.py` | `/tmp/cnet-p4-red-package.log`: no version source/install/package surface; `/tmp/cnet-p4-red-ci.log`: workflow missing; `/tmp/cnet-global-warning-red.log`: 116 strict diagnostics across 16 sources | `make PORTABLE=1 ci` passes locally: manual-dispatch-only YAML, portable flags, versioned staged install/uninstall, pkg-config consumer link/run, source tarball, and a complete shared-library `-Wall -Wextra -Wpedantic -Werror` gate (`NATIVE_WARNING_GATE_PASS`) |

P1 rejects H0: the chimera fixes are no longer standalone/manual-only evidence. P2 rejects H0: manifest replay now verifies source state, exact executable, model, ordered window, golden fixture and base, and only completed runs atomically publish output-base provenance. P3 rejects H0: the execution-tier contract now matches both the Makefile and executable symbol gate. P4 rejects H0: release version, CPU CI, portable configuration, staged package consumption, and warning-debt boundaries are executable rather than manual conventions. The final umbrella remains the single closure check.

## 2026-07-16 CNET 5.1.1 Integrity Closure

Source of truth: `plans/cnet_5_1_1_integrity_closure.md`
Final authority: `make --no-print-directory release_integrity`

| Slice | Artifact | Status | Required evidence |
|---|---|---|---|
| Exact model ingestion | `plans/integrity_q5_gguf.md` | FOCUSED GREEN | exact Q5_K decoding matches an independent reference across one uniform and three consecutive nonuniform blocks; malformed/truncated KV and tensor metadata fail closed; ASan/LSan proves current-record cleanup (`GGUF_INTEGRITY_GATE_PASS`) |
| Transactional residency | `plans/integrity_model_runtime.md` | FOCUSED GREEN | failed loads preserve selected bystanders exactly; successful replacements publish before reentrant victim callbacks; actual-size rejection is lock-safe; 146 assertions pass under `-Werror` and ASan/UBSan (`MODEL_RUNTIME_INTEGRITY_GATE_PASS`) |
| Specialist authority | `plans/integrity_specialist_authority.md` | FOCUSED GREEN | production constructors require certification; static bypass audit passes; unchecked constructors/appends and low-level certified append are absent from the ELF dynamic ABI (`ADMISSION_ABI_AUDIT_PASS`) |
| Crash-safe restart | `plans/integrity_persistence.md` | FOCUSED GREEN | temp+fsync+rename publication, verified previous-generation fallback, fail-closed registry globals/expansions, and automatic SoulHost replay pass focused gates |
| MCP survival | `plans/integrity_mcp_protocol.md` | REPAIR IN PROGRESS | first delegate handled common malformed frames; parent review rejected primitive-root/params exceptions, unenforced JSON-RPC version, notification replies, and non-concurrent lock-scope evidence; repair lane active |
| Release authority | `plans/integrity_release_gate.md` | FOCUSED GREEN / UMBRELLA PENDING | v5.1.1 archive is reproducible and builds/installs/runs a pkg-config consumer from extraction; static single-authority test passes; final marker remains gated on integrated slices |

H0 remains active until the clean integration tree executes the final authority and its exact evidence is inspected. No feature breadth or external push belongs to this cycle.
