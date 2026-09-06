# DS4 extraction record and current boundary

The original July survey mapped an external DwarfStar checkout onto CNET
inference/runtime needs. It included host-specific paths, measured streaming
experiments and rejected deployment cutover. Those observations are preserved
exactly in the [documentation archive](docs/MAINTENANCE.md); they are not
current external-checkout or live-service assertions.

CNET-side interfaces worth consulting are
[async_runtime.h](include/async_runtime.h),
[model_runtime.h](include/model_runtime.h) and the retained
[unified-runtime decision](plans/phase6_unified_runtime.md).
Model backends own tokenizer/KV/kernel details while the CNET runtime
governs placement, leases and evidence.

The earlier dual-device streaming speedup did not justify endpoint cutover:
its differently sized/modelled fallback served the probe much faster.
Keep that failed cutover comparison visible, including model mismatch.

Do not import external code, change services or port a vendor-specific kernel
as part of documentation maintenance. Any future extraction needs explicit
scope, provenance, compatible licensing, AMD support and an unchanged numeric/
acceptance gate. Use [GPU training](docs/GPU_TRAINING.md) for the separate
current resident-training experiment.
