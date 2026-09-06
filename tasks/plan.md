# Current CNET work plan

This file is the current task queue, not a replay of old implementation steps.
Decision history remains in [plans/](../plans/). Current documentation starts at
[docs/INDEX.md](../docs/INDEX.md).

## Completed product source checkpoint

The owner authorized all remaining product and documentation-exposed code work.
The ordered acceptance/verification contract is
[product closure](../plans/cnet_product_closure_20260906.md), starting from
`7c24204` in the private `feature/product-closure-20260906` worktree.
SQLite and native workflow repair are independent; lifecycle, daemon control,
persistence, evidence assets and integrated proof are sequential. Live rollout
still requires an explicit target/configuration approval.

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

## Product closure status

SQLite provider remediation, native harvest/package migration, anti-collapse
distillation, resident host/control/store, bounded source assets, managed
scriptlet isolation and server security/lifetime/UI repairs are implemented.
Focused negative tests and independent reviews are recorded in their dated
`result/*closure_20260906.md` reports. The explicit private deployment helper
passes interrupted-publication and unsafe-archive tests. Final native regression
passes all 28 required fresh-log suites, including the new lifecycle/recovery
dependencies. Sequential frozen capability certification passed all six
existing manifests at clean commit `6068a8c`, without source drift or altered
floors. The completed implementation and evidence handoff are in the
[integrated report](../result/cnet_product_closure_20260906.md).

The source domain is five exact local literal facts, not arbitrary source
understanding or unrestricted compiler/test execution. Approved arithmetic-tool
acquisition remains independently labelled; publication needs explicit owner
activation. A live rollout requires a selected target/configuration, separately
authorized operations and fresh deployment-specific verification.

The governing lifecycle requirements remain in
[capsule hot-swap plan](../plans/cnet_capsule_hot_swap_20260906.md).
The deterministic 4096-capsule capacity gate remains separate and passed again;
the experimental neural adapter remains capped at 62 capsules.

No old ownership assignment, PID, endpoint availability or handoff checkbox
grants current authority. Do not restart services or overwrite live state based
on a historical plan.
