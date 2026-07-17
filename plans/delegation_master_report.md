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
| MCP survival | `plans/integrity_mcp_protocol.md` | FOCUSED GREEN | malformed roots/params survive in one process; JSON-RPC 2.0 is enforced; every notification is silent; synchronized writes preserve protocol-only stdout; an injected blocked compression operation proves concurrent `tools/list` progress; integrated suite passes 12/12 (`MCP_PROTOCOL_SURVIVAL_GATE_PASS`) |
| Release authority | `plans/integrity_release_gate.md` | CLOSED | v5.1.1 archive is reproducible and builds/installs/runs a pkg-config consumer from extraction; clean tracked-tree preflight/closure pass; final authority emits `CNET_RELEASE_INTEGRITY_PASS` |

H0 is rejected: the clean integration tree executed the single release authority, every ordered marker was inspected, the archive self-built and ran its consumer, and no release process remained. No external push was performed during that closure run.

## 2026-07-16 Real-Model Recovery, Benchmark Closure, and Bounded Activation

Source of truth: `plans/post_release_real_model_continuation.md`

| Slice | Artifact | Status | Evidence |
|---|---|---|---|
| Candidate recovery | `reports/qwythos_candidate_recovery.json` | GREEN | Quarantined filename claimed Q8_0 while embedded headers contained 256 TQ1_0 tensors; an atomic CPU Q4_K_S replacement was admitted without weakening `max_quality_delta=0.0` |
| Real-model campaign | `reports/qwythos_real_model_acceptance.json` | GREEN, CANDIDATE ADMITTED | candidate quality `1.0`, reference `0.6666667`, delta `+0.3333333`; QGKP byte-identical; restart responses identical; candidate selected |
| Hermes launch | `tools/run_hermes_wrapper.py`, selected wrapper manifest | GREEN | isolated custom provider, loopback llama.cpp, `--n-gpu-layers 0`, strict final-line `Ready`, return code 0, and process cleanup |
| Phase 1–3 benchmarks | `reports/phase123_benchmark_closure.json` | PASS WITH EXTERNAL CLAIMS WITHHELD | three native contracts pass; nine sparse selector budgets are exact with full needle retention; real-model narrative delta `-0.036` passes `-0.05`; absent FACTOR/TruthfulQA and LongBench runtime paths are explicitly not claimed |
| Bounded registry consumer | `reports/cnet_bounded_activation.json` | GREEN | eight tests pass; row 6 completed `proposed -> in_progress -> verified`; unchanged candidate/replacement SHA linkage and zero-regression policy recorded; repeat invocation `idle`; zero actionable rows remain |
| Publication | private `origin/master` | PENDING FINAL AUTHORITY | run `make release_integrity`, attribution audit, private push, and local/remote SHA equality check |

H0 is rejected for recovery, benchmark integrity, and bounded activation: the replacement is admitted under unchanged gates, unavailable external metrics are withheld rather than simulated, and the only actionable row is durably verified with dedup/recovery evidence. Publication remains contingent on the single clean-tree release authority.

## 2026-07-17 Runtime Provenance and Gap-Service Unification

Source of truth: `plans/runtime_provenance_and_gap_service.md`
Final authority: `make --no-print-directory release_integrity`
Status at commit: **FOCUSED GREEN — READY FOR FINAL AUTHORITY**

| Slice | RED/adversarial evidence | Focused GREEN / permanent authority |
|---|---|---|
| Gap-service fail-safe | The enabled user unit had no daemon artifact and accumulated `203/EXEC` restarts. The first proposed `ConditionPathIsExecutable` name is invalid on systemd 255 and was rejected rather than normalized into the implementation. | Exactly one non-negated `ConditionFileIsExecutable` in `[Unit]` matches `ExecStart`; `systemd-analyze condition` accepts `/bin/sh` and rejects a known-missing path; `gap_lane_service_prepare` builds/tests only and never starts/enables. `gap_lane_service_config` is mandatory in `unified_native`. |
| Complete Oracle provenance | Native RED linked undefined `soul_oracle_runtime_libs_digest`; managed RED showed missing `ArtifactSha256`/`RuntimeLibsDigest`. The existing fixed-signature `soul_oracle_identity` could not be extended without ABI breakage. | Independent native accessors expose the exact persisted 32-byte artifact hash and v5 runtime digest; invalid arguments fail, zero stays an unattested label. The .NET record appends optional zero defaults and retains the exact old constructor overload for source/binary compatibility; real `SoulHost` data replaces them. Focused API test 1/1 and MCP real-fixture tests 3/3 pass exact `a0…bf` / `0x00000000c1b5c0de`. |
| False-green resistance | A focused MCP command using `--no-restore` on a fresh worktree returned 0 with `IsTestProject` empty and ran zero tests. That success was rejected. | The test project is restored before focused execution and output must contain a nonzero all-passing count. Permanent `mcp_protocol_survival` already restores/runs the full project and rejects absent/zero pass markers; `unified` separately asserts exact stdio MCP fields and both ELF symbols. |
| Operational boundary | Any service start could load the local Gemma-backed teacher and violate the offline requirement. | No start/enable action is present in Make targets or tests; `cnet-gap-lane.service` remains disabled/inactive and Gemma remains offline. Build-only recovery is explicit operator preparation, not activation. |

H0 for the focused candidate is rejected: the service has a fail-safe executable condition and build-only recovery path, while exact full-artifact and linked-runtime provenance reaches native, managed, and MCP observers without changing the old identity ABI. Release closure is granted only by the single clean-tree authority above; no external push is part of this slice.
