# CNET July 18–19 Commit Audit

Scope: every commit from `4216468` through `93fe9aa` (29 commits, 170 files, 25,820 insertions / 2,597 deletions), plus the current uncommitted continuation. The date window starts at 2026-07-18 00:00 +03:00 because the final commit crossed midnight at 00:11 on July 19.

## Commit-by-commit review

| Commit | Intent | Review result / required closure |
|---|---|---|
| `4216468` | Collapse serve/env lookup ladders | Performance-only rewrite; no focused test delta. Must remain behind existing routing/serving gates. |
| `2a34c3d` | Port-key route planning and last-hit lookup | Key equality retains full `strcmp` collision check. `src/router/route.c` was committed wholly CRLF and needs normalization. No focused test delta. |
| `9cb7ba7` | Open-addressed registry name index | Focused collision/growth test added. Keep fallback semantics and hash invalidation under registry gates. |
| `98f6e65` | Closed-set JSON tool-call spine | Native and managed tests added. Initial v0 has only canonical examples, so a separate held-out format/feature benchmark is required. |
| `2c5adfa` | Seal JSON tool-call unit into live CNB | Idempotent seal and automation coverage added. Runtime children still snapshot the pre-growth CNB. |
| `a6289e5` | Rebuild learner before restart | Correct dependency ordering, but no direct regression test for the restart path. |
| `afea399` | Unify JTC agent/MCP/alphabet/metrics/gaps | Managed coverage exists; the MCP explicitly documents that children must recycle. That limitation must be removed rather than documented. |
| `0860159` | JTC-capable hermetic gap-lane teacher | Native teacher/seal tests added. |
| `dee00a3` | Personal-AI ops scheduler | **Regression:** no direct ops-tick gate; `set -euo pipefail` exits at the `promoted=`/`count=` extraction pipeline when `grep` has no match, causing the observed repeating service failure and stale report. |
| `6e4f298` | Lookup tools, learn loop, algebra solve | Very broad (41 files). Core math/learn tests exist, but it also leaves the ops curriculum pointed at a local Ollama model, contrary to the deployed cloud-only/offline policy. |
| `ca6868f` | Pattern runtime | Focused runtime gate added. Ops integration inherits the broken extraction pipeline above. |
| `6b9b5e1` | SSMax/DSA/MLA and DeepSeek forest map | Five focused tests added; synthetic tensor-map claims are clearly separable from real-model claims. |
| `b5c268a` | Bind DeepSeek map into forest | Focused map/forest test added. |
| `c0a1efc` | DeepSeek stack host runtime | Focused runtime test added. |
| `1c8d407` | AMD OpenCL/HIP path | Four focused tests/benches added. Product-host `auto` currently attempts HIP and OpenCL together; benchmark code must default CPU on this reserved-GPU host and dual-backend selection must be explicit. |
| `63352cf` | GPU attention/residency | No test-file delta; relies on previously added GPU gates. |
| `34ad63f` | Device residual/KV stream | No test-file delta; relies on GPU gates. |
| `5fc23e7` | Device RoPE and slot Q/K/V | No test-file delta; relies on GPU gates. |
| `9b1a8b6` | YaRN/QK norm and device FFN | No test-file delta; relies on GPU gates. |
| `0a01b50` | Forest sparse activation/int8 KV | Focused runtime test updated. |
| `67f317c` | Forest MTP and EP tags | Focused speculative-decode test added. |
| `52117fe` | Async paged KV | Focused paging test added. |
| `7db4775` | GGUF KV pager wiring | Focused pager integration test updated. |
| `ad4a301` | Parallel MTP verify | No test-file delta. Published speedup is simulation-tax-only and must remain labeled synthetic. |
| `28e04f5` | Resource governor/orchestrator | Focused governor test added. Documentation contains NVIDIA-specific power-cap wording on an AMD deployment and patch whitespace debt. |
| `8ef4e06` | MTK phase 1 | Focused CMSK/MTK test added. |
| `3fe8d69` | MTK phases 2–5 | Focused MTK integration test expanded. |
| `6d5019d` | MTK product host | Synthetic eval target added, but no dedicated test file. Host fallback mutates process-wide sparse env and `auto` can attach both GPU backends. |
| `93fe9aa` | Quality eval/campaign bench | **Truthfulness regressions:** no test; the only real prompt is `2+2`; PASS checks merely that generated text contains a letter and never scores `4`; quality-eval build failure can be ignored because the campaign script lacks `errexit`/a wrapped build step; missing real model is a soft skip; GPU is not forced off for experiments. |

## Cross-commit findings to close

1. Repair and gate the personal-AI ops tick; disable the unsupported local curriculum path.
2. Make MCP hosts reload an atomically replaced CNB generation and invalidate all roster/token caches without process recycling.
3. Replace the campaign false-green paths with strict, explicit result accounting and CPU-safe defaults.
4. Add an actual held-out JTC capability set that is not used for mining/certification and reports measured accuracy.
5. Eliminate newly introduced warning/format debt: C warning sites, managed warning flood policy, and the three CRLF source files changed in this window.
6. Expose per-Oracle provenance completeness while retaining the correct `descriptor_only_not_runtime_trust` boundary; never synthesize missing historical provenance.
7. Reconcile source continuation versus generated/runtime debris, preserve the live learner state, and finish on an intentional clean commit.
8. Close the pre-existing but audit-relevant MCP transport contradiction: the header promises a bounded command-free fetch, while Linux uses shell `popen` and accepts arbitrary HTTPS hosts.

## Adversarial second-pass defects

The first pass established commit boundaries. A second symbol-level pass added these release blockers; a green synthetic gate does not waive them:

- **DeepSeek/MLA:** reject incompatible GGUF tensor dimensions rather than silently choosing an orientation; remove fused-buffer aliasing in `build_layer_mla`; check every CNPK write; align int8/f32 accumulator semantics; free `idx`/`wt` on all exits; propagate expert-load failure.
- **GPU backends:** free file-static q8 caches on close; replace fixed resident-table exhaustion with bounded eviction or explicit failure telemetry; reject stream K-shape mismatch; never continue a CPU fallback with stale residual state. Real-model GPU parity remains evidence-required, not implied by tiny fixtures.
- **MTP:** restore `h->pos` after parallel draft walks; prevent random synthetic draft weights from being silently enabled on real hosts; label `CNET_MTP_SIM_LAUNCH` speedups as modeled, not measured.
- **Paged KV:** hash and compare the cold-file body, not its stored header digest; serialize ring mutation against raw-row readers; eliminate dead single-stage “double-buffer” state; bound qwen2 score spans when hot capacity is below logical context; avoid transient dense-KV allocation before pager activation.
- **MTK:** validate MTSK dimensions/count arithmetic before allocation or iteration; reject unsupported versions; provide a real pager clear on skill swap; avoid simultaneous HIP+OpenCL auto-attachment; restore caller-owned environment after sparse retry.
- **Benchmark integrity:** expected answers must be machine-scored; output buffers and tokenizer/model identity must be exact and bounded; build failures and explicitly required real-model absence are failures; CPU experiments must not seize reserved GPUs.
- **Repository state:** runtime memories, fact caches, gap journals, pager archives and backups are local state, not source. `cnet.so` is the deliberate exception: release policy intentionally versions the deterministic default shared library.

## Hypothesis

- H0: the July 18–19 work is only a large collection of locally green features; operations, serving freshness, capability quality, and release hygiene remain unreliable.
- H1: focused RED reproductions fail before repair, each named defect has a permanent gate, and one final umbrella run proves the integrated CPU-safe system without fabricated claims.
