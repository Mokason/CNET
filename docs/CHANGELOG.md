# Source and evidence changelog

Entries describe bounded repository changes. They are not live-deployment
attestations or a list of claims automatically revalidated at each release.

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
The default daemon still reloads the inventory per capsule request;
the durable resident lifecycle remains in [the task queue](../tasks/todo.md).

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
