# CNET architecture

> **CNET builds ASI — Artificial Specialized Intelligence. In CNET, ASI never
> means Artificial Superintelligence. CNET is not claiming AGI. Broad semantic
> grounding exists to understand intent; deep competence comes from isolated,
> certified, portable Micro Tensor Kernels. Wikipedia-like accumulation may
> broaden coverage and composition over time, but unit count alone is not
> intelligence and all broader claims remain benchmark-gated.**

This guide describes the source architecture. It is not a deployment inventory
or a substitute for benchmark receipts. Start at [INDEX.md](INDEX.md) for tasks;
use [execution tiers](EXECUTION_TIERS.md) to distinguish supported and legacy
build surfaces.

## Authority and data flow

```text
request → intent proposal → verified registry → plan proposal
                                               ↓
                             strict execution at every hop
                             identity + contract + coverage
                                               ↓
                                verified result / abstention

miss → external evidence → candidate fit → certification → growth replay
                                                      → explicit publication
```

Intent extraction, learned scores, retrieved memories and external model prose
are proposals. They do not certify themselves. A plausible path is insufficient
if one intermediate value falls outside its capsule's coverage.

The anti-collapse rule applies to every learning lane: labels come from an
external teacher, a user correction or a verified tool result. Replaying sealed
labels to check a candidate is evaluation, not permission to train on CNET's
own Tier-A answers.

## Types and persistence

| Object | Responsibility | Source contract |
| --- | --- | --- |
| BTN | A learned transform with explicit input/output ports | [nn.h](../include/nn.h) |
| Specialist | Planner-visible implementation admitted under one typed contract | [specialist.h](../include/specialist.h) |
| Contract / sealed unit | Exemplars, certification and behavior identity | [contract.h](../include/contract/contract.h), [base.h](../include/base.h) |
| Certified capsule | Portable subset plus manifest, coverage and optional bound asset | [cnet_capsule.h](../include/cnet_capsule.h) |
| MTK cartridge | Host-model tensor-site deltas, requiring host compatibility | [cce_mtk.h](../include/cce/cce_mtk.h) |
| Model catalog | Resources, residency, loading and generation leases | [model_runtime.h](../include/model_runtime.h) |
| Core checkpoint | Versioned, unapproved shared-cell weights and evidence hashes | [cnet_core_candidate.h](../include/cnet_core_candidate.h) |

Capsules extend the existing CNB subset export/import path. Do not introduce
another capsule package to carry model weights. A core checkpoint is not a
capsule: conversion must still pass the existing BTN contract and capsule gates.
Content hashes detect mismatches; they are not signatures or teacher
authentication.

## Certified planning and execution

The generic registry/planner supports native BTN, CCE-backed and admitted oracle
specialists. Recall inside a CCE forest stays inside that specialist's contract;
cross-specialist selection belongs to the certified planner.
[Dispatch](dispatch.md) explains this separation.

The local capsule core builds a complete verified inventory before accepting it.
Its ordinary search follows typed-port and actual-value states; coverage is
checked at every hop. Search, replay and inventory sizes are bounded. A corrupt
member or conflicting identity refuses the new inventory rather than partially
loading it.

The deterministic library is single-threaded and admits up to 4096 capsules.
The daemon retains request-pinned generations. Owner-only named-set staging,
self-closure/upgrade replay, immutable snapshots and durable monotonic selection
provide same-process activation, restart and rollback. Publication into a source
set does not activate it. See [the operator contract](CAPSULE_CORE.md).
The explicit source-evidence asset schema adds fixed labels and live freshness
without allowing unvalidated prose or replacing the existing capsule format.

## Experimental neural selection and activation

A shared 3-input, 8-hidden-unit, 1-output cell supplies local updates over a
verified typed graph. The same 41 FP32 parameters are applied at every node for
up to 64 iterations. Identity is metadata, not an arbitrary numeric feature.
The implementation learns a supplied finite propagation rule; it does not learn
language semantics or discover the rule autonomously.

The opt-in adapter admits 62 capsules plus start/goal nodes. It proposes a path
and executes it through the ordinary audit, contract and coverage checks, with
no fallback that converts a refused neural answer into a success.

The owner API in [cnet_core_host.h](../include/cnet_core_host.h) stages candidates,
binds independent graph/shadow evidence, replays all sealed-label joins through
the actual proposed serving path, and permits explicit activation. Four loaded
generations and 64 request leases are allowed. Pinned requests retain both old
weights and old inventory; rollback invalidates pending approval.

This wrapper serializes the legacy BTN/certification state globally. Do not call
raw legacy APIs concurrently outside it. The owner stops new operations before
destruction; a pinned host cannot be destroyed. Activation/rollback are in-memory
operations, not durable service deployment. [GPU training](GPU_TRAINING.md)
documents the complete experimental boundary.

## CCE and acceleration

CCE provides tensors, blocks, cascades, forests, archive views, routing and
training. These are implementation mechanisms; having a model in a forest does
not confer the external specialist certificate.

CPU execution remains available. Optional AMD backends have different tensor
contracts and are not interchangeable simply because each performs matrix
multiplication:

- OpenCL model kernels operate on their own device resources and model layouts.
- HIP/rocBLAS and the AMD math library provide explicit FP32 matrix/update paths.
- The resident controller owns bounded model/scratch allocations and a private
  stream; snapshots are copies, not borrowed mutable weights.
- The shared-cell worker is a separate tiny task with separate performance and
  accuracy evidence.

The old claim that training is possible only through the OpenCL model path is
obsolete: the resident HIP/rocBLAS experiment now performs forward and updates.
This does not imply every model architecture supports that trainer.
Reduced-precision experiments that fail parity remain unadmitted.

## Learning and memory

The base/registry path, gap lane and teacher runtime are distinct from text
retrieval and presentation:

- The gap lane records uncovered typed requests and seeks independently sourced
  labels under its configured policy.
- Teacher descriptors carry provenance. A live binding and successful
  certification are still required for trusted execution.
- Shared-workspace and semantic-cortex entries can hold proposals without
  promoting them to certified answers.
- Tile/episodic memory, ROE packs and response templates are not automatically
  certified numerical capsules.
- KV caches belong to a model/weight epoch; stored text and capsule evidence
  have different invalidation rules.

See [teacher acquisition](TEACH_PATH.md), [memory](MEM_RUNTIME.md),
[weight epochs](WEIGHT_EPOCH.md) and [capsule operations](CAPSULE_CORE.md).
Historical design specs may describe deferred behavior; [maintenance](MAINTENANCE.md)
records their status and known implementation corrections.

## Process and integration boundaries

`cnetd` exposes the local socket front door; .NET/MCP, web and scheduler layers
are consumers of native authority, not replacements for it. Optional network
teacher calls require their own policy, identity and error handling.
[The managed inference harness](cnet_dotnet_inference_harness.md) is a separate
model/session API with bounded offload and context processing.

The GPU worker pool limits two jobs, one per discrete device, and uses sealed
snapshot input plus exact-length private output. Filesystem mutation is limited
by Landlock; network and process restrictions cover worker threads after trusted
HIP warm-up. The GPU runtime/driver remains trusted. Read access, total runtime
RSS and total scratch consumption are not fully isolated. See [security](SECURITY.md).

## Verification boundaries

`make knowledge_accumulation_bench` checks isolation, interference, portable
round-trip and refusal. `make knowledge_composition_bench` checks a small typed
composition mechanism. `make capability_cert` measures its declared held-out
manifests; it does not validate every experimental subsystem.

The current GPU [result report](../result/cnet_gpu_product_sequence_20260906.md)
separates the larger training benchmark, six graph-suite models, and the worker
demonstration model. Failed controls and open security findings remain visible.
Broader unmeasured capability is WITHHELD.
