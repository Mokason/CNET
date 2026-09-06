# Capsule hot-swap and local codebase evidence

Status: lifecycle implementation pending; larger resident inventory capacity
requested and implemented separately, with measured gates recorded in
`plans/cnet_capsule_capacity_4096_20260906.md`.

The operator approved learning CNET codebase knowledge from local source and
verified compiler/test results, and requested capsule swapping to reload
knowledge on the fly. This extends the certified capsule runtime, not MTK
weight-delta patching. Existing CNB/CNU1 + capsule manifests remain the only
knowledge packaging format.

## Current behavior and boundary

`tools/cnetd.c` currently opens and closes the entire configured capsule root
for each typed request. Additions are visible without restart, but there is no
resident active-set lifecycle, explicit generation switch, or retained fallback.
`cnet_capsule_core_open` validates imports; `validate_growth` replays sealed
labels and guarded joins. The existing `cnet_swap_admit` law separately governs
certified replacement and must not be bypassed by relabeling replacement as
an upgrade.

Two operations have different contracts:

- Active-set switch: explicitly choose a working set. An unloaded capability
  may abstain until its set is loaded again; stored knowledge is not deleted.
- Upgrade/reload: retain the current certified obligations. Changed answers,
  lost coverage, failed compositions or exhausted proof budgets refuse.

Do not describe an active-set switch as a measured improvement. Switching must
not silently replace a same-identity certified unit with conflicting behavior.

## Ordered slices

### 1. Resident lifecycle

Files: `include/cnet_capsule_core.h`, a capsule-runtime implementation, focused
native tests, and `mk/authority.mk`.

Acceptance:
- Load a complete validated candidate before changing the active pointer.
- Failed load/reload preserves the active generation and its answers.
- A request executes entirely within one generation, with every-hop coverage.

Initial activation and set switches must run candidate self-closure validation
(`validate_growth(candidate, candidate)`), not only `open`: direct-contract
checks alone do not catch contradictory indirect paths. Reload also replays
incumbent obligations. Test a valid direct answer contradicted by a two-hop
path. Explicit unload keeps the capsule-intent boundary active so a refusal
cannot fall through to uncertified residual prose.

Verification: actual RED first; load, reload, invalid candidate, OOD, unload,
generation and teardown tests; focused address/undefined-behavior checks.
Default policy: retain accumulated capsules when capacity permits; explicit
operator set switching may unload a working set without deleting stored data.

### 2. Operator control and daemon integration

Files: daemon, protocol/control adapter, and real-socket tests.

Acceptance:
- Explicit named-set activation/reload/status works without daemon restart.
- Ordinary chat cannot supply paths or select privileged control operations.
- Old requests remain coherent; failed candidates are visible refusals, not
  silent updates or falsely reported active generations.

Verification: same-PID socket test before/after switch; invalid name/path,
corruption, stale generation and failed-upgrade negatives; existing front-door
and acquisition gates. No threading claim beyond the serial daemon contract.
Dependency: slice 1.

### 3. Persistence and rollback

Files: activation persistence helper/CLI, fault-injection tests, documentation.

Acceptance:
- Durable activation records select a validated generation, not half a set.
- Failed publication leaves a usable current generation and reports failure.
- Restart restores the selected set; rollback preserves stored knowledge.

Verification: crash/failure boundaries, identical retry, restart and rollback;
source/candidate directory mutation and symlink refusal tests.
Dependency: slice 2. Checkpoint: lifecycle/control/recovery review before live
configuration changes.

### 4. Source-grounded CNET knowledge

Files: bounded extractor, source-evidence schema/asset validator, test fixtures.

Acceptance:
- Extract only allowlisted local source facts, with source/content identity;
  source text is data, never executable instructions or self-answer evidence.
- Use the existing manifest-bound asset for provenance and decoding metadata;
  numeric BTN output alone must not certify an arbitrary prose answer.
- Changed source invalidates current-fact claims until reacquisition. Compiler
  and test claims require actual bounded verified tool receipts.

Verification: independent fixtures, changed/deleted source, malformed evidence,
path escapes, unsupported questions and prompt-like source comments.
Dependency: capsule lifecycle and an asset-aware certified consumer. A source
symbol index is not evidence of broad code understanding.

The current core intentionally uses schema-1-only import. Supporting evidence
assets requires explicit asset-schema dispatch, semantic validation and
snapshot-owned asset lifetime/freeing. Bind each fact to source/extractor
digests; bind compiler/test receipts to configuration and tested revision.
Accepting an asset and discarding its contents is forbidden. Keep source text
as data; it must never select commands, trusted paths or control operations.

### 5. Integrated proof and deployment

Acceptance:
- Learn a bounded local-source fact set, activate it, switch away/back, and
  reject stale/corrupt evidence through the actual daemon.
- Preserve existing approved-tool acquisition and all certification floors.
- Run full regression, capability certification and live checks with rollback.

Verification: focused gates, `make verify`, sequential `make capability_cert`,
knowledge accumulation/composition, same-PID hot-swap demo, deployed health.
Dependency: slices 1–4. Review and record exact measured scope before release.

## Threats and non-goals

Control-plane requests are not chat intent. Filesystem ownership, immutable
snapshot lifetime, source freshness, generation compare-and-swap, active reader
ownership, asset schema validation and candidate consistency require tests.
Checksums remain corruption detection, not origin authentication.

Fresh-context design review identified five actionable safeguards: candidate
self-closure proof, asset-aware ownership, semantic evidence validation,
operator control separated from ASK text, and stable immutable publication
with explicit unloaded/startup/reload states. All are part of acceptance above;
none is claimed implemented. External cross-model review has been offered,
but no external CLI has been authorized or invoked.

No new external teacher, arbitrary source execution, new packaging format,
certification-floor relaxation, or unmeasured broader-capability claim.
