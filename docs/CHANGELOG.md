# Source and evidence changelog

Entries describe bounded repository changes. They are not live-deployment
attestations or a list of claims automatically revalidated at each release.

## 2026-09-09 — Verified composition reuse

Added explicit export of the capsule dependencies selected by a verified request.
Original capsule bytes/assets are preserved; staged identity and guarded replay
checks precede atomic publication. Two-hop hours/minutes/seconds reuse, coverage
misses, unused-capability exclusion and retained source freshness have native
regression tests. No new package format, training labels or live activation.
[Operator guide](COMPOSITION_REUSE.md), [measured scope](../result/cnet_composition_reuse_20260909.md).

## 2026-09-08 — Bounded MCP read brick

Added a portable two-action dispatch capsule, strict native MCP evidence
validation, opt-in public Wikipedia/page reads and terminal daemon routing.
Network policy blocks private destinations, redirects and oversized responses;
retrieved text never becomes a certified answer or automatic training label.
131 managed tests, native safety gates and a real Wikipedia integration pass.
[Operator guide](MCP_READ_BRICK.md), [measured scope](../result/cnet_mcp_read_brick_20260908.md).
Live deployment, recursive crawling and arbitrary MCP tools are not included.

## 2026-09-06 — Bounded product closure

The ordinary daemon now retains pinned capsule working sets with owner-only
control, guarded upgrade versus explicit switch, immutable snapshots and durable
activation/restart/rollback. The existing schema-2 asset carries five bounded
source facts, with original payload/receipt/decoder identities and used-hop/final
freshness refusal. Existing source-set publication is now separate from serving
activation; migrate legacy per-request reload workflows explicitly.

Documentation-exposed code fixes include the loaded SQLite provider, native
harvest/package workflows, verified anti-collapse distillation, isolated managed
scriptlets, and managed server authentication/resource/model-lifetime controls.
The opt-in browser page is offline, chat-only and plain-text. Operator procedures
and remaining live authority are in [the current plan](../tasks/plan.md).

## 2026-09-06 — Documentation cleanup

Current guides were rewritten against source: capsule/core/MTK boundaries,
AMD training, daemon/web authority, managed library loading, scriptlet limits,
build entrypoints and remaining product/security scope.
Superseded implementation transcripts and handoffs moved to a recoverable
archive; runtime text inputs and frozen evidence were preserved.

The existing `asi_framing` check searched for a corrupted dash. A RED run
reproduced it; the check now matches the unchanged canonical UTF-8 sentence.

See [cleanup decision and verification](../plans/documentation_cleanup_20260906.md)
and [recovery](MAINTENANCE.md).

## 2026-09-06 — GPU candidate experiment

The bounded resident FP32 trainer, trusted Linux worker boundary, direct
weight conversion and guarded private core activation are recorded in
[the product report](../result/cnet_gpu_product_sequence_20260906.md).
It includes per-device timing, precision checks, negative controls and
explicit limits. Deterministic BFS remains the default; the small neural
adapter is opt-in, not a general task-quality replacement.

## 2026-09-05–06 — Capsule product sequence

Dated reports cover [implementation](../result/cnet_implementation_20260905.md),
[live acquisition observations](../result/cnet_live_acquisition_20260906.md),
[composition](../result/cnet_composed_acquisition_20260906.md) and
[4096-entry capacity](../result/cnet_capsule_capacity_4096_20260906.md).
Those earlier reports predate the resident lifecycle above; their then-current
per-request reload behavior is historical, not the current daemon contract.

## Earlier history

The previous complete changelog, including withdrawn/corrected classification
claims and vision measurements, is retained byte-for-byte in the
[documentation archive](MAINTENANCE.md). Also retain:

- [Project history](cnet-history.md).
- [Vision V2 protocol/results](../plans/cnet_vision_object_detection_v2_20260727.md).
- [Vision portability scope](../plans/cnet_vision_portable_specialist_20260728.md).
- [Phase evidence taxonomy](phase123_benchmark_closure.md).
- [Frozen benchmark generations](../benchmarks/).

Do not replace a failed historical result with a later successful experiment
or rewrite a spent holdout as fresh.
