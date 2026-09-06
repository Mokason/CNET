# Current CNET work plan

This file is the current task queue, not a replay of old implementation steps.
Decision history remains in [plans/](../plans/). Current documentation starts at
[docs/INDEX.md](../docs/INDEX.md).

## Completed bounded source work

The September 6 GPU product sequence implements resident AMD FP32 training,
immutable snapshots, a bounded shared-cell selector, canonical core candidates,
direct fitted-weight BTN/capsule conversion, guarded in-memory activation with
request pinning/rollback, and isolated two-device workers. Fresh source and
capability gates passed at the recorded checkpoint. This is not a live rollout.

Evidence and limitations:
[result report](../result/cnet_gpu_product_sequence_20260906.md),
[execution ledger](../plans/cnet_gpu_product_execution_20260906.md).

## Completed documentation maintenance

Rewrote 96 existing guides and added four current references; retired 47
superseded files into a verified archive while preserving 366 original
Markdown/text inputs and records. Two links in one historical spec changed.
Native regression and bounded managed/documentation checks passed.
[Cleanup evidence](../result/documentation_cleanup_20260906.md).

## Open product scope

1. Remediate the managed control-plane SQLite dependency advisory, verifying
   the actual native provider and all ingestion/activation behavior.
2. Complete the ordinary daemon's resident capsule lifecycle: named operator
   selection, same-process swaps, durable activation, restart and rollback.
   The new private in-memory core host is a building block, not completion of
   this daemon/deployment scope.
3. Add local source-evidence acceptance through existing manifest-bound assets,
   then integrate acquisition, swapping and regression checks.
4. Perform any live rollout only with explicit operator authority and fresh
   deployment-specific evidence.

The governing lifecycle requirements remain in
[capsule hot-swap plan](../plans/cnet_capsule_hot_swap_20260906.md).
The deterministic 4096-capsule capacity gate is already separate and complete;
the experimental neural adapter remains capped at 62 capsules.

No old ownership assignment, PID, endpoint availability or handoff checkbox
grants current authority. Do not restart services or overwrite live state based
on a historical plan.
