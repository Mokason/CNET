# Execution tiers and backend boundaries

Build membership is not a capability certificate. The CCE aggregate, model
acceleration backends, capsule-serving library and experimental trainers have
different responsibilities and dependencies.

## Core aggregate

`$(CCE)` and [mk/cce_lib.mk](../mk/cce_lib.mk) define the shared CCE implementation
and link products. The aggregate includes tensor/block/cascade/forest/archive,
routing, training, model I/O and selected model kernels.

- `src/cce/cce_aicimo.c` is the canonical adapter router in the
  **core CCE aggregate**. Unprefixed historical names are header compatibility
  wrappers, not a second global ABI.
- `src/cce/cce_aicimo_bridge.c`, preservation and role-slice files remain
  experimental; they are not the canonical implementation.
- `src/cnet_lm.c` is a legacy training/generation path,
  **not in the core** aggregate. Explicit legacy/demo targets may still use it.

The local capsule library is assembled in [mk/authority.mk](../mk/authority.mk).
It adds certified inventory/query/publication-facing APIs and the opt-in core
candidate host without making the neural path the default.

## AMD acceleration

| Surface | Contract |
| --- | --- |
| Generic tensor device API | `cce_tensor` device abstraction; default CPU context |
| OpenCL model kernels | Separate row-major/device-cache API via `cce_clgemm` |
| HIP/rocBLAS resident trainer | Explicit device ownership, private streams and FP32 updates |
| AMD math library | Bounded device-pointer matrix/update functions |
| Managed bounded offload | Native model/session backend plus explicit resource policy |

These are not interchangeable tensor layouts. `CceModel.UseDevice` does not
implicitly expose OpenCL; reserved unsupported device values must refuse.
Historical CUDA implementation files describe legacy vendor-specific surfaces;
they are not the supported AMD product workflow. See [GPU training](GPU_TRAINING.md).

The resident trainer proves GPU forward and updates for its fixed workload.
It does not provide every model architecture with a training backend. The shared
cell and the larger recurrent trainer have separate tests and measurements.
Failed precision gates stay failed.

## Verification tiers

```sh
make alt_paths_gate execution_tiers_doc_gate
make aicimo_smoke cce_smoke
make verify-fast
make verify
make verify-t2
```

The alternate-path gate checks canonical AICIMO symbols, rejects legacy leakage
and distinguishes generic device initialization from model-kernel APIs.
[membership](../mk/verify_tiers.mk) defines the source verification tiers.

Use-loop targets such as `cnet_use_loop_acceptance`, `serve_feedback` and
`oracle_teacher_runtime` compose runtime components; they are not another
execution backend. Private checkpoints, GPUs and external teachers require
their own evidence. [Build guide](BUILD_AND_TEST.md), [architecture](ARCHITECTURE.md).
