# Bounded specialist product closure — 2026-09-06

Scope: source implementation in `feature/product-closure-20260906`, based on
`7c24204692a5b05e009627f43c98660bfcbea66d`. No files in main or the older experiment
worktree were changed; an existing main-checkout CPU harness was used read-only
for the explicitly scoped interoperability supplement. No live restart,
deployment selection, teacher-generation request,
GPU reset or remote push was performed. Final aggregate verification is being
recorded below; implementation evidence is not a public-release certificate.

## Delivered source boundaries

- Ordinary daemon resident sets use the existing capsule/core host, not a new
  package format. Initial/switch self-closure, incumbent-obligation replay for
  upgrades, immutable original payload/coverage/asset identity, monotonic revision
  checks, request pinning and terminal capsule refusal remain mandatory.
- The owner-only Unix control socket and native `cnet_capsulectl` are separate
  from ASK. Named sets cannot become arbitrary paths or chat commands. A
  mutation transport failure is explicitly an unknown outcome, not rollback.
- Private immutable snapshots and one explicit byte-encoded selection record
  support restart, exact latest-operation retry and rollback. Failed preparation
  retains serving. Post-rename sync uncertainty freezes mutations and retains
  the incumbent until restart reconciles a complete verified selection.
- Five literal local-source facts travel in the existing schema-2 asset, with
  exact source, extractor, actual grep receipt and decoder identities. Numeric
  output does not authorize arbitrary prose. Actual used hops and final output
  recheck freshness; static historical replay does not disable unrelated units
  when source becomes stale. This is not arbitrary compiler/test execution.
- The prior documentation-exposed SQLite, native harvest/package, distillation,
  scriptlet, managed server and deployment-helper limits received code changes,
  actual negative tests and independent review. See the scoped reports below.

Publication into an acquisition source set no longer silently reloads a running
daemon. Workers and owner activation are separate; existing acquisition gates
were migrated without deleting any held-out, exact-answer or OOD assertion.
The legacy daemon inventory variable now means startup-only resident loading.

## RED-first and independent review findings

Actual initial REDs recorded missing resident/store/snapshot APIs and missing
operator socket behavior before implementation. Follow-up negative tests exposed:

1. Candidate direct contracts can pass while a two-hop path contradicts them:
   initial/switch self-closure and complete upgrade replay reject that candidate.
2. Abandoned `selection-pending-*` files survived pre-rename process crashes:
   bounded recovery cleanup now removes only validated private orphan records.
3. Assetless source input routes could bypass source-evidence authority:
   both reserved source input and output interfaces now require bound assets.
4. Merged provenance descriptors reused a common name across different ports.
   Adding an earlier-sorted capsule changed a previous identity despite identical
   bytes. Identity now binds each original CNB payload, including its own
   provenance, plus the sealed unit/coverage/asset digests. Append-index and
   imported-name invariants are checked. The actual daemon reproducer failed
   before this repair and passed afterward; no identity field was dropped.
5. Independent review required root/effective-UID ancestor ownership and safe
   snapshot cleanup after publication. Synthetic UID tests make no real chown;
   filesystem corruption/crash cases use private fixtures only.

The actual source mutation-window test changes a fixture immediately after a
successful used-hop freshness check, through a test-only interposer. The final
receipt must refuse even when the terminal hop is numeric. Another test uses
identical numeric 0..4 codes with changed source labels: upgrade refuses,
explicit new-name switch succeeds, and rollback re-applies old freshness.

Independent author-separated reviews cleared the final bounded lifecycle,
source and client changes. Cross-model consultation was offered during sandbox
feasibility work but not invoked without confirmation. No Claude/Grok review is
claimed. Kernel and filesystem behavior were checked against primary Linux
documentation, including [fsync](https://man7.org/linux/man-pages/man2/fsync.2.html)
and [rename](https://man7.org/linux/man-pages/man2/rename.2.html): atomic visibility
is not acknowledged durability without parent-directory synchronization.

## Focused native verification

| Gate | Observed result |
| --- | --- |
| Resident host lifecycle | 27 checks, zero failures |
| Immutable snapshot boundary | 10 checks, zero failures |
| Durable store/restart/history | 31 checks, zero failures |
| Actual daemon adversarial recovery | 10 tests: 17 refused starts and 15 successful recoveries |
| Publication faults and actual child crashes | 40 checks, zero failures |
| Reserved source interface | 2 checks, zero failures after actual RED |
| Source evidence parser/acquisition | 53 C assertions and 6 producer tests |
| Native owner-control client | 9 scripted-socket tests |
| Actual daemon lifecycle/source/soak | 8 tests, including same-process source composition |
| Existing frontdoor/acquisition/composition | All original answer/refusal checks retained and GREEN |

`make capsule_product_sanitize` builds the complete linked C runtime, including
CCE C sources, in a new private directory without modifying normal object flags.
ASan/UBSan with leak detection passed the 27+10+31+40+2 focused checks. Evidence:
`/tmp/cnet-product-sanitize-gate-20260906.log`, artifacts
`/tmp/cnet-product-sanitize-TDGo1P`. Earlier private sanitizer runs are retained;
this is not an exhaustive sanitizer claim for every optional backend.

The first full `make verify` attempt exposed missing explicit Make references
for three new test sources. The next exposed two internal test callers needing
the new explicit live/static search argument. Both were repaired without
changing test expectations, budgets or certification floors; final aggregate
results are recorded after rerunning the gate.

## Small-fixture resource and performance evidence

The final focused daemon run used 20 successive named-set upgrades in one PID,
120 correct minute-conversion answers and 20 OOD refusals, then recovered
revision 22 after restart. At most three private cache snapshots remained.
RSS was 16,084 → 18,508 KiB; median stage+activate was 2.696 ms, maximum 3.152 ms.
These figures come from `/tmp/cnet-capsule-provenance-order-green-20260906.log` on
shared hardware. They are not a 4096-unit RAM or latency guarantee.

The final AMD product regression also passed against the repaired runtime:
`make -C experiments/offline_controller OUT=/tmp/cnet-closure-neural-regression-b171pP product-test`.
Receipt: `/tmp/cnet-product-gpu-final-20260906.log`; private evidence:
`/tmp/cnet-gpu-product-NtNasF`. Both devices completed real FP32 worker jobs;
cancellation/restart, fault injection, immutable transfer, guarded host
activation/rollback and direct FP32-to-BTN capsule conversion passed. The latter
tested eight domain rows, minimum margin 0.489318189 and prediction maximum
absolute difference 3.1082705e-08; weak-margin and OOD cases refused.

The serving benchmark ran three repeats per selector, each with 9,000 requests:
8,000 verified correct and 1,000 OOD refusals. Deterministic p50 was
2.635–2.644 µs versus 9.748–9.858 µs for the neural selector on this one-capsule
fixture. The deterministic default remains faster here. This measures serving,
not a new GPU-training speedup, broad task accuracy or large-inventory scaling.

Bounds: 4096 units and 32 GiB artifact bytes per snapshot; 64 MiB per CNB,
16 MiB per manifest/asset; four materialized generations; 64 pinned requests;
one staged candidate; 16,384 historical names. Materialized coverage, old pinned
generations and scratch add RAM. Admission can delay ASK in the serial daemon.
Cache collection removes private unselected copies, never source inventories or
historical identities. Unknown cache/state objects refuse instead of being
silently deleted. No hard real-time, physical power-cut, signed-origin or
malicious same-UID writer guarantee is claimed.

## Managed and auxiliary evidence

- [SQLite](sqlite_provider_closure_20260906.md): loaded 3.53.4, regression and
  separate transitive audit; old loaded 3.41.2 RED retained.
- [Native workflows](native_workflows_closure_20260906.md): actual native
  acquisition/package/smoke in private fixtures, no removed Python helpers.
- [Distillation](../docs/CNET_DISTILL.md): 25 negative/positive boundary tests,
  complete per-query anti-collapse receipts before teacher proposals.
- [Scriptlet isolation](scriptlet_isolation_closure_20260906.md): 49 focused
  tests and exact artifact publish checks, explicit Linux/runtime/resource scope.
- [Managed server](managed_server_closure_20260906.md): full bridge 426 passed,
  4 fixture-gated skips; control 80; server security/lifetime/UI 48; existing
  server 18. Those four native-memory tests subsequently passed (4/4, zero
  skips) using fingerprinted existing CPU artifacts, not a fresh native build.
  Real protected-browser proof and five project audits passed.
- [Private deployment handoff](deployment_handoff_closure_20260906.md): explicit
  trusted archive/digest/new destination, no live switch or data merge.

## Final aggregate checkpoint

`make -j2 verify` passed: all 28 required log suites were fresh and successful
against that run's sentinel, including the integrated authority dependencies.
Final receipt: `/tmp/cnet-product-verify-final-20260906.log`, now also including
the ten-case recovery module. Earlier successful receipt:
`/tmp/cnet-product-verify3-20260906.log`.

The final knowledge/security batch exited 0. Receipt:
`/tmp/cnet-product-knowledge-security-final-20260906.log`:

- Accumulation: 32/32 isolation; 8/8 replay and exact coverage; 8/8 pre-import
  unservable checks; 96 OOD cases. Composition: three independently certified
  members, coverage checked at root and intermediate hops.
- Coverage abstention: 55 checks and 4/4 held-out cases. Capsule transfer:
  94 checks, five coverage rows. No floor or held-out expectation was lowered.
- Learning health: 48 strict negative/positive fixture checks and separate
  configuration-only inspection. The existing gate also inspected the running
  lane read-only and checked its configured residual endpoint by TCP connect;
  no generation request was sent. That observed old deployment's health is not
  rollout acceptance for this new source.
- Distillation: 25 tests; native workflows passed outside the repository with
  fixture-only acquisition; deployment handoff: 18 tests; recovery: ten tests.
- ASI framing, execution-tier documentation, license metadata and strict claims
  checks passed.

Sequential frozen capability certification and final root commit identity are
pending. Live rollout remains separately authorized, not hidden as a passing
source test.
